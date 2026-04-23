#include "game/game.h"
#include "rendering/debug_names.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>

// ============================================================================
// Save / load — human-readable text format.
// ============================================================================
//
// The landscape is fully deterministic from the fixed world seed, so a save
// only needs the mutable game state:
//   * frame counter, RNG state
//   * player / pocket / weapon / mushroom / whistle state
//   * global events (flooding, earthquake, robot availability)
//   * all 16 primary object slots (full field set)
//   * all 32 secondary slots (compact packed form)
//   * 235 bytes of tertiary data (door states, spawn gates, switch toggles)
//
// Layout:
//
//   # exile-cpp save v1
//   version 1
//   frame 0x4c
//
//   [player]
//   weapon 1
//   angle 0xc0
//   ...
//
//   [events]
//   flooding 0x00
//   earthquake 0xff
//   ...
//
//   [object 0]
//   type PLAYER
//   x 0x9b.80
//   ...
//
//   [secondary 0]
//   type 0x4c
//   x 0x84
//   ...
//
//   [tertiary]
//   00 80 01 02 ...   (16 per line)
//
//   [rng]
//   state 0x49 0x52 0x56 0x49

namespace {

// ------------------------- output helpers ---------------------------------

static std::string hex_byte(unsigned v) {
    std::ostringstream o;
    o << "0x" << std::hex << std::setw(2) << std::setfill('0') << (v & 0xff);
    return o.str();
}

static std::string hex_word(unsigned v) {
    std::ostringstream o;
    o << "0x" << std::hex << std::setw(4) << std::setfill('0') << (v & 0xffff);
    return o.str();
}

// ------------------------- parsing helpers --------------------------------

// Parse a single numeric token. Supports "0xff", "255", negative decimals.
static long parse_num(const std::string& tok) {
    if (tok.empty()) return 0;
    try {
        if (tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
            return std::stol(tok.substr(2), nullptr, 16);
        }
        return std::stol(tok, nullptr, 10);
    } catch (...) {
        return 0;
    }
}

// Narrowing wrappers so loaders can write `field = parse_u8(tok)` without
// triggering C4244 on every uint8_t-typed Object/Game member. The cast is
// deliberate (save files are authoritative; the source is a byte in the
// first place).
static uint8_t  parse_u8 (const std::string& tok) { return static_cast<uint8_t >(parse_num(tok)); }
static uint16_t parse_u16(const std::string& tok) { return static_cast<uint16_t>(parse_num(tok)); }

// Split "0x9b.80" into whole / fraction. Accepts either form — falls back to
// a single number (whole part only) if there's no '.'.
static void parse_fixed(const std::string& tok, uint8_t& whole, uint8_t& frac) {
    auto dot = tok.find('.');
    if (dot == std::string::npos) {
        whole = static_cast<uint8_t>(parse_num(tok));
        frac = 0;
    } else {
        whole = static_cast<uint8_t>(parse_num(tok.substr(0, dot)));
        // Fraction is written as two hex digits without any "0x" prefix.
        frac = static_cast<uint8_t>(std::stoul(tok.substr(dot + 1), nullptr, 16));
    }
}

static std::string fixed_str(uint8_t whole, uint8_t frac) {
    std::ostringstream o;
    o << "0x" << std::hex << std::setw(2) << std::setfill('0') << (unsigned)whole
      << "." << std::setw(2) << std::setfill('0') << (unsigned)frac;
    return o.str();
}

// Split a line on whitespace; strip a leading '#' comment line entirely.
static std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream ss(line);
    std::string tok;
    while (ss >> tok) out.push_back(std::move(tok));
    if (!out.empty() && out[0].size() > 0 && out[0][0] == '#') out.clear();
    return out;
}

// ------------------------- primary object I/O -----------------------------

static void write_object(std::ostream& f, int slot, const Object& o) {
    f << "[object " << slot << "]\n";
    f << "type "            << object_type_name(o.type)
      << "  " << hex_byte(static_cast<unsigned>(o.type)) << "\n";
    f << "x "               << fixed_str(o.x.whole, o.x.fraction) << "\n";
    f << "y "               << fixed_str(o.y.whole, o.y.fraction) << "\n";
    f << "energy "          << hex_byte(o.energy) << "\n";
    f << "sprite "          << hex_byte(o.sprite) << "\n";
    f << "palette "         << hex_byte(o.palette) << "\n";
    f << "flags "           << hex_byte(o.flags) << "\n";
    f << "touching "        << hex_byte(o.touching) << "\n";
    f << "target_and_flags " << hex_byte(o.target_and_flags) << "\n";
    f << "velocity_x "      << (int)o.velocity_x << "\n";
    f << "velocity_y "      << (int)o.velocity_y << "\n";
    f << "timer "           << hex_byte(o.timer) << "\n";
    f << "state "           << hex_byte(o.state) << "\n";
    f << "tx "              << hex_byte(o.tx) << "\n";
    f << "ty "              << hex_byte(o.ty) << "\n";
    f << "tertiary_slot "   << (int)o.tertiary_slot << "\n";
    f << "tertiary_data "   << hex_byte(o.tertiary_data_offset) << "\n";
    f << "\n";
}

