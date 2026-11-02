#pragma once

// The Mods menu's view of the runtime's mods, and the functions WinBack gives
// mods. Code mods load once as the game boots, so turning one on or off only
// takes effect after a restart: the change waits here until the game exits.
// Everything except the mod exports runs on the window thread.

#include <string>
#include <vector>

namespace winback::mods {
    enum class OptionType { Enum, Number, String, Bool };

    struct Option {
        std::string id;
        std::string name;
        std::string description;
        OptionType type = OptionType::Bool;
        std::vector<std::string> values;  // Enum only.
        std::string value;                // As shown.
        bool disabled = false;
    };

    struct Mod {
        std::string id;
        std::string name;
        std::string version;
        std::string authors;
        std::string description;
        bool enabled = false;  // Including a change waiting for a restart.
        std::vector<Option> options;
    };

    // A file or folder in mods/ that couldn't be opened.
    struct BrokenMod {
        std::string name;
        std::string reason;
    };

    // Registers the functions mods can import. Call before the game starts.
    void register_exports();

    std::vector<Mod> list();
    std::vector<BrokenMod> broken();

    void set_enabled(const std::string& mod_id, bool enabled);
    // True when a mod's on/off state differs from what's loaded.
    bool restart_needed();
    // Steps an enum or number option, wrapping around; flips a bool.
    void step_option(const std::string& mod_id, const std::string& option_id, int delta);

    // After a failed load: turns off the mods that failed.
    void turn_off_failed();
    // Before exiting: writes pending changes to mods.json and lets the runtime
    // finish saving changed options.
    void finish();
}
