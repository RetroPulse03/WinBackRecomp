#pragma once

// settings.json next to the executable, shared by the Display and Controls
// screens and edited by RecompLauncher. Each writer replaces only its own
// keys, so the others' settings are kept.

#include <cstdio>
#include <filesystem>
#include <fstream>

#include <json/json.hpp>

#include "librecomp/game.hpp"

namespace winback::settings_file {
    inline std::filesystem::path path() {
        return recomp::get_config_path() / "settings.json";
    }

    // The file's contents, or an empty object if it is missing or unreadable.
    inline nlohmann::json read() {
        std::ifstream in(path());
        if (in) {
            try {
                nlohmann::json j = nlohmann::json::parse(in);
                if (j.is_object()) {
                    return j;
                }
            }
            catch (const nlohmann::json::exception& e) {
                std::fprintf(stderr, "[settings] ignoring settings.json: %s\n", e.what());
            }
        }
        return nlohmann::json::object();
    }

    // Window thread: sets the given keys and keeps the rest of the file.
    inline void update(const nlohmann::json& values) {
        nlohmann::json j = read();
        for (const auto& [key, value] : values.items()) {
            j[key] = value;
        }
        std::ofstream out(path());
        if (out) {
            out << j.dump(4) << '\n';
        }
        else {
            std::fprintf(stderr, "[settings] can't write %s\n", path().string().c_str());
        }
    }
}