// Apply a single "key value" line to an Object. Unknown keys are ignored so
// older saves can be loaded into newer builds with added fields.
static void apply_object_field(Object& o, const std::vector<std::string>& t) {
    if (t.size() < 2) return;
    const std::string& k = t[0];
    const std::string& v = t[1];
    if      (k == "type") {
        // Prefer the hex fallback (second token) over the name so renames
        // don't break saves. When writing we emit both.
        if (t.size() >= 3) o.type = static_cast<ObjectType>(parse_num(t[2]));
        else               o.type = static_cast<ObjectType>(parse_num(v));
    }
    else if (k == "x")                parse_fixed(v, o.x.whole, o.x.fraction);
    else if (k == "y")                parse_fixed(v, o.y.whole, o.y.fraction);
    else if (k == "energy")           o.energy = parse_u8(v);
    else if (k == "sprite")           o.sprite = parse_u8(v);
    else if (k == "palette")          o.palette = parse_u8(v);
    else if (k == "flags")            o.flags = parse_u8(v);
    else if (k == "touching")         o.touching = parse_u8(v);
    else if (k == "target_and_flags") o.target_and_flags = parse_u8(v);
    else if (k == "velocity_x")       o.velocity_x = static_cast<int8_t>(parse_num(v));
    else if (k == "velocity_y")       o.velocity_y = static_cast<int8_t>(parse_num(v));
    else if (k == "timer")            o.timer = parse_u8(v);
    else if (k == "state")            o.state = parse_u8(v);
    else if (k == "tx")               o.tx = parse_u8(v);
    else if (k == "ty")               o.ty = parse_u8(v);
    else if (k == "tertiary_slot")    o.tertiary_slot = parse_u8(v);
    else if (k == "tertiary_data")    o.tertiary_data_offset = parse_u8(v);
}

} // namespace

