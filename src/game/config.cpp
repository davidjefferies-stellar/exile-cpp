#include "game/config.h"
#include "rendering/debug_names.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

// Tiny INI parser. One section header per `[name]`, one key=value per
// line, `;` or `#` starts a comment. Common cases (bool flags, uint8_t
// fields) dispatch through declarative tables; section-specific logic
// (weapon names, key bitmasks, startup_spawns CSV) is inlined below.
namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Parse decimal (42) or hex (0x2a / 0X2A) into an unsigned long.
bool parse_uint(const std::string& s, unsigned long& out) {
    if (s.empty()) return false;
    try {
        size_t used = 0;
        int base = 10;
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            base = 16;
        unsigned long v = std::stoul(s, &used, base);
        if (used == 0) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

// Parse + clamp into any integral T. Saves the repeated
// "parse_uint, clamp, static_cast" boilerplate at numeric call sites.
template <typename T>
bool parse_uint_clamped(const std::string& s, T& out,
                        unsigned long lo, unsigned long hi) {
    unsigned long v;
    if (!parse_uint(s, v)) return false;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    out = static_cast<T>(v);
    return true;
}

// Named weapon -> slot index (0..5). Numeric fallback handles slots we
// haven't bothered to name.
int parse_weapon(const std::string& raw) {
    std::string s = to_lower(raw);
    if (s == "jetpack") return 0;
    if (s == "pistol")  return 1;
    if (s == "icer")    return 2;
    if (s == "blaster") return 3;
    if (s == "plasma" || s == "plasma_gun") return 4;
    if (s == "suit" || s == "protection_suit") return 5;
    unsigned long v;
    if (parse_uint(raw, v) && v < 6) return static_cast<int>(v);
    return -1;
}

bool parse_bool(const std::string& raw, bool& out) {
    std::string s = to_lower(raw);
    if (s == "true" || s == "yes" || s == "on"  || s == "1") { out = true;  return true; }
    if (s == "false"|| s == "no"  || s == "off" || s == "0") { out = false; return true; }
    return false;
}

// Named object type -> uint8_t. Matches object_type_name's spelling
// case-insensitively, plus a short alias table for friendlier long
// names (used by [pockets] / [startup_spawns] / [keys]). Numeric
// fallback lets configs reference types by raw id.
int parse_object_type(const std::string& raw) {
    std::string want = to_lower(raw);
    for (int i = 0; i < static_cast<int>(ObjectType::COUNT); i++) {
        if (to_lower(object_type_name(static_cast<ObjectType>(i))) == want) return i;
    }
    struct Alias { const char* name; int id; };
    static constexpr Alias aliases[] = {
        { "cyan_yellow_green_key", static_cast<int>(ObjectType::CYAN_YELLOW_GREEN_KEY) },
        { "red_yellow_green_key",  static_cast<int>(ObjectType::RED_YELLOW_GREEN_KEY)  },
        { "green_yellow_red_key",  static_cast<int>(ObjectType::GREEN_YELLOW_RED_KEY)  },
        { "yellow_white_red_key",  static_cast<int>(ObjectType::YELLOW_WHITE_RED_KEY)  },
        { "red_magenta_red_key",   static_cast<int>(ObjectType::RED_MAGENTA_RED_KEY)   },
        { "blue_cyan_green_key",   static_cast<int>(ObjectType::BLUE_CYAN_GREEN_KEY)   },
    };
    for (const auto& a : aliases) if (want == a.name) return a.id;
    unsigned long v;
    if (parse_uint(raw, v) && v < static_cast<unsigned long>(ObjectType::COUNT))
        return static_cast<int>(v);
    return -1;
}

// ----------------------------------------------------------------------
// Declarative dispatch tables. Each row maps (section, key) to a member
// pointer; the main loop walks these before falling through to section-
// specific handlers, so adding a new bool / uint8_t flag is a one-line
// edit rather than another if/else branch.
// ----------------------------------------------------------------------

struct BoolEntry { const char* sec; const char* key; bool StartupConfig::* f; };
constexpr BoolEntry kBools[] = {
    {"player",    "give_protection_suit",        &StartupConfig::give_protection_suit},
    {"player",    "invincible",                  &StartupConfig::invincible},
    {"creatures", "pipe_198_190_crab",           &StartupConfig::pipe_198_190_crab},
    {"creatures", "spawn_initial_triax",         &StartupConfig::spawn_initial_triax},
    {"creatures", "sucking_nest_damages_player", &StartupConfig::sucking_nest_damages_player},
    {"audio",     "enabled",                     &StartupConfig::audio_enabled},
    {"logs",      "enabled",                     &StartupConfig::logs_enabled},
    {"debug",     "stress_test",                 &StartupConfig::stress_test},
    {"debug",     "grenade_chain",               &StartupConfig::grenade_chain},
    {"debug",     "icer_drop",                   &StartupConfig::icer_drop},
    {"debug",     "jetpack_boost_tint",          &StartupConfig::jetpack_boost_tint},
    {"debug",     "profile",                     &StartupConfig::profile},
    {"debug",     "show_fps",                    &StartupConfig::show_fps},
    {"debug",     "npc_firing_enabled",          &StartupConfig::npc_firing_enabled},
    {"landscape", "use_cpp_impl",                &StartupConfig::use_cpp_landscape},
    {"whistles",  "whistle_one_collected",       &StartupConfig::whistle_one_collected},
    {"whistles",  "whistle_two_collected",       &StartupConfig::whistle_two_collected},
};

struct U8Entry { const char* sec; const char* key; uint8_t StartupConfig::* f; };
constexpr U8Entry kU8s[] = {
    {"player", "start_x", &StartupConfig::start_x},
    {"player", "start_y", &StartupConfig::start_y},
    {"player", "energy",  &StartupConfig::energy},
};

struct SubpixelEntry { const char* name; StartupConfig::SubpixelMode mode; };
constexpr SubpixelEntry kSubpixelModes[] = {
    {"off",      StartupConfig::SubpixelMode::Off},
    {"false",    StartupConfig::SubpixelMode::Off},
    {"no",       StartupConfig::SubpixelMode::Off},
    {"0",        StartupConfig::SubpixelMode::Off},
    {"on",       StartupConfig::SubpixelMode::On},
    {"true",     StartupConfig::SubpixelMode::On},
    {"yes",      StartupConfig::SubpixelMode::On},
    {"1",        StartupConfig::SubpixelMode::On},
    {"adaptive", StartupConfig::SubpixelMode::Adaptive},
    {"auto",     StartupConfig::SubpixelMode::Adaptive},
};

// Split "a, b, c" -> {"a","b","c"} with whitespace trimmed off each token.
// Used by [startup_spawns]; the rest of the file is one-value-per-line.
std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { out.push_back(trim(cur)); cur.clear(); }
        else            cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(trim(cur));
    return out;
}

} // namespace

