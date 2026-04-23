#include "game/config.h"
#include "rendering/debug_names.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

// Tiny INI parser. One section header per `[name]`, one key=value per
// line, `;` or `#` starts a comment. We don't bother with fancy features
// (quoted strings, multi-line, include) — the config surface is small.
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

// Parse an integer value — accept decimal (42), hex (0x2a / 0X2A), or
// named weapons / object types (see parse_object_type / parse_weapon).
// Returns true and writes *out on success.
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

// Named weapon → slot index (0..5). Keep this list tight; numeric
// fallback covers anything we haven't bothered to name.
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

// Accepts common boolean spellings used in INI files: "true"/"false",
// "yes"/"no", "on"/"off", and numeric 0/1. Anything else leaves *out
// untouched and returns false so the caller can warn.
bool parse_bool(const std::string& raw, bool& out) {
    std::string s = to_lower(raw);
    if (s == "true" || s == "yes" || s == "on"  || s == "1") { out = true;  return true; }
    if (s == "false"|| s == "no"  || s == "off" || s == "0") { out = false; return true; }
    return false;
}

// Named object type → uint8_t. We reuse object_type_name's spelling
// (uppercase with underscores) but match case-insensitively. Unknown
// strings fall through to parse_uint so a config author can specify by
// number, e.g. `slot0 = 0x5b` for ICER.
int parse_object_type(const std::string& raw) {
    std::string want = to_lower(raw);
    for (int i = 0; i < static_cast<int>(ObjectType::COUNT); i++) {
        std::string name = to_lower(object_type_name(static_cast<ObjectType>(i)));
        if (name == want) return i;
    }
    unsigned long v;
    if (parse_uint(raw, v) &&
        v < static_cast<unsigned long>(ObjectType::COUNT)) {
        return static_cast<int>(v);
    }
    return -1;
}

} // namespace

StartupConfig load_startup_config(const std::string& path) {
    StartupConfig cfg;

    // Try a couple of locations so the same exe works whether VS launches
    // it from the project root (default $(ProjectDir)) or it's run
    // directly from build/Debug/. First match wins; missing file is
    // silent — we just use defaults.
    const std::string candidates[] = {
        path,
        "../../" + path,    // project root from build/Debug/
        "../" + path,       // project root from build/
    };
    std::ifstream in;
    for (const auto& p : candidates) {
        in.open(p);
        if (in) break;
    }
    if (!in) {
        return cfg;
    }

    std::string line, section;
    int line_no = 0;
    while (std::getline(in, line)) {
        line_no++;

        // Strip trailing comment. `;` and `#` both count, and we look
        // for them anywhere on the line (simple — no escaping).
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == ';' || line[i] == '#') { line.resize(i); break; }
        }
        std::string t = trim(line);
        if (t.empty()) continue;

        // Section header.
        if (t.front() == '[' && t.back() == ']') {
            section = to_lower(trim(t.substr(1, t.size() - 2)));
            continue;
        }

        // key = value
        auto eq = t.find('=');
        if (eq == std::string::npos) {
            std::fprintf(stderr, "exile.ini:%d: expected 'key = value'\n", line_no);
            continue;
        }
        std::string key   = to_lower(trim(t.substr(0, eq)));
        std::string value = trim(t.substr(eq + 1));

        if (section == "player") {
            unsigned long v;
            if (key == "start_x" && parse_uint(value, v))      cfg.start_x = static_cast<uint8_t>(v);
            else if (key == "start_y" && parse_uint(value, v)) cfg.start_y = static_cast<uint8_t>(v);
            else if (key == "energy"  && parse_uint(value, v)) cfg.energy  = static_cast<uint8_t>(v);
            else if (key == "weapon") {
                int w = parse_weapon(value);
                if (w >= 0) cfg.weapon = static_cast<uint8_t>(w);
            }
        } else if (section == "weapon_energy") {
            int slot = parse_weapon(key);
            unsigned long v;
            if (slot >= 0 && parse_uint(value, v)) {
                cfg.weapon_energy[static_cast<size_t>(slot)] =
                    static_cast<uint16_t>(v);
            }
        } else if (section == "caches") {
            unsigned long v;
            if (parse_uint(value, v)) {
                // Clamp to [1, compile-time max]; the ObjectManager
                // setters re-clamp too so an over-large ini value is
                // harmless.
                int n = static_cast<int>(v);
                if (n < 1) n = 1;
                if (key == "primary_slots") {
                    if (n > GameConstants::PRIMARY_OBJECT_SLOTS)
                        n = GameConstants::PRIMARY_OBJECT_SLOTS;
                    cfg.primary_slots = n;
                } else if (key == "secondary_slots") {
                    if (n > GameConstants::SECONDARY_OBJECT_SLOTS)
                        n = GameConstants::SECONDARY_OBJECT_SLOTS;
                    cfg.secondary_slots = n;
                }
            }
        } else if (section == "distances") {
            unsigned long v;
            if (parse_uint(value, v) && v <= 0xff) {
                uint8_t u = static_cast<uint8_t>(v);
                if      (key == "demote_tertiary")   cfg.demote_tertiary   = u;
                else if (key == "demote_moving")     cfg.demote_moving     = u;
                else if (key == "demote_settled")    cfg.demote_settled    = u;
                else if (key == "promote_secondary") cfg.promote_secondary = u;
                else if (key == "spawn_tertiary")    cfg.spawn_tertiary    = u;
            }
        } else if (section == "whistles") {
            bool b;
            if (key == "whistle_one_collected" && parse_bool(value, b)) {
                cfg.whistle_one_collected = b;
            } else if (key == "whistle_two_collected" && parse_bool(value, b)) {
                cfg.whistle_two_collected = b;
            }
        } else if (section == "pockets") {
            // Keys are slot0..slot4. slot0 = top of stack.
            if (key.size() == 5 && key.rfind("slot", 0) == 0 &&
                key[4] >= '0' && key[4] <= '4') {
                int slot = key[4] - '0';
                int t_id = parse_object_type(value);
                if (t_id >= 0) {
                    cfg.pockets[static_cast<size_t>(slot)] =
                        static_cast<uint8_t>(t_id);
                }
            }
        }
    }

    // Compact pockets_used from filled-slot count (lowest contiguous
    // run of non-0xff from slot 0 upwards matches the 6502's convention
    // where the stack grows upward).
    cfg.pockets_used = 0;
    for (size_t i = 0; i < cfg.pockets.size(); i++) {
        if (cfg.pockets[i] != 0xff) cfg.pockets_used++;
        else break;
    }

    return cfg;
}