bool Game::save_game(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;

    f << "# exile-cpp save file v1\n";
    f << "# Position format: whole.fraction, both hex.\n";
    f << "version 1\n";
    f << "frame " << hex_byte(frame_counter_) << "\n\n";

    // -------- player-scope state (not in the primary object) --------------
    f << "[player]\n";
    f << "weapon "              << (int)player_weapon_ << "\n";
    f << "aim_angle "           << hex_byte(player_aim_angle_) << "\n";
    f << "angle "               << hex_byte(player_angle_) << "\n";
    f << "facing "              << hex_byte(player_facing_) << "\n";
    f << "held_slot "           << hex_byte(held_object_slot_) << "\n";
    f << "pockets";
    for (int i = 0; i < 5; i++) f << " " << hex_byte(pockets_[i]);
    f << "\n";
    f << "pockets_used "        << (int)pockets_used_ << "\n";
    f << "weapon_energy";
    for (int i = 0; i < 6; i++) f << " " << hex_word(weapon_energy_[i]);
    f << "\n";
    f << "jetpack_active "      << (jetpack_active_ ? 1 : 0) << "\n";
    f << "whistle_one_active "  << (whistle_one_active_ ? 1 : 0) << "\n";
    f << "whistle_two_activator " << hex_byte(whistle_two_activator_) << "\n";
    f << "whistle_one_collected " << (whistle_one_collected_ ? 1 : 0) << "\n";
    f << "whistle_two_collected " << (whistle_two_collected_ ? 1 : 0) << "\n";
    f << "chatter_reserve "     << hex_byte(chatter_energy_reserve_) << "\n";
    f << "mushroom_timers "     << hex_byte(player_mushroom_timers_[0])
                                << " " << hex_byte(player_mushroom_timers_[1]) << "\n";
    f << "mushroom_immunity "   << (mushroom_immunity_collected_ ? 1 : 0) << "\n";
    f << "\n";

    // -------- global events ----------------------------------------------
    f << "[events]\n";
    f << "flooding "          << hex_byte(flooding_state_) << "\n";
    f << "earthquake "        << hex_byte(earthquake_state_) << "\n";
    f << "robot_availability";
    for (int i = 0; i < 4; i++) f << " " << hex_byte(clawed_robot_availability_[i]);
    f << "\n";
    f << "robot_teleport";
    for (int i = 0; i < 4; i++) f << " " << hex_byte(clawed_robot_teleport_energy_[i]);
    f << "\n";
    f << "door_timer "        << hex_byte(object_mgr_.door_timer_) << "\n";
    f << "\n";

    // -------- primary objects --------------------------------------------
    for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        const Object& o = object_mgr_.object(i);
        if (!o.is_active()) continue;
        write_object(f, i, o);
    }

    // -------- secondary objects ------------------------------------------
    for (int i = 0; i < GameConstants::SECONDARY_OBJECT_SLOTS; i++) {
        const SecondaryObject& s = object_mgr_.secondary(i);
        if (s.y == 0) continue;
        f << "[secondary " << i << "]\n";
        f << "type "   << hex_byte(s.type) << "\n";
        f << "x "      << hex_byte(s.x) << "\n";
        f << "y "      << hex_byte(s.y) << "\n";
        f << "energy_fracs " << hex_byte(s.energy_and_fractions) << "\n";
        f << "\n";
    }

    // -------- tertiary data dump -----------------------------------------
    f << "[tertiary]\n";
    const uint8_t* td = object_mgr_.tertiary_data_ptr();
    for (int i = 0; i < ObjectManager::TERTIARY_DATA_SIZE; i++) {
        f << std::hex << std::setw(2) << std::setfill('0') << (unsigned)td[i];
        if ((i & 0x0f) == 0x0f) f << "\n";
        else                    f << " ";
    }
    if (ObjectManager::TERTIARY_DATA_SIZE % 16 != 0) f << "\n";
    f << std::dec << "\n";

    // -------- RNG state ---------------------------------------------------
    f << "[rng]\n";
    f << "state " << hex_byte(rng_.state(0)) << " " << hex_byte(rng_.state(1))
      << " "      << hex_byte(rng_.state(2)) << " " << hex_byte(rng_.state(3)) << "\n";

    return true;
}

