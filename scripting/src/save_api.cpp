#include "aether/script/save_api.h"

#include "aether/save/save_games.h"

#include <cmath>

namespace aether::script {

namespace {

using save::SaveGames;

bool NeedString(NativeCall& c, usize i, const char* what) {
    if (c.IsString(i)) return true;
    c.Fail(std::string("SaveGames: ") + what + " must be a string");
    return false;
}

bool NeedNumber(NativeCall& c, usize i, const char* what) {
    if (c.IsNumber(i)) return true;
    c.Fail(std::string("SaveGames: ") + what + " must be a number");
    return false;
}

} // namespace

void InstallSaveApi(LuauHost& host) {
    const auto def = [&](const char* name, NativeFunction fn) { host.RegisterNative("SaveGames", name, std::move(fn)); };

    def("SetNumber", [](NativeCall& c) {
        if (!NeedString(c, 0, "the key") || !NeedNumber(c, 1, "the value")) return;
        const f64 v = c.Number(1);
        if (v == std::floor(v) && std::fabs(v) < 2147483647.0) SaveGames::SetInt(c.String(0), static_cast<i32>(v));
        else SaveGames::SetFloat(c.String(0), static_cast<f32>(v));
    });
    def("SetString", [](NativeCall& c) {
        if (!NeedString(c, 0, "the key") || !NeedString(c, 1, "the value")) return;
        SaveGames::SetString(c.String(0), c.String(1));
    });
    def("SetBool", [](NativeCall& c) {
        if (!NeedString(c, 0, "the key")) return;
        if (!c.IsBool(1)) return c.Fail("SaveGames: the value must be a boolean");
        SaveGames::SetBool(c.String(0), c.Bool(1));
    });
    def("SetVector", [](NativeCall& c) {
        if (!NeedString(c, 0, "the key") || !NeedNumber(c, 1, "x") || !NeedNumber(c, 2, "y") || !NeedNumber(c, 3, "z")) return;
        SaveGames::SetVector(c.String(0), Vec3(static_cast<f32>(c.Number(1)), static_cast<f32>(c.Number(2)), static_cast<f32>(c.Number(3))));
    });
    def("GetNumber", [](NativeCall& c) {
        if (!NeedString(c, 0, "the key")) return;
        const f64 fallback = c.Number(1, 0.0);
        // A number is an int or a float in the bag; read whichever is there.
        const std::string key = c.String(0);
        const auto* e = save::SaveSystem::Active() ? save::SaveSystem::Active()->Bag().Find(key) : nullptr;
        if (e && e->kind == save::SaveEntry::Int) c.Return(static_cast<f64>(e->i));
        else if (e && e->kind == save::SaveEntry::Float) c.Return(static_cast<f64>(e->f));
        else c.Return(fallback);
    });
    def("GetString", [](NativeCall& c) {
        if (!NeedString(c, 0, "the key")) return;
        c.Return(SaveGames::GetString(c.String(0), c.String(1)));
    });
    def("GetBool", [](NativeCall& c) {
        if (!NeedString(c, 0, "the key")) return;
        c.Return(SaveGames::GetBool(c.String(0), c.Bool(1)));
    });
    def("GetVector", [](NativeCall& c) {
        if (!NeedString(c, 0, "the key")) return;
        const Vec3 v = SaveGames::GetVector(c.String(0), Vec3(static_cast<f32>(c.Number(1)), static_cast<f32>(c.Number(2)), static_cast<f32>(c.Number(3))));
        c.Return(static_cast<f64>(v.x));
        c.Return(static_cast<f64>(v.y));
        c.Return(static_cast<f64>(v.z));
    });
    def("HasKey", [](NativeCall& c) {
        if (NeedString(c, 0, "the key")) c.Return(SaveGames::HasKey(c.String(0)));
    });
    def("RemoveKey", [](NativeCall& c) {
        if (NeedString(c, 0, "the key")) c.Return(SaveGames::RemoveKey(c.String(0)));
    });
    def("CreateSaveObject", [](NativeCall&) { SaveGames::CreateSaveObject(); });
    def("SaveToSlot", [](NativeCall& c) {
        if (NeedString(c, 0, "the slot")) c.Return(SaveGames::SaveToSlot(c.String(0)));
    });
    def("SaveToSlotAsync", [](NativeCall& c) {
        if (NeedString(c, 0, "the slot")) SaveGames::SaveToSlotAsync(c.String(0));
    });
    def("LoadFromSlot", [](NativeCall& c) {
        if (NeedString(c, 0, "the slot")) c.Return(SaveGames::LoadFromSlot(c.String(0)));
    });
    def("DoesSaveExist", [](NativeCall& c) {
        if (NeedString(c, 0, "the slot")) c.Return(SaveGames::DoesSaveExist(c.String(0)));
    });
    def("DeleteSave", [](NativeCall& c) {
        if (NeedString(c, 0, "the slot")) c.Return(SaveGames::DeleteSave(c.String(0)));
    });
    def("GetSlotCount", [](NativeCall& c) { c.Return(static_cast<f64>(SaveGames::GetSlotCount())); });
    def("GetSlotName", [](NativeCall& c) {
        if (NeedNumber(c, 0, "the index")) c.Return(SaveGames::GetSlotName(static_cast<i32>(c.Number(0))));
    });
    def("GetLastError", [](NativeCall& c) { c.Return(SaveGames::GetLastError()); });
    def("CaptureWorld", [](NativeCall& c) { c.Return(SaveGames::CaptureWorld()); });
    def("RestoreWorld", [](NativeCall& c) { c.Return(SaveGames::RestoreWorld()); });
}

} // namespace aether::script
