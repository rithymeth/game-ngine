#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <minidumpapiset.h>

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

struct MemoryRange {
    DWORD64 address = 0;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

struct DumpView {
    HANDLE mapping = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::size_t file_size = 0;
    std::uint8_t* base = nullptr;
    std::vector<MemoryRange> memory;

    ~DumpView() {
        if (base) UnmapViewOfFile(base);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }

    const void* At(ULONG32 rva, std::size_t size) const {
        if (!base || rva > file_size || size > file_size - rva ||
            size > std::numeric_limits<ULONG32>::max() ||
            rva > std::numeric_limits<ULONG32>::max() - static_cast<ULONG32>(size))
            return nullptr;
        return base + rva;
    }

    bool Read(DWORD64 address, void* destination, std::size_t size) const {
        auto* out = static_cast<std::uint8_t*>(destination);
        while (size) {
            const auto it = std::find_if(memory.begin(), memory.end(), [&](const MemoryRange& range) {
                return address >= range.address && address - range.address < range.size;
            });
            if (it == memory.end()) return false;
            const std::size_t offset = static_cast<std::size_t>(address - it->address);
            const std::size_t count = std::min(size, it->size - offset);
            std::memcpy(out, it->data + offset, count);
            address += count;
            out += count;
            size -= count;
        }
        return true;
    }
};

DumpView* g_dump = nullptr;

BOOL CALLBACK ReadMemory(HANDLE, DWORD64 address, PVOID buffer, DWORD size, LPDWORD read) {
    if (!g_dump || !g_dump->Read(address, buffer, size)) {
        if (read) *read = 0;
        return FALSE;
    }
    if (read) *read = size;
    return TRUE;
}

PVOID CALLBACK FunctionTable(HANDLE process, DWORD64 address) {
    return SymFunctionTableAccess64(process, address);
}

DWORD64 CALLBACK ModuleBase(HANDLE process, DWORD64 address) {
    return SymGetModuleBase64(process, address);
}

const wchar_t* Architecture(USHORT arch) {
    switch (arch) {
    case PROCESSOR_ARCHITECTURE_AMD64: return L"x64";
    case PROCESSOR_ARCHITECTURE_INTEL: return L"x86 (not supported by this build)";
    case PROCESSOR_ARCHITECTURE_ARM64: return L"ARM64 (not supported by this build)";
    default: return L"unknown";
    }
}

std::wstring DumpString(const DumpView& dump, ULONG32 rva) {
    const auto* length = static_cast<const ULONG32*>(dump.At(rva, sizeof(ULONG32)));
    if (!length || (*length % sizeof(wchar_t)) != 0 || rva > std::numeric_limits<ULONG32>::max() - sizeof(ULONG32)) return {};
    const auto* data = static_cast<const wchar_t*>(dump.At(rva + sizeof(ULONG32), *length));
    if (!data) return {};
    return std::wstring(data, *length / sizeof(wchar_t));
}

bool LoadMemoryRanges(DumpView& dump) {
    void* stream = nullptr;
    ULONG stream_size = 0;
    if (MiniDumpReadDumpStream(dump.base, MemoryListStream, nullptr, &stream, &stream_size) &&
        stream_size >= offsetof(MINIDUMP_MEMORY_LIST, MemoryRanges)) {
        const auto* list = static_cast<const MINIDUMP_MEMORY_LIST*>(stream);
        if (list->NumberOfMemoryRanges > (stream_size - offsetof(MINIDUMP_MEMORY_LIST, MemoryRanges)) / sizeof(MINIDUMP_MEMORY_DESCRIPTOR)) return false;
        const auto* entries = reinterpret_cast<const MINIDUMP_MEMORY_DESCRIPTOR*>(list + 1);
        for (ULONG32 i = 0; i < list->NumberOfMemoryRanges; ++i) {
            const auto& entry = entries[i];
            const auto* bytes = static_cast<const std::uint8_t*>(dump.At(entry.Memory.Rva, entry.Memory.DataSize));
            if (bytes) dump.memory.push_back({entry.StartOfMemoryRange, bytes, entry.Memory.DataSize});
        }
    }

    stream = nullptr;
    stream_size = 0;
    if (MiniDumpReadDumpStream(dump.base, Memory64ListStream, nullptr, &stream, &stream_size) &&
        stream_size >= offsetof(MINIDUMP_MEMORY64_LIST, MemoryRanges)) {
        const auto* list = static_cast<const MINIDUMP_MEMORY64_LIST*>(stream);
        if (list->NumberOfMemoryRanges > (stream_size - offsetof(MINIDUMP_MEMORY64_LIST, MemoryRanges)) / sizeof(MINIDUMP_MEMORY_DESCRIPTOR64)) return false;
        const auto* entries = reinterpret_cast<const MINIDUMP_MEMORY_DESCRIPTOR64*>(list + 1);
        ULONG64 rva = list->BaseRva;
        for (ULONG64 i = 0; i < list->NumberOfMemoryRanges; ++i) {
            const auto& entry = entries[i];
            if (rva <= std::numeric_limits<ULONG32>::max() && entry.DataSize <= std::numeric_limits<std::size_t>::max()) {
                const auto* bytes = static_cast<const std::uint8_t*>(dump.At(static_cast<ULONG32>(rva), static_cast<std::size_t>(entry.DataSize)));
                if (bytes) dump.memory.push_back({entry.StartOfMemoryRange, bytes, static_cast<std::size_t>(entry.DataSize)});
            }
            rva += entry.DataSize;
        }
    }
    std::sort(dump.memory.begin(), dump.memory.end(), [](const MemoryRange& a, const MemoryRange& b) {
        return a.address < b.address;
    });
    return !dump.memory.empty();
}

std::wstring SymbolSearchPath(const std::filesystem::path& dump_path, const std::filesystem::path& symbols_path) {
    std::wstring path = symbols_path.empty() ? dump_path.parent_path().wstring() : symbols_path.wstring();
    const wchar_t* existing = _wgetenv(L"_NT_SYMBOL_PATH");
    if (existing && *existing) {
        if (!path.empty()) path += L';';
        path += existing;
    }
    return path;
}

void PrintFrame(std::wostream& output, HANDLE process, const STACKFRAME64& frame) {
    const DWORD64 address = frame.AddrPC.Offset;
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 1024] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 1023;
    DWORD64 displacement = 0;
    if (SymFromAddr(process, address, &displacement, symbol)) {
        output << L"  " << symbol->Name << L" + 0x" << std::hex << displacement;
    } else {
        output << L"  0x" << std::hex << address << L" (symbol unavailable)";
    }
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD line_displacement = 0;
    if (SymGetLineFromAddr64(process, address, &line_displacement, &line))
        output << L" at " << line.FileName << L':' << std::dec << line.LineNumber;
    output << L'\n';
}

