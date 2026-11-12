// Aether application installer (compiled by tools/build_installer.py with the .NET Framework compiler that ships with Windows,
// which is C# 5: no string interpolation, no ?. operator, no expression-bodied members). The payload, aether-app.zip, is a managed
// resource of this executable. @VERSION@ is replaced by the build script.
//
//   AetherInstaller.exe                  install to %LOCALAPPDATA%\Aether, make shortcuts, offer to start the editor
//   /silent       no questions and no "press Enter" (running programs from the install folder are closed)
//   /target:DIR   install to DIR instead
//   /noshortcuts  skip the Start menu and Desktop shortcuts
//   /nolaunch     do not offer to start the editor
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Text;
using System.Threading;

class AetherInstaller
{
    const string Version = "@VERSION@";
    // The editor program inside bin\ (the Dear ImGui shell; the old Qt editor was aether_editor.exe). The installer checks it after unpacking, points the shortcuts
    // at it and offers to start it.
    const string EditorExe = "aether_editor_shell.exe";
    static readonly string LogPath = Path.Combine(Path.GetTempPath(), "aether-install-log.txt");
    static readonly StringBuilder Log = new StringBuilder();
    static bool Silent;
    static bool NoShortcuts;
    static bool NoLaunch;
    static string TargetOverride;

    static bool Interactive { get { return !Silent && !Console.IsInputRedirected; } }

    static void Note(string line) { Console.WriteLine(line); Log.AppendLine(line); }

    static void WriteLog() { try { File.WriteAllText(LogPath, Log.ToString()); } catch { } }

    static void Pause()
    {
        if (!Interactive) return;
        Console.WriteLine();
        Console.Write("Press Enter to close...");
        try { Console.ReadLine(); } catch { }
    }

    // A yes/no question; Enter means yes. Without a person to ask (silent, or input redirected) the answer is yes.
    static bool Ask(string question)
    {
        if (!Interactive) return true;
        Console.Write(question + " [Y/n] ");
        string answer = null;
        try { answer = Console.ReadLine(); } catch { }
        return answer == null || answer.Trim().Length == 0 || answer.Trim().StartsWith("y", StringComparison.OrdinalIgnoreCase);
    }

    static int Fail(string message)
    {
        Console.Error.WriteLine();
        Console.Error.WriteLine("INSTALL FAILED: " + message);
        Console.Error.WriteLine("Log written to " + LogPath);
        Log.AppendLine("FAILED: " + message);
        WriteLog();
        Pause();
        return 1;
    }

    // Programs running from the install folder hold its files open, so the old installation could neither be deleted nor replaced.
    static bool CloseRunning(string target)
    {
        string prefix = Path.GetFullPath(target).TrimEnd('\\') + "\\";
        List<Process> running = new List<Process>();
        foreach (Process process in Process.GetProcesses())
        {
            try
            {
                string file = process.MainModule.FileName;
                if (file != null && file.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) running.Add(process);
            }
            catch { } // system processes refuse the query; none of them is ours
        }
        if (running.Count == 0) return true;
        Note("These programs are running from the install folder and must be closed first:");
        foreach (Process process in running) Note("  " + process.ProcessName + " (process " + process.Id + ")");
        if (!Ask("Close them and continue?")) return false;
        foreach (Process process in running)
        {
            try
            {
                if (!process.CloseMainWindow() || !process.WaitForExit(4000))
                {
                    process.Kill();
                    process.WaitForExit(4000);
                }
            }
            catch { }
        }
        return true;
    }

