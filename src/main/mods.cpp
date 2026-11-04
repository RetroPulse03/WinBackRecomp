#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <thread>
#include <unordered_set>

#include <json/json.hpp>

#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "librecomp/helpers.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/overlays.hpp"

#include "input.hpp"
#include "mods.hpp"

namespace fs = std::filesystem;
namespace config = recomp::config;

namespace {

constexpr const char* mod_game_id = "winback";

// Changes waiting for a restart: mod id -> new state.
std::map<std::string, bool> pending;
std::chrono::steady_clock::time_point last_option_change{};
bool option_changed = false;

bool effective_enabled(const std::string& id) {
    auto it = pending.find(id);
    return it != pending.end() ? it->second
                               : recomp::mods::is_mod_enabled(id) || recomp::mods::is_mod_auto_enabled(id);
}

std::string number_text(const config::ConfigOptionNumber& n, double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f%s", n.precision, value, n.percent ? "%" : "");
    return buf;
}

std::string option_text(const config::ConfigOption& opt, const config::ConfigValueVariant& value) {
    switch (opt.type) {
        case config::ConfigOptionType::Enum: {
            const auto& e = std::get<config::ConfigOptionEnum>(opt.variant);
            auto it = e.find_option_from_value(std::get<uint32_t>(value));
            return it != e.options.end() ? it->name : "";
        }
        case config::ConfigOptionType::Number:
            return number_text(std::get<config::ConfigOptionNumber>(opt.variant), std::get<double>(value));
        case config::ConfigOptionType::Bool:
            return std::get<bool>(value) ? "ON" : "OFF";
        case config::ConfigOptionType::String:
            return std::get<std::string>(value);
        default:
            return "";
    }
}

std::vector<winback::mods::Option> read_options(const std::string& mod_id) {
    config::ConfigSchema schema = recomp::mods::get_mod_config_schema(mod_id);
    std::unordered_set<size_t> hidden;
    std::unordered_set<size_t> disabled;
    for (size_t i = 0; i < schema.options.size(); i++) {
        config::ConfigValueVariant value = recomp::mods::get_mod_config_value(mod_id, schema.options[i].id);
        for (const auto& [option, match] : schema.hidden_dependencies.check_option_dependencies(i, value)) {
            if (match) hidden.insert(option);
        }
        for (const auto& [option, match] : schema.disable_dependencies.check_option_dependencies(i, value)) {
            if (match) disabled.insert(option);
        }
    }
    std::vector<winback::mods::Option> out;
    for (size_t i = 0; i < schema.options.size(); i++) {
        const config::ConfigOption& opt = schema.options[i];
        if (opt.hidden || hidden.contains(i)) {
            continue;
        }
        winback::mods::Option o;
        o.id = opt.id;
        o.name = opt.name;
        o.description = opt.description;
        o.disabled = disabled.contains(i);
        o.value = option_text(opt, recomp::mods::get_mod_config_value(mod_id, opt.id));
        switch (opt.type) {
            case config::ConfigOptionType::Enum:
                o.type = winback::mods::OptionType::Enum;
                for (const auto& e : std::get<config::ConfigOptionEnum>(opt.variant).options) {
                    o.values.push_back(e.name);
                }
                break;
            case config::ConfigOptionType::Number: o.type = winback::mods::OptionType::Number; break;
            case config::ConfigOptionType::String: o.type = winback::mods::OptionType::String; break;
            default: o.type = winback::mods::OptionType::Bool; break;
        }
        out.push_back(std::move(o));
    }
    return out;
}

// --- Functions for mods ------------------------------------------------------

// A path inside the mod, without any way out of it.
bool valid_mod_path(const std::string& path) {
    fs::path p = fs::path(path);
    if (path.empty() || p.is_absolute() || p.has_root_name()) {
        return false;
    }
    for (const fs::path& part : p) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

// Reads a file from the calling mod's folder or .nrm.
std::vector<char> read_mod_file(size_t mod_index, const std::string& path, bool& exists) {
    exists = false;
    if (!valid_mod_path(path)) {
        return {};
    }
    fs::path root = recomp::mods::get_mod_path(mod_index);
    recomp::mods::ModOpenError error = recomp::mods::ModOpenError::Good;
    std::unique_ptr<recomp::mods::ModFileHandle> handle;
    if (fs::is_directory(root)) {
        handle = std::make_unique<recomp::mods::LooseModFileHandle>(root, error);
    }
    else {
        handle = std::make_unique<recomp::mods::ZipModFileHandle>(root, error);
    }
    if (error != recomp::mods::ModOpenError::Good) {
        return {};
    }
    return handle->read_file(path, exists);
}

// s32 winback_mod_file_size(const char* path): the file's size, or -1 if it doesn't exist.
void winback_mod_file_size(uint8_t* rdram, recomp_context* ctx, size_t mod_index) {
    bool exists;
    std::vector<char> data = read_mod_file(mod_index, _arg_string<0>(rdram, ctx), exists);
    _return<s32>(ctx, exists ? s32(data.size()) : -1);
}

// s32 winback_mod_read_file(const char* path, void* dest, u32 max_size): copies
// up to max_size bytes to dest and returns how many, or -1 if it doesn't exist.
void winback_mod_read_file(uint8_t* rdram, recomp_context* ctx, size_t mod_index) {
    bool exists;
    std::vector<char> data = read_mod_file(mod_index, _arg_string<0>(rdram, ctx), exists);
    if (!exists) {
        _return<s32>(ctx, -1);
        return;
    }
    gpr dest = ctx->r5;
    uint64_t size = std::min<uint64_t>(data.size(), uint32_t(ctx->r6));
    // Inside the memory the runtime makes available.
    uint32_t start = uint32_t(dest);
    if (start < 0x80000000u || start + size > 0x80000000u + recomp::mem_size) {
        _return<s32>(ctx, -1);
        return;
    }
    for (uint64_t i = 0; i < size; i++) {
        MEM_B(i, dest) = data[i];
    }
    _return<s32>(ctx, s32(size));
}

// void winback_mod_log(const char* text): prints a line to the game's log.
void winback_mod_log(uint8_t* rdram, recomp_context* ctx, size_t mod_index) {
    std::printf("[mod %s] %s\n", recomp::mods::get_mod_id(mod_index).c_str(), _arg_string<0>(rdram, ctx).c_str());
}

// u32 winback_mod_time_us(void): a steady clock in microseconds.
void winback_mod_time_us(uint8_t* rdram, recomp_context* ctx, size_t mod_index) {
    static const auto start = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::steady_clock::now() - start;
    _return<u32>(ctx, u32(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
}

// void winback_mod_pad_read(s32 controller, WinbackPad* pad)
void winback_mod_pad_read(uint8_t* rdram, recomp_context* ctx, size_t mod_index) {
    winback::input::RawPad raw = winback::input::read_raw(int(ctx->r4));
    gpr pad = ctx->r5;
    auto bits = [](float f) {
        s32 out;
        std::memcpy(&out, &f, sizeof(out));
        return out;
    };
    MEM_W(0x00, pad) = s32(raw.pad_buttons);
    MEM_W(0x04, pad) = s32(raw.other_buttons);
    MEM_W(0x08, pad) = bits(raw.left_x);
    MEM_W(0x0C, pad) = bits(raw.left_y);
    MEM_W(0x10, pad) = bits(raw.right_x);
    MEM_W(0x14, pad) = bits(raw.right_y);
}

// void winback_mod_pad_set(s32 controller, u32 buttons, f32 x, f32 y)
void winback_mod_pad_set(uint8_t* rdram, recomp_context* ctx, size_t mod_index) {
    auto as_float = [](gpr reg) {
        s32 raw = s32(reg);
        float out;
        std::memcpy(&out, &raw, sizeof(out));
        return out;
    };
    winback::input::set_mod_pad(int(ctx->r4), uint16_t(ctx->r5), as_float(ctx->r6), as_float(ctx->r7));
}

void write_mods_json() {
    fs::path path = recomp::get_config_path() / "mods.json";
    nlohmann::json j = nlohmann::json::object();
    {
        std::ifstream in(path);
        if (in.good()) {
            try {
                j = nlohmann::json::parse(in);
            }
            catch (const nlohmann::json::exception& e) {
                std::fprintf(stderr, "[mods] rewriting mods.json: %s\n", e.what());
            }
        }
    }
    std::vector<std::string> order = j.value("mod_order", std::vector<std::string>{});
    std::vector<std::string> enabled;
    for (const auto& mod : recomp::mods::get_all_mod_details(mod_game_id)) {
        // A mod missing from the order counts as new, which would turn a
        // default-on mod back on.
        if (std::find(order.begin(), order.end(), mod.mod_id) == order.end()) {
            order.push_back(mod.mod_id);
        }
        auto it = pending.find(mod.mod_id);
        if (it != pending.end() ? it->second : recomp::mods::is_mod_enabled(mod.mod_id)) {
            enabled.push_back(mod.mod_id);
        }
    }
    j["enabled_mods"] = enabled;
    j["mod_order"] = order;
    if (!j.contains("latest_game_mode")) {
        j["latest_game_mode"] = "";
    }
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << j.dump(4);
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        std::fprintf(stderr, "[mods] couldn't write mods.json: %s\n", ec.message().c_str());
    }
}

} // namespace

void winback::mods::register_exports() {
    recomp::overlays::register_ext_base_export("winback_mod_file_size", winback_mod_file_size);
    recomp::overlays::register_ext_base_export("winback_mod_read_file", winback_mod_read_file);
    recomp::overlays::register_ext_base_export("winback_mod_log", winback_mod_log);
    recomp::overlays::register_ext_base_export("winback_mod_time_us", winback_mod_time_us);
    recomp::overlays::register_ext_base_export("winback_mod_pad_read", winback_mod_pad_read);
    recomp::overlays::register_ext_base_export("winback_mod_pad_set", winback_mod_pad_set);
}

std::vector<winback::mods::Mod> winback::mods::list() {
    std::vector<Mod> out;
    for (const auto& d : recomp::mods::get_all_mod_details(mod_game_id)) {
        Mod m;
        m.id = d.mod_id;
        m.name = d.display_name;
        m.version = d.version.to_string();
        for (const std::string& author : d.authors) {
            m.authors += (m.authors.empty() ? "" : ", ") + author;
        }
        m.description = d.description.empty() ? d.short_description : d.description;
        m.enabled = effective_enabled(d.mod_id);
        m.options = read_options(d.mod_id);
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<winback::mods::BrokenMod> winback::mods::broken() {
    std::vector<BrokenMod> out;
    for (const auto& e : recomp::mods::get_mod_open_errors()) {
        std::string reason = recomp::mods::error_to_string(e.error);
        if (!e.error_param.empty()) {
            reason += " (" + e.error_param + ")";
        }
        out.push_back({e.mod_path.filename().string(), reason});
    }
    return out;
}

void winback::mods::set_enabled(const std::string& mod_id, bool enabled) {
    if (enabled == (recomp::mods::is_mod_enabled(mod_id) || recomp::mods::is_mod_auto_enabled(mod_id))) {
        pending.erase(mod_id);
    }
    else {
        pending[mod_id] = enabled;
    }
}

bool winback::mods::restart_needed() {
    return !pending.empty();
}

void winback::mods::step_option(const std::string& mod_id, const std::string& option_id, int delta) {
    const config::ConfigSchema& schema = recomp::mods::get_mod_config_schema(mod_id);
    auto it = schema.options_by_id.find(option_id);
    if (it == schema.options_by_id.end()) {
        return;
    }
    const config::ConfigOption& opt = schema.options[it->second];
    config::ConfigValueVariant value = recomp::mods::get_mod_config_value(mod_id, option_id);
    switch (opt.type) {
        case config::ConfigOptionType::Enum: {
            const auto& options = std::get<config::ConfigOptionEnum>(opt.variant).options;
            int n = int(options.size());
            int i = 0;
            while (i < n && options[i].value != std::get<uint32_t>(value)) {
                i++;
            }
            if (n == 0) {
                return;
            }
            value = options[(i + delta + n) % n].value;
            break;
        }
        case config::ConfigOptionType::Number: {
            const auto& n = std::get<config::ConfigOptionNumber>(opt.variant);
            double v = std::get<double>(value) + delta * (n.step > 0.0 ? n.step : 1.0);
            value = v > n.max ? n.min : v < n.min ? n.max : v;
            break;
        }
        case config::ConfigOptionType::Bool:
            value = !std::get<bool>(value);
            break;
        default:
            return;  // Text can't be typed in the menu yet.
    }
    recomp::mods::set_mod_config_value(mod_id, option_id, value);
    last_option_change = std::chrono::steady_clock::now();
    option_changed = true;
}

void winback::mods::turn_off_failed() {
    bool changed = false;
    for (const auto& e : recomp::mods::get_mod_load_errors()) {
        if (recomp::mods::is_mod_enabled(e.mod_id)) {
            pending[e.mod_id] = false;
            changed = true;
        }
    }
    // Otherwise the same load would fail again after the restart.
    if (!changed) {
        for (const auto& mod : recomp::mods::get_all_mod_details(mod_game_id)) {
            pending[mod.mod_id] = false;
        }
    }
}

void winback::mods::finish() {
    // The runtime saves changed options once they have been quiet for a second.
    if (option_changed) {
        using namespace std::chrono_literals;
        auto saved_by = last_option_change + 1500ms;
        std::this_thread::sleep_until(saved_by);
    }
    if (!pending.empty()) {
        write_mods_json();
    }
}
