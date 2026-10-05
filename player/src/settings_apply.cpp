#include "aether/player/settings_apply.h"

namespace aether::player {

using save::GameSettings;

std::vector<std::string> ApplySettings(const GameSettings& now, const SettingsTargets& targets, const GameSettings* before) {
    std::vector<std::string> done;
    if (targets.set_bus_volume_db) {
        struct Bus {
            const char* name;
            f32 GameSettings::*field;
        };
        for (const Bus& b : {Bus{"Master", &GameSettings::master}, Bus{"Music", &GameSettings::music}, Bus{"SFX", &GameSettings::sfx},
                             Bus{"Voice", &GameSettings::voice}}) {
            if (before && before->*(b.field) == now.*(b.field)) continue;
            targets.set_bus_volume_db(b.name, save::BusVolumeDb(now.*(b.field)));
            done.push_back(std::string("volume ") + b.name);
        }
    }
    if (targets.set_quality && (!before || before->quality != now.quality)) {
        targets.set_quality(now.quality);
        done.push_back("quality " + now.quality);
    }
    if (targets.set_window &&
        (!before || before->width != now.width || before->height != now.height || before->fullscreen != now.fullscreen || before->vsync != now.vsync)) {
        targets.set_window(now.width, now.height, now.fullscreen, now.vsync);
        done.push_back("window");
    }
    return done;
}

} // namespace aether::player