    // Moves the previous installation aside and deletes it. The move is all or nothing: if anything in the folder is still open it
    // fails and the old installation is left exactly as it was, instead of half deleted.
    static void ClearTarget(string target)
    {
        if (!Directory.Exists(target)) return;
        Note("Removing the previous installation...");
        string aside = target + ".old-" + DateTime.Now.ToString("yyyyMMdd-HHmmss");
        // A program that was just closed can take a moment to let go of its files, so try a few times before giving up.
        Exception moveError = null;
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            try { Directory.Move(target, aside); moveError = null; break; }
            catch (Exception error) { moveError = error; Thread.Sleep(500); }
        }
        if (moveError != null)
        {
            throw new IOException("the previous installation in " + target + " is in use (" + moveError.Message +
                                  "). It has not been changed. Close Aether, including the editor, and run this installer again.");
        }
        try { Directory.Delete(aside, true); }
        catch (Exception error) { Note("  could not delete " + aside + " (" + error.Message + "); it can be deleted by hand"); }
    }

    static void MakeShortcut(string linkPath, string exe, string workingDirectory)
    {
        Type shellType = Type.GetTypeFromProgID("WScript.Shell");
        object shell = Activator.CreateInstance(shellType);
        object link = shellType.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { linkPath });
        Type linkType = link.GetType();
        linkType.InvokeMember("TargetPath", BindingFlags.SetProperty, null, link, new object[] { exe });
        linkType.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, link, new object[] { workingDirectory });
        linkType.InvokeMember("Description", BindingFlags.SetProperty, null, link, new object[] { "Aether Editor " + Version });
        linkType.InvokeMember("IconLocation", BindingFlags.SetProperty, null, link, new object[] { exe + ",0" });
        linkType.InvokeMember("Save", BindingFlags.InvokeMethod, null, link, null);
    }

    static void MakeShortcuts(string exe, string bin)
    {
        // Same names every time: installing again replaces the shortcut rather than adding a second one.
        string startMenu = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), "Aether");
        string[] places = new string[]
        {
            Path.Combine(startMenu, "Aether Editor.lnk"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "Aether Editor.lnk")
        };
        foreach (string link in places)
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(link));
                MakeShortcut(link, exe, bin);
                Note("Shortcut: " + link);
            }
            catch (Exception error) { Note("Could not create the shortcut " + link + ": " + error.Message); }
        }
    }

    static int Main(string[] args)
    {
        foreach (string arg in args)
        {
            string a = arg.TrimStart('/', '-');
            if (a.Equals("silent", StringComparison.OrdinalIgnoreCase)) Silent = true;
            else if (a.Equals("noshortcuts", StringComparison.OrdinalIgnoreCase)) NoShortcuts = true;
            else if (a.Equals("nolaunch", StringComparison.OrdinalIgnoreCase)) NoLaunch = true;
            else if (a.StartsWith("target:", StringComparison.OrdinalIgnoreCase)) TargetOverride = a.Substring("target:".Length).Trim('"');
        }
        try
        {
            Note("Aether Engine " + Version + " - application installer");
            Note("");
            Stream payload = Assembly.GetExecutingAssembly().GetManifestResourceStream("aether-app.zip");
            if (payload == null) return Fail("the payload is not inside this executable");
            Note("Payload found: " + (payload.Length / 1024 / 1024) + " MB");

            string target = TargetOverride != null && TargetOverride.Length > 0
                ? Path.GetFullPath(TargetOverride)
                : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Aether");
            Note("Installing to: " + target);

            if (!CloseRunning(target)) return Fail("Aether is running from " + target + "; close it and run the installer again");

            string temp = Path.Combine(Path.GetTempPath(), "aether-install-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(temp);
            string zipPath = Path.Combine(temp, "aether-app.zip");
            Note("Writing the payload to a scratch folder...");
            using (FileStream file = File.Create(zipPath)) { payload.CopyTo(file); }

            ClearTarget(target);
            Directory.CreateDirectory(target);
            Note("Unpacking, this takes a few moments...");
            ZipFile.ExtractToDirectory(zipPath, target);
            try { Directory.Delete(temp, true); } catch { }

            string bin = Path.Combine(target, "bin");
            string editor = Path.Combine(bin, EditorExe);
            if (!File.Exists(editor)) return Fail("the installation is incomplete: " + editor + " is missing after unpacking");
            File.WriteAllText(Path.Combine(target, "INSTALLED_VERSION.txt"), Version + Environment.NewLine);

            int count = Directory.GetFiles(target, "*", SearchOption.AllDirectories).Length;
            Note("");
            Note("Installed " + count + " files.");
            if (!NoShortcuts) MakeShortcuts(editor, bin);
            Note("");
            Note("The editor is " + editor);
            Note("If you start the editor from anywhere else (an old shortcut, an older download), you are running an older copy.");
            Note("");
            Note("To use the SDK from CMake:");
            Note("  cmake -S . -B build -DCMAKE_PREFIX_PATH=\"" + target + "\"");
            Note("  find_package(Aether REQUIRED)");
            WriteLog();

            if (!NoLaunch && Interactive && Ask("Start the Aether editor now?"))
            {
                ProcessStartInfo start = new ProcessStartInfo(editor);
                start.WorkingDirectory = bin;
                start.UseShellExecute = false;
                Process.Start(start);
                return 0;
            }
            Pause();
            return 0;
        }
        catch (Exception error)
        {
            return Fail(error.GetType().Name + ": " + error.Message);
        }
    }
}