bool Game::load_game(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;

    // Reset the world so partial loads don't leave stale primaries. We
    // re-run init() to regenerate the landscape from the seed, then
    // overwrite the mutable state from the save.
    object_mgr_.init();

    // Zero out *all* primary slots (including slot 0) — the save will
    // rewrite whatever was active. This prevents leftover TRIAX at slot 1
    // (set by init) if the save doesn't contain it.
    for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        object_mgr_.object(i).y.whole = 0; // is_active() returns false
    }
    // Same for secondary.
    for (int i = 0; i < GameConstants::SECONDARY_OBJECT_SLOTS; i++) {
        object_mgr_.secondary(i).y = 0;
    }

    std::string line;
    std::string section;
    int section_index = -1;
    Object* cur_object = nullptr;
    SecondaryObject* cur_secondary = nullptr;
    std::vector<uint8_t> tertiary_buf;

    while (std::getline(f, line)) {
        // Strip comments past '#'
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);

        auto t = tokens(line);
        if (t.empty()) continue;

        // Section header?
        if (t[0].size() > 0 && t[0][0] == '[') {
            // Collapse any split tokens like "[object" "0]" back together.
            std::string hdr;
            for (auto& s : t) { if (!hdr.empty()) hdr += " "; hdr += s; }
            // hdr looks like "[object 3]" or "[player]" — strip brackets.
            if (hdr.back() == ']') hdr.pop_back();
            if (!hdr.empty() && hdr.front() == '[') hdr.erase(hdr.begin());
            // Parse section name + optional index.
            std::istringstream ss(hdr);
            ss >> section;
            section_index = -1;
            ss >> section_index;

            cur_object = nullptr;
            cur_secondary = nullptr;
            if (section == "object" && section_index >= 0 &&
                section_index < GameConstants::PRIMARY_OBJECT_SLOTS) {
                cur_object = &object_mgr_.object(section_index);
                // Clear to defaults — the save may omit fields that default
                // to zero in the Object struct.
                *cur_object = Object{};
            } else if (section == "secondary" && section_index >= 0 &&
                       section_index < GameConstants::SECONDARY_OBJECT_SLOTS) {
                cur_secondary = &object_mgr_.secondary(section_index);
                *cur_secondary = SecondaryObject{};
            }
            continue;
        }

        // Top-level "version 1" / "frame <n>"
        if (section.empty()) {
            if (t[0] == "frame" && t.size() >= 2) {
                frame_counter_ = parse_u8(t[1]);
            }
            continue;
        }

        // --- [player] ---
        if (section == "player") {
            const std::string& k = t[0];
            if      (k == "weapon")              player_weapon_ = parse_u8(t[1]);
            else if (k == "aim_angle")           player_aim_angle_ = parse_u8(t[1]);
            else if (k == "angle")               player_angle_ = parse_u8(t[1]);
            else if (k == "facing")              player_facing_ = parse_u8(t[1]);
            else if (k == "held_slot")           held_object_slot_ = parse_u8(t[1]);
            else if (k == "pockets") {
                for (int i = 0; i < 5 && i + 1 < (int)t.size(); i++)
                    pockets_[i] = parse_u8(t[i + 1]);
            }
            else if (k == "pockets_used")        pockets_used_ = parse_u8(t[1]);
            else if (k == "weapon_energy") {
                for (int i = 0; i < 6 && i + 1 < (int)t.size(); i++)
                    weapon_energy_[i] = parse_u16(t[i + 1]);
            }
            else if (k == "jetpack_active")      jetpack_active_ = parse_num(t[1]) != 0;
            else if (k == "whistle_one_active")  whistle_one_active_ = parse_num(t[1]) != 0;
            else if (k == "whistle_two_activator") whistle_two_activator_ = parse_u8(t[1]);
            else if (k == "whistle_one_collected") whistle_one_collected_ = parse_num(t[1]) != 0;
            else if (k == "whistle_two_collected") whistle_two_collected_ = parse_num(t[1]) != 0;
            else if (k == "chatter_reserve")     chatter_energy_reserve_ = parse_u8(t[1]);
            else if (k == "mushroom_timers" && t.size() >= 3) {
                player_mushroom_timers_[0] = parse_u8(t[1]);
                player_mushroom_timers_[1] = parse_u8(t[2]);
            }
            else if (k == "mushroom_immunity")   mushroom_immunity_collected_ = parse_num(t[1]) != 0;
            continue;
        }

        // --- [events] ---
        if (section == "events") {
            const std::string& k = t[0];
            if      (k == "flooding")    flooding_state_ = parse_u8(t[1]);
            else if (k == "earthquake")  earthquake_state_ = parse_u8(t[1]);
            else if (k == "robot_availability") {
                for (int i = 0; i < 4 && i + 1 < (int)t.size(); i++)
                    clawed_robot_availability_[i] = parse_u8(t[i + 1]);
            }
            else if (k == "robot_teleport") {
                for (int i = 0; i < 4 && i + 1 < (int)t.size(); i++)
                    clawed_robot_teleport_energy_[i] = parse_u8(t[i + 1]);
            }
            else if (k == "door_timer")  object_mgr_.door_timer_ = parse_u8(t[1]);
            continue;
        }

        // --- [object N] ---
        if (cur_object) {
            apply_object_field(*cur_object, t);
            continue;
        }

        // --- [secondary N] ---
        if (cur_secondary) {
            const std::string& k = t[0];
            if      (k == "type")         cur_secondary->type = parse_u8(t[1]);
            else if (k == "x")            cur_secondary->x = parse_u8(t[1]);
            else if (k == "y")            cur_secondary->y = parse_u8(t[1]);
            else if (k == "energy_fracs") cur_secondary->energy_and_fractions = parse_u8(t[1]);
            continue;
        }

        // --- [tertiary] ---
        if (section == "tertiary") {
            for (auto& tok : t) {
                tertiary_buf.push_back(static_cast<uint8_t>(std::stoul(tok, nullptr, 16)));
                if ((int)tertiary_buf.size() >= ObjectManager::TERTIARY_DATA_SIZE) break;
            }
            continue;
        }

        // --- [rng] ---
        if (section == "rng") {
            if (t[0] == "state" && t.size() >= 5) {
                rng_.seed(static_cast<uint8_t>(parse_num(t[1])),
                          static_cast<uint8_t>(parse_num(t[2])),
                          static_cast<uint8_t>(parse_num(t[3])),
                          static_cast<uint8_t>(parse_num(t[4])));
            }
            continue;
        }
    }

    // Commit tertiary buffer (pad / truncate to the exact size).
    uint8_t* td = object_mgr_.tertiary_data_ptr();
    for (int i = 0; i < ObjectManager::TERTIARY_DATA_SIZE; i++) {
        td[i] = (i < (int)tertiary_buf.size()) ? tertiary_buf[i] : 0;
    }

    return true;
}