StartupConfig load_startup_config(const std::string& path) {
    StartupConfig cfg;

    // Search a couple of locations so the same exe works from project
    // root or from a build subdir. First match wins; missing file is
    // silent and we just use defaults.
    const std::string candidates[] = { path, "../../" + path, "../" + path };
    std::ifstream in;
    for (const auto& p : candidates) { in.open(p); if (in) break; }
    if (!in) return cfg;

    std::string line, section;
    int line_no = 0;
    while (std::getline(in, line)) {
        line_no++;

        // Strip trailing comment (`;` or `#`), trim, skip blanks.
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == ';' || line[i] == '#') { line.resize(i); break; }
        }
        std::string t = trim(line);
        if (t.empty()) continue;

        if (t.front() == '[' && t.back() == ']') {
            section = to_lower(trim(t.substr(1, t.size() - 2)));
            continue;
        }

        auto eq = t.find('=');
        if (eq == std::string::npos) {
            std::fprintf(stderr, "exile.ini:%d: expected 'key = value'\n", line_no);
            continue;
        }
        std::string key   = to_lower(trim(t.substr(0, eq)));
        std::string value = trim(t.substr(eq + 1));

        // 1. Generic bool dispatch.
        bool handled = false;
        for (const auto& e : kBools) {
            if (section == e.sec && key == e.key) {
                bool b; if (parse_bool(value, b)) cfg.*(e.f) = b;
                handled = true; break;
            }
        }
        if (handled) continue;

        // 2. Generic uint8_t dispatch.
        for (const auto& e : kU8s) {
            if (section == e.sec && key == e.key) {
                parse_uint_clamped(value, cfg.*(e.f),
                                   0ul, 0xfful);
                handled = true; break;
            }
        }
        if (handled) continue;

        // 3. Section-specific handlers for everything that doesn't fit
        //    the generic (section, key) -> field mapping.
        if (section == "player") {
            if (key == "weapon") {
                int w = parse_weapon(value);
                if (w >= 0) cfg.weapon = static_cast<uint8_t>(w);
            } else if (key == "bbc_save") {
                cfg.bbc_save_path = value;
            }
        } else if (section == "weapon_energy") {
            int slot = parse_weapon(key);
            unsigned long v;
            if (slot >= 0 && parse_uint(value, v)) {
                cfg.weapon_energy[static_cast<size_t>(slot)] =
                    static_cast<uint16_t>(v);
            }
        } else if (section == "caches") {
            if (key == "primary_slots") {
                parse_uint_clamped(value, cfg.primary_slots,
                                   1ul, static_cast<unsigned long>(
                                       GameConstants::PRIMARY_OBJECT_SLOTS));
            } else if (key == "secondary_slots") {
                parse_uint_clamped(value, cfg.secondary_slots,
                                   1ul, static_cast<unsigned long>(
                                       GameConstants::SECONDARY_OBJECT_SLOTS));
            }
        } else if (section == "distances") {
            // radius_static fans out to all four static-related rings.
            // Keeping them tied avoids the spawn/demote oscillation the
            // port's wider viewport causes when they diverge.
            uint8_t u;
            if (!parse_uint_clamped(value, u, 0ul, 0xfful)) continue;
            if (key == "radius_static") {
                cfg.demote_tertiary = u;
                cfg.demote_settled  = u;
                cfg.promote_secondary = u;
                cfg.spawn_tertiary  = u;
            } else if (key == "radius_moving") {
                cfg.demote_moving = u;
            }
        } else if (section == "keys") {
            // Key names route through parse_object_type (handles long
            // names like cyan_yellow_green_key and short aliases like
            // cyg_key). Map the resolved type back to its bitmask slot.
            int t = parse_object_type(key);
            constexpr int base = static_cast<int>(ObjectType::CYAN_YELLOW_GREEN_KEY);
            constexpr int top  = static_cast<int>(ObjectType::BLUE_CYAN_GREEN_KEY);
            if (t < base || t > top) continue;
            bool b;
            if (parse_bool(value, b))
                cfg.keys_collected[static_cast<size_t>(t - base)] = b ? 0x80 : 0;
        } else if (section == "render") {
            if (key == "subpixel_rendering") {
                std::string v = to_lower(value);
                bool found = false;
                for (const auto& sp : kSubpixelModes) {
                    if (v == sp.name) {
                        cfg.subpixel_mode = sp.mode;
                        found = true; break;
                    }
                }
                if (!found) {
                    std::fprintf(stderr,
                        "exile.ini:%d: unknown subpixel_rendering value '%s'"
                        " (expected off / on / adaptive)\n",
                        line_no, value.c_str());
                }
            } else if (key == "zoom_den") {
                parse_uint_clamped(value, cfg.zoom_den, 1ul, 0xfful);
            }
        } else if (section == "debug") {
            if (key == "target_fps") {
                parse_uint_clamped(value, cfg.target_fps,
                                   static_cast<unsigned long>(GameConstants::TARGET_FPS_MIN),
                                   static_cast<unsigned long>(GameConstants::TARGET_FPS_MAX));
            }
        } else if (section == "startup_spawns") {
            // Format: <key> = <type>, <tile_x>, <tile_y>[, <x_frac>][, <y_frac>]
            auto tokens = split_csv(value);
            if (tokens.size() < 3 || tokens.size() > 5) {
                std::fprintf(stderr,
                    "exile.ini:%d: [startup_spawns] expected "
                    "type,x,y[,x_frac,y_frac]\n", line_no);
                continue;
            }
            int t_id = parse_object_type(tokens[0]);
            unsigned long tx = 0, ty = 0, xf = 0x80, yf = 0x80;
            bool ok = (t_id >= 0)
                   && parse_uint(tokens[1], tx) && tx <= 0xff
                   && parse_uint(tokens[2], ty) && ty <= 0xff;
            if (ok && tokens.size() >= 4) ok = parse_uint(tokens[3], xf) && xf <= 0xff;
            if (ok && tokens.size() >= 5) ok = parse_uint(tokens[4], yf) && yf <= 0xff;
            if (!ok) {
                std::fprintf(stderr,
                    "exile.ini:%d: [startup_spawns] could not parse '%s'\n",
                    line_no, value.c_str());
                continue;
            }
            cfg.startup_spawns.push_back({
                static_cast<uint8_t>(t_id),
                static_cast<uint8_t>(tx), static_cast<uint8_t>(ty),
                static_cast<uint8_t>(xf), static_cast<uint8_t>(yf),
            });
        } else if (section == "pockets") {
            // Keys are slot0..slot4. slot0 = top of stack.
            if (key.size() == 5 && key.rfind("slot", 0) == 0 &&
                key[4] >= '0' && key[4] <= '4') {
                int slot = key[4] - '0';
                int t_id = parse_object_type(value);
                if (t_id >= 0)
                    cfg.pockets[static_cast<size_t>(slot)] = static_cast<uint8_t>(t_id);
            }
        }
    }

    // Compact pockets_used: count the contiguous run of filled slots
    // from slot 0 upward (matches the 6502's stack-grows-upward convention).
    cfg.pockets_used = 0;
    for (size_t i = 0; i < cfg.pockets.size(); i++) {
        if (cfg.pockets[i] != 0xff) cfg.pockets_used++;
        else break;
    }

    return cfg;
}