bool WalkThread(const DumpView& dump, const MINIDUMP_THREAD& thread, USHORT architecture,
                HANDLE process, std::wostream& output) {
    if (architecture != PROCESSOR_ARCHITECTURE_AMD64 || thread.ThreadContext.DataSize < sizeof(CONTEXT)) return false;
    const auto* context_data = dump.At(thread.ThreadContext.Rva, thread.ThreadContext.DataSize);
    if (!context_data) return false;
    CONTEXT context{};
    std::memcpy(&context, context_data, sizeof(context));

    STACKFRAME64 frame{};
    const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    output << L"Thread " << thread.ThreadId << L"\n";
    for (unsigned depth = 0; depth < 128 && frame.AddrPC.Offset; ++depth) {
        PrintFrame(output, process, frame);
        if (!StackWalk64(machine, process, GetCurrentThread(),
                         &frame, &context, &ReadMemory, &FunctionTable, &ModuleBase, nullptr))
            break;
    }
    return true;
}

int Symbolicate(const std::filesystem::path& dump_path, const std::filesystem::path& symbols_path,
                const std::filesystem::path& output_path) {
    DumpView dump;
    dump.file = CreateFileW(dump_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump.file == INVALID_HANDLE_VALUE) {
        std::wcerr << L"Could not open minidump: " << dump_path << L" (Win32 " << GetLastError() << L")\n";
        return 1;
    }
    LARGE_INTEGER mapped_file_size{};
    if (!GetFileSizeEx(dump.file, &mapped_file_size) || mapped_file_size.QuadPart <= 0 ||
        static_cast<ULONGLONG>(mapped_file_size.QuadPart) > std::numeric_limits<std::size_t>::max()) {
        std::wcerr << L"Invalid or oversized minidump file.\n";
        return 1;
    }
    dump.file_size = static_cast<std::size_t>(mapped_file_size.QuadPart);
    dump.mapping = CreateFileMappingW(dump.file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!dump.mapping) {
        std::wcerr << L"Could not create minidump mapping (Win32 " << GetLastError() << L")\n";
        return 1;
    }
    dump.base = static_cast<std::uint8_t*>(MapViewOfFile(dump.mapping, FILE_MAP_READ, 0, 0, 0));
    if (!dump.base) {
        std::wcerr << L"Could not map minidump (Win32 " << GetLastError() << L")\n";
        return 1;
    }

    MINIDUMP_SYSTEM_INFO* system = nullptr;
    ULONG system_size = 0;
    if (!MiniDumpReadDumpStream(dump.base, SystemInfoStream, nullptr, reinterpret_cast<void**>(&system), &system_size) ||
        !LoadMemoryRanges(dump)) {
        std::wcerr << L"The input is not a readable minidump with captured memory.\n";
        return 1;
    }
    const USHORT architecture = system->ProcessorArchitecture;
    if (architecture != PROCESSOR_ARCHITECTURE_AMD64) {
        std::wcerr << L"Unsupported dump architecture: " << Architecture(architecture) << L"\n";
        return 1;
    }

    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    const std::wstring search = SymbolSearchPath(dump_path, symbols_path);
    if (!SymInitializeW(process, search.c_str(), FALSE)) {
        std::wcerr << L"Could not initialize DbgHelp (Win32 " << GetLastError() << L")\n";
        return 1;
    }
    struct SymCleanup { HANDLE process; ~SymCleanup() { ::SymCleanup(process); } } cleanup{process};

    void* stream = nullptr;
    ULONG stream_size = 0;
    if (!MiniDumpReadDumpStream(dump.base, ModuleListStream, nullptr, &stream, &stream_size)) {
        std::wcerr << L"The minidump does not include a module list.\n";
        return 1;
    }
    if (stream_size < offsetof(MINIDUMP_MODULE_LIST, Modules) ||
        static_cast<const MINIDUMP_MODULE_LIST*>(stream)->NumberOfModules >
            (stream_size - offsetof(MINIDUMP_MODULE_LIST, Modules)) / sizeof(MINIDUMP_MODULE)) {
        std::wcerr << L"The minidump module list is truncated.\n";
        return 1;
    }
    const auto* modules = static_cast<const MINIDUMP_MODULE_LIST*>(stream);
    for (ULONG32 i = 0; i < modules->NumberOfModules; ++i) {
        const auto& module = modules->Modules[i];
        const std::wstring image = DumpString(dump, module.ModuleNameRva);
        const std::wstring name = std::filesystem::path(image).filename().wstring();
        if (module.BaseOfImage && module.SizeOfImage)
            SymLoadModuleExW(process, nullptr, image.empty() ? nullptr : image.c_str(), name.c_str(),
                             module.BaseOfImage, module.SizeOfImage, nullptr, 0);
    }

    std::wofstream output(output_path, std::ios::trunc);
    if (!output) {
        std::wcerr << L"Could not create symbolicated report: " << output_path << L"\n";
        return 1;
    }
    output << L"Aether symbolicated crash dump\nDump: " << dump_path << L"\nArchitecture: "
           << Architecture(architecture) << L"\nSymbol search path: " << search << L"\n\n";
    if (!MiniDumpReadDumpStream(dump.base, ThreadListStream, nullptr, &stream, &stream_size)) {
        std::wcerr << L"The minidump does not include a thread list.\n";
        return 1;
    }
    if (stream_size < offsetof(MINIDUMP_THREAD_LIST, Threads) ||
        static_cast<const MINIDUMP_THREAD_LIST*>(stream)->NumberOfThreads >
            (stream_size - offsetof(MINIDUMP_THREAD_LIST, Threads)) / sizeof(MINIDUMP_THREAD)) {
        std::wcerr << L"The minidump thread list is truncated.\n";
        return 1;
    }
    const auto* threads = static_cast<const MINIDUMP_THREAD_LIST*>(stream);
    g_dump = &dump;
    unsigned walked = 0;
    for (ULONG32 i = 0; i < threads->NumberOfThreads; ++i)
        if (WalkThread(dump, threads->Threads[i], architecture, process, output)) ++walked;
    g_dump = nullptr;
    output.close();
    if (!walked || !output) {
        std::wcerr << L"No thread stacks could be read from the dump.\n";
        return 1;
    }
    std::wcout << L"Wrote " << output_path << L" (" << walked << L" thread stacks).\n";
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 4) {
        std::wcerr << L"Usage: aether_symbolicate <crash.dmp> [symbols-directory] [output.txt]\n";
        return 2;
    }
    const std::filesystem::path dump = argv[1];
    const std::filesystem::path symbols = argc >= 3 ? argv[2] : dump.parent_path();
    const std::filesystem::path output = argc >= 4 ? argv[3] : dump.parent_path() / (dump.stem().wstring() + L".symbols.txt");
    return Symbolicate(dump, symbols, output);
}
