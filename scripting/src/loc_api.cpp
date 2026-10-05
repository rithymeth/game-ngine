#include "aether/script/loc_api.h"

#include "aether/loc/localization.h"
#include "aether/loc/localize.h"

#include <cmath>

namespace aether::script {

namespace {

using loc::Localize;

bool NeedString(NativeCall& c, usize i, const char* what) {
    if (c.IsString(i)) return true;
    c.Fail(std::string("Localization: ") + what + " must be a string");
    return false;
}

} // namespace

void InstallLocApi(LuauHost& host) {
    const auto def = [&](const char* name, NativeFunction fn) { host.RegisterNative("Localization", name, std::move(fn)); };

    def("GetText", [](NativeCall& c) {
        if (!NeedString(c, 0, "the key")) return;
        c.Return(Localize::GetText(c.String(0), c.IsString(1) ? c.String(1) : std::string()));
    });
    def("Format", [](NativeCall& c) {
        if (!NeedString(c, 0, "the key") || !NeedString(c, 1, "the default") || !NeedString(c, 2, "the argument name")) return;
        if (c.IsNumber(3)) {
            const f64 v = c.Number(3);
            if (v == std::floor(v) && std::fabs(v) < 2147483647.0) {
                c.Return(Localize::FormatInt(c.String(0), c.String(1), c.String(2), static_cast<i32>(v)));
            } else {
                loc::Localization* l = loc::Localization::Active();
                loc::FormatArgs args{{c.String(2), v}};
                c.Return(l ? l->Text(c.String(0), c.String(1), &args) : loc::Format(c.String(1).empty() ? c.String(0) : c.String(1), "en", args));
            }
        } else if (c.IsString(3)) {
            c.Return(Localize::FormatString(c.String(0), c.String(1), c.String(2), c.String(3)));
        } else {
            c.Fail("Localization: the value must be a number or a string");
        }
    });
    def("HasText", [](NativeCall& c) {
        if (NeedString(c, 0, "the key")) c.Return(Localize::HasText(c.String(0)));
    });
    def("SetLanguage", [](NativeCall& c) {
        if (NeedString(c, 0, "the language")) Localize::SetLanguage(c.String(0));
    });
    def("GetLanguage", [](NativeCall& c) { c.Return(Localize::GetLanguage()); });
}

} // namespace aether::script
