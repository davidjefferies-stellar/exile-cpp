#include "game/game.h"
#include "objects/object_tables.h"
#include "particles/particle_system.h"
#include "player/input.h"
#include "rendering/debug_names.h"
#include "world/water.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>

// Save / load — text format. Landscape is deterministic from the seed,
// so we persist only mutable state (frame, RNG, player, events, 16
// primaries, 32 secondaries, 235 bytes tertiary). Hex 0x prefix, fixed
// point 0xWW.FF; sections [player][events][object N][secondary N][rng].

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
    else if (k == "tertiary_slot")    o.tertiary_slot =
                                          static_cast<uint16_t>(parse_num(v));
    else if (k == "tertiary_data")    o.tertiary_data_offset = parse_u8(v);
}

} // namespace

bool Game::save_game(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;
    write_state(f);
    return true;
}

std::string Game::snapshot() const {
    std::ostringstream f;
    write_state(f);
    return f.str();
}

bool Game::dump_ring_buffer(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;
    f << "# exile-cpp multi-frame trace\n";
    f << "# Frames written in chronological order, oldest first.\n";
    f << "frame_count " << snapshot_ring_count_ << "\n\n";
    // snapshot_ring_head_ is the next write slot; the oldest frame sits
    // there when the ring is full, otherwise the oldest is at index 0.
    size_t start = (snapshot_ring_count_ == snapshot_ring_.size())
                  ? snapshot_ring_head_ : 0;
    for (size_t i = 0; i < snapshot_ring_count_; ++i) {
        size_t idx = (start + i) % snapshot_ring_.size();
        f << "=== frame " << i << " ===\n";
        f << snapshot_ring_[idx];
        f << "\n";
    }
    return true;
}

void Game::write_state(std::ostream& f) const {
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
    f << "weapons_collected";
    for (int i = 0; i < 6; i++) f << " " << hex_byte(player_weapons_collected_[i]);
    f << "\n";
    f << "fire_immunity_collected "      << (fire_immunity_collected_ ? 1 : 0) << "\n";
    f << "radiation_immunity_collected " << (radiation_immunity_collected_ ? 1 : 0) << "\n";
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
    f << "waterline_y";
    for (int i = 0; i < 4; i++) f << " " << hex_byte(Water::get_y(i));
    f << "\n";
    f << "waterline_y_frac";
    for (int i = 0; i < 4; i++) f << " " << hex_byte(Water::get_y_fraction(i));
    f << "\n";
    f << "waterline_desired_y";
    for (int i = 0; i < 4; i++) f << " " << hex_byte(Water::get_desired_y(i));
    f << "\n";
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

    // Tertiary: each CHECK_TERTIARY cell owns a TertiaryEntry. Dump
    // live data byte only (entries 1..n; 0 is reserved sentinel) —
    // tile_and_flip / type are static, persisted in the map file.
    f << "[tertiary]\n";
    int n_entries = landscape_.tertiary_count();
    f << "count " << n_entries << "\n";
    for (int i = 0; i < n_entries; ++i) {
        f << std::hex << std::setw(2) << std::setfill('0')
          << (unsigned)landscape_.tertiary_entry(i).data;
        if ((i & 0x0f) == 0x0f) f << "\n";
        else                    f << " ";
    }
    if (n_entries == 0 || (n_entries % 16) != 0) f << "\n";
    f << std::dec << "\n";

    // -------- particles --------------------------------------------------
    // Per-frame snapshot fodder; also useful when reloading a save mid-
    // explosion. Format: one line per live particle as `vx vy xf yf x y
    // ttl cf`, decimal int8 for velocities, hex for the rest.
    f << "[particles]\n";
    f << "count " << particles_.count() << "\n";
    for (int i = 0; i < particles_.count(); ++i) {
        const Particle& p = particles_.get(i);
        f << (int)p.velocity_x << " " << (int)p.velocity_y << " "
          << hex_byte(p.x_fraction) << " " << hex_byte(p.y_fraction) << " "
          << hex_byte(p.x) << " " << hex_byte(p.y) << " "
          << hex_byte(p.ttl) << " " << hex_byte(p.colour_and_flags) << "\n";
    }
    f << "\n";

    // -------- input ------------------------------------------------------
    // The current frame's input state, captured for replay / inspection.
    // weapon_select sentinel 0xff means "no change this frame".
    {
        const InputState& s = input_.state();
        f << "[input]\n";
        f << "move_left "    << (int)s.move_left    << "\n";
        f << "move_right "   << (int)s.move_right   << "\n";
        f << "move_up "      << (int)s.move_up      << "\n";
        f << "move_down "    << (int)s.move_down    << "\n";
        f << "jetpack "      << (int)s.jetpack      << "\n";
        f << "fire "         << (int)s.fire         << "\n";
        f << "turn_around "  << (int)s.turn_around  << "\n";
        f << "lie_down "     << (int)s.lie_down     << "\n";
        f << "boost "        << (int)s.boost        << "\n";
        f << "pickup_drop "  << (int)s.pickup_drop  << "\n";
        f << "pickup "       << (int)s.pickup       << "\n";
        f << "drop "         << (int)s.drop         << "\n";
        f << "throw_obj "    << (int)s.throw_obj    << "\n";
        f << "store "        << (int)s.store        << "\n";
        f << "retrieve "     << (int)s.retrieve     << "\n";
        f << "remember_pos " << (int)s.remember_pos << "\n";
        f << "teleport "     << (int)s.teleport     << "\n";
        f << "aim_up "       << (int)s.aim_up       << "\n";
        f << "aim_down "     << (int)s.aim_down     << "\n";
        f << "aim_centre "   << (int)s.aim_centre   << "\n";
        f << "toggle_pause " << (int)s.toggle_pause << "\n";
        f << "whistle_one "  << (int)s.whistle_one  << "\n";
        f << "whistle_two "  << (int)s.whistle_two  << "\n";
        f << "weapon_select " << hex_byte(s.weapon_select) << "\n";
        f << "\n";
    }

    // -------- RNG state ---------------------------------------------------
    f << "[rng]\n";
    f << "state " << hex_byte(rng_.state(0)) << " " << hex_byte(rng_.state(1))
      << " "      << hex_byte(rng_.state(2)) << " " << hex_byte(rng_.state(3)) << "\n";
    f << "[cosmetic_rng]\n";
    f << "state " << hex_byte(cosmetic_rng_.state(0)) << " " << hex_byte(cosmetic_rng_.state(1))
      << " "      << hex_byte(cosmetic_rng_.state(2)) << " " << hex_byte(cosmetic_rng_.state(3)) << "\n";
}

bool Game::load_game(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    bool ok = read_state(f);
    if (ok) {
        // The rewind ring holds snapshots from BEFORE the load and
        // would jump the player back across the load boundary if we
        // didn't reset. restore_snapshot deliberately doesn't touch
        // the ring, so we clear only on the explicit path-based load.
        snapshot_ring_head_  = 0;
        snapshot_ring_count_ = 0;
        scrubbing_           = false;
        scrub_offset_        = 0;
    }
    return ok;
}

bool Game::restore_snapshot(const std::string& s) {
    std::istringstream f(s);
    return read_state(f);
}

bool Game::read_state(std::istream& f) {
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
    // Particles get cleared up front; we accumulate parsed entries via
    // ParticleSystem::push_raw as we read [particles].
    particles_.clear();
    InputState restored_input{};

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
            else if (k == "weapons_collected" && t.size() >= 7) {
                for (int i = 0; i < 6; i++) {
                    player_weapons_collected_[i] = parse_u8(t[i + 1]);
                }
            }
            else if (k == "jetpack_booster_collected") {
                // Back-compat for saves written before the array refactor.
                player_weapons_collected_[0] =
                    (parse_num(t[1]) != 0) ? 0x80 : 0x00;
            }
            else if (k == "fire_immunity_collected") fire_immunity_collected_ = parse_num(t[1]) != 0;
            else if (k == "radiation_immunity_collected") radiation_immunity_collected_ = parse_num(t[1]) != 0;
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
            else if (k == "waterline_y") {
                for (int i = 0; i < 4 && i + 1 < (int)t.size(); i++)
                    Water::set_y(i, parse_u8(t[i + 1]),
                                 Water::get_y_fraction(i));
            }
            else if (k == "waterline_y_frac") {
                for (int i = 0; i < 4 && i + 1 < (int)t.size(); i++)
                    Water::set_y(i, Water::get_y(i), parse_u8(t[i + 1]));
            }
            else if (k == "waterline_desired_y") {
                for (int i = 0; i < 4 && i + 1 < (int)t.size(); i++)
                    Water::set_desired_y(i, parse_u8(t[i + 1]));
            }
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
            // Skip the optional "count N" header line — it's a hint
            // for human readers, the in-memory entry count is what
            // bounds the writeback below.
            if (!t.empty() && t[0] == "count") continue;
            for (auto& tok : t) {
                tertiary_buf.push_back(
                    static_cast<uint8_t>(std::stoul(tok, nullptr, 16)));
                if ((int)tertiary_buf.size() >=
                    Landscape::TERTIARY_CAPACITY) break;
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

        // --- [cosmetic_rng] --- port-only second stream; absent from old
        // saves, in which case it keeps its init-time seed.
        if (section == "cosmetic_rng") {
            if (t[0] == "state" && t.size() >= 5) {
                cosmetic_rng_.seed(static_cast<uint8_t>(parse_num(t[1])),
                                   static_cast<uint8_t>(parse_num(t[2])),
                                   static_cast<uint8_t>(parse_num(t[3])),
                                   static_cast<uint8_t>(parse_num(t[4])));
            }
            continue;
        }

        // --- [particles] ---
        if (section == "particles") {
            if (t[0] == "count") continue;
            if (t.size() < 8) continue;
            Particle p;
            p.velocity_x       = static_cast<int8_t>(parse_num(t[0]));
            p.velocity_y       = static_cast<int8_t>(parse_num(t[1]));
            p.x_fraction       = parse_u8(t[2]);
            p.y_fraction       = parse_u8(t[3]);
            p.x                = parse_u8(t[4]);
            p.y                = parse_u8(t[5]);
            p.ttl              = parse_u8(t[6]);
            p.colour_and_flags = parse_u8(t[7]);
            particles_.push_raw(p);
            continue;
        }

        // --- [input] ---
        if (section == "input") {
            const std::string& k = t[0];
            bool v = parse_num(t[1]) != 0;
            if      (k == "move_left")    restored_input.move_left    = v;
            else if (k == "move_right")   restored_input.move_right   = v;
            else if (k == "move_up")      restored_input.move_up      = v;
            else if (k == "move_down")    restored_input.move_down    = v;
            else if (k == "jetpack")      restored_input.jetpack      = v;
            else if (k == "fire")         restored_input.fire         = v;
            else if (k == "turn_around")  restored_input.turn_around  = v;
            else if (k == "lie_down")     restored_input.lie_down     = v;
            else if (k == "boost")        restored_input.boost        = v;
            else if (k == "pickup_drop")  restored_input.pickup_drop  = v;
            else if (k == "pickup")       restored_input.pickup       = v;
            else if (k == "drop")         restored_input.drop         = v;
            else if (k == "throw_obj")    restored_input.throw_obj    = v;
            else if (k == "store")        restored_input.store        = v;
            else if (k == "retrieve")     restored_input.retrieve     = v;
            else if (k == "remember_pos") restored_input.remember_pos = v;
            else if (k == "teleport")     restored_input.teleport     = v;
            else if (k == "aim_up")       restored_input.aim_up       = v;
            else if (k == "aim_down")     restored_input.aim_down     = v;
            else if (k == "aim_centre")   restored_input.aim_centre   = v;
            else if (k == "toggle_pause") restored_input.toggle_pause = v;
            else if (k == "whistle_one")  restored_input.whistle_one  = v;
            else if (k == "whistle_two")  restored_input.whistle_two  = v;
            else if (k == "weapon_select") restored_input.weapon_select = parse_u8(t[1]);
            continue;
        }
    }
    input_.set_state(restored_input);

    // Commit tertiary data bytes back into the live landscape entries.
    // Entries beyond the saved buffer's length keep whatever value
    // bake / load_from_file populated — typically the ROM default
    // with bit 7 (spawn gate) still set.
    int n_entries = landscape_.tertiary_count();
    for (int i = 0; i < n_entries && i < (int)tertiary_buf.size(); ++i) {
        landscape_.tertiary_entry_mut(i).data = tertiary_buf[i];
    }

    return true;
}

// --------------------------------------------------------------------------
// BBC-format save loader
// --------------------------------------------------------------------------
//
// On-disk format documented in docs/save_game_format.md. Briefly:
//   - exactly 0x400 bytes
//   - the first 0x37e are XOR-streamed (BCD-keyed); the trailing 0x82
//     are page-align padding
//   - decrypted layout maps to supervisor memory &1a20..&1d9d
//
// We expose this via Game::load_bbc_save so the ini key
// [player] bbc_save = ... can drop the player straight into a BBC-era
// save on startup. The text save_game / load_game pair is unchanged.

namespace {

// 6502 BCD ADC. Returns new accumulator, updates carry by reference.
static uint8_t bcd_adc(uint8_t a, uint8_t m, uint8_t& carry) {
    int lo = (a & 0x0f) + (m & 0x0f) + carry;
    if (lo >= 10) lo += 6;
    int hi = (a >> 4) + (m >> 4) + (lo >= 0x10 ? 1 : 0);
    if (hi >= 10) { hi += 6; carry = 1; } else { carry = 0; }
    return static_cast<uint8_t>(((hi & 0x0f) << 4) | (lo & 0x0f));
}

// Port of supervisor &2f80 decrypt_temporary_copy_of_game_state. The
// cipher is symmetric (XOR), so the same routine encrypts on the way
// out. Only the first 0x37e bytes are run through it.
static void bbc_decrypt(uint8_t* buf) {
    uint8_t key = 0x6e;        // &0b
    uint8_t a   = 0x92;
    uint8_t carry = 1;         // SEC at &2f92
    for (int y = 0; y < 0x37e; ++y) {
        a = bcd_adc(a, key, carry);     // ADC &0b
        a = bcd_adc(a, 0x15, carry);    // ADC #&15
        key = a;
        uint8_t plain = a ^ buf[y];     // EOR &0400,Y
        buf[y] = plain;                 // STA &1a20,Y
        a = plain ^ key;                // EOR &0b — feeds next iteration
    }
}

// LE little-endian helpers — reading 16-/32-bit fields from offsets.
static uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

} // namespace

bool Game::load_bbc_save(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    if (raw.size() != 0x400) return false;

    bbc_decrypt(raw.data());

    // Reject malformed saves the same way the supervisor does
    // (&2fbc-&2fc1): the last entry of player_teleports_x / _y must be
    // the spawn pair (0x99, 0x3c).
    if (raw[0x2f] != 0x99 || raw[0x34] != 0x3c) return false;

    // Verify checksum_one — XOR of bytes [0x000, 0x35a) starting from
    // initial value 0xdc (the supervisor's loop seed at &2fd8).
    uint8_t cs = 0xdc;
    for (int i = 0; i < 0x35a; ++i) cs ^= raw[i];
    if (cs != raw[0x35a]) return false;

    const uint8_t* d = raw.data();

    // Reset the world before populating from the save (same dance as
    // read_state — clear all primaries, all secondaries, all
    // particles, the rewind ring).
    object_mgr_.init();
    for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        object_mgr_.object(i) = Object{};
        object_mgr_.object(i).y.whole = 0;
    }
    for (int i = 0; i < GameConstants::SECONDARY_OBJECT_SLOTS; i++) {
        object_mgr_.secondary(i) = SecondaryObject{};
    }
    particles_.clear();
    snapshot_ring_head_  = 0;
    snapshot_ring_count_ = 0;
    scrubbing_           = false;
    scrub_offset_        = 0;

    // -- Top-level scalars --
    rng_.seed(d[0x000], d[0x001], d[0x002], d[0x003]);
    held_object_slot_      = d[0x004];
    player_angle_          = d[0x005];
    player_facing_         = d[0x006];
    // d[0x007..0x00a] = game_time (4 bytes) — our port doesn't track this.
    player_deaths_         = static_cast<uint16_t>(
        d[0x00b] | (d[0x00c] << 8)); // top byte (d[0x00d]) is unused

    // Keys (8 entries, &1a2e)
    for (int i = 0; i < 8; i++) player_keys_collected_[i] = d[0x00e + i];

    // Weapons (6 entries) + immunity / whistle / radiation flags (5)
    for (int i = 0; i < 6; i++) player_weapons_collected_[i] = d[0x016 + i];
    fire_immunity_collected_     = (d[0x01c] != 0);
    mushroom_immunity_collected_ = (d[0x01d] != 0);
    whistle_one_collected_       = (d[0x01e] != 0);
    whistle_two_collected_       = (d[0x01f] != 0);
    radiation_immunity_collected_= (d[0x020] != 0);

    object_mgr_.door_timer_      = d[0x021];
    player_mushroom_timers_[0]   = d[0x022];
    player_mushroom_timers_[1]   = d[0x023];
    chatter_energy_reserve_      = d[0x024];
    explosion_timer_             = static_cast<int8_t>(d[0x025]);
    flooding_state_              = d[0x026];
    earthquake_state_            = d[0x027];
    // d[0x028] = unused
    player_next_teleport_        = d[0x029];
    player_teleports_remembered_ = d[0x02a];
    for (int i = 0; i < 5; i++) player_teleports_x_[i] = d[0x02b + i];
    for (int i = 0; i < 5; i++) player_teleports_y_[i] = d[0x030 + i];
    // d[0x035] = copy_protection_third_byte — port ignores

    // Waterline (4 ranges)
    for (int i = 0; i < 4; i++) {
        Water::set_y(i, d[0x03a + i], d[0x036 + i]);
        Water::set_desired_y(i, d[0x03e + i]);
    }

    // Imp gifts + clawed robots
    for (int i = 0; i < 5; i++) imp_gifts_remaining_[i] = d[0x042 + i];
    for (int i = 0; i < 4; i++) clawed_robot_availability_[i]   = d[0x047 + i];
    for (int i = 0; i < 4; i++) clawed_robot_teleport_energy_[i] = d[0x04b + i];

    // Pockets + selected weapon
    pockets_used_ = d[0x04f];
    for (int i = 0; i < 5; i++) pockets_[i] = d[0x050 + i];
    player_weapon_ = d[0x055];

    // Weapon energies (lo+hi byte pair per slot, big-endian when packed)
    for (int i = 0; i < 6; i++) {
        weapon_energy_[i] = static_cast<uint16_t>(
            d[0x056 + i] | (d[0x05c + i] << 8));
    }
    // d[0x062..0x067] = weapons_energy_cost — port hard-codes these
    // (object_tables.h::weapon_energy_cost), so we just verify-and-skip.

    // -- 16 primary slots --
    // Per-field parallel arrays starting at offset 0x068. Note the BBC
    // saves objects_x_fraction as 17 entries and objects_x as 18 (same
    // for y) — we read the first 16 of each, which corresponds to the
    // live primary slots in the 6502.
    for (int i = 0; i < 16 && i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        Object& o = object_mgr_.object(i);
        o.type            = static_cast<ObjectType>(d[0x068 + i]);
        o.sprite          = d[0x078 + i];
        o.x.fraction      = d[0x088 + i];
        o.x.whole         = d[0x099 + i];
        o.y.fraction      = d[0x0ab + i];
        o.y.whole         = d[0x0bc + i];
        o.flags           = d[0x0ce + i];
        o.palette         = d[0x0de + i];
        o.velocity_x      = static_cast<int8_t>(d[0x0ee + i]);
        o.velocity_y      = static_cast<int8_t>(d[0x0fe + i]);
        o.target_and_flags= d[0x10e + i];
        o.tx              = d[0x11e + i];
        o.energy          = d[0x12e + i];
        o.ty              = d[0x13e + i];
        o.touching        = d[0x14e + i];
        o.timer           = d[0x15e + i];
        // Semantic mismatch: BBC's `objects_tertiary_data_offset` byte
        // is an INDEX into the 235-byte tertiary_objects_data array
        // (the suffix "offset" was literal). Our Object stores the
        // DATA VALUE inline (the legacy name kept). Dereference here
        // — without it, update_placeholder reads the index as the
        // convert-to type and turns a fire_immunity placeholder
        // (index 0x18 -> value 0x5f) into a pistol bullet (0x18).
        // Index 0 in the BBC means "no tertiary data byte" (&404b BEQ).
        uint8_t bbc_data_idx = d[0x16e + i];
        if (bbc_data_idx == 0 || bbc_data_idx >= 0xeb) {
            o.tertiary_data_offset = 0;
        } else {
            o.tertiary_data_offset = d[0x18e + bbc_data_idx];
        }
        o.state           = d[0x17e + i];
        // Link this primary back to our port's per-cell tertiary entry
        // ONLY when the BBC saved a non-zero data_offset. The 6502's
        // &404b BEQ treats objects_tertiary_data_offset == 0 as "not
        // spawned from a tertiary" — free primaries (held items the
        // player dropped, projectiles, hive spawns) shouldn't get
        // re-linked just because their tile happens to have a tertiary.
        // Without this gate, dropped flasks were getting glued to the
        // landscape and skipping physics updates that normal free
        // primaries get.
        if (bbc_data_idx != 0 && o.y.whole != 0) {
            uint16_t cell_entry = landscape_.tertiary_index_at(
                o.x.whole, o.y.whole);
            o.tertiary_slot = (cell_entry == Landscape::NO_TERTIARY)
                                ? 0 : cell_entry;
        } else {
            o.tertiary_slot = 0;
        }
        o.tile_collision         = false;
        o.pre_collision_magnitude= 0;
        o.pre_collision_angle    = 0;
    }

    // -- Tertiary data bytes --
    // The BBC dumps the 235-byte `tertiary_objects_data` array directly
    // at offset 0x18e. Its index space is the DATA-array index, not
    // the source-table index. The translation at bake time was:
    //   data_idx = (source_idx + tertiary_data_offset[tile_type]) & 0xff
    // (see bake_tertiary_lookup() at &05dd). Our port has already
    // applied that translation when populating TertiaryEntry.data from
    // ROM, so to overlay the save we re-derive data_idx per cell and
    // read the saved byte at that index.
    for (int y = 0; y < Landscape::WORLD_SIZE; ++y) {
        for (int x = 0; x < Landscape::WORLD_SIZE; ++x) {
            uint16_t entry_idx = landscape_.tertiary_index_at(
                static_cast<uint8_t>(x), static_cast<uint8_t>(y));
            if (entry_idx == Landscape::NO_TERTIARY) continue;
            uint16_t source = landscape_.tertiary_source_idx_at(
                static_cast<uint8_t>(x), static_cast<uint8_t>(y));
            if (source == Landscape::NO_TERTIARY) continue;
            uint8_t tile_type = landscape_.get_tile(
                static_cast<uint8_t>(x), static_cast<uint8_t>(y))
                & TileFlip::TYPE_MASK;
            if (tile_type >= 9) continue;  // not a CHECK_TERTIARY tile
            uint8_t data_idx = static_cast<uint8_t>(
                static_cast<int>(source) +
                static_cast<int8_t>(tertiary_data_offset[tile_type]));
            if (data_idx >= 0xeb) continue;
            landscape_.tertiary_entry_mut(entry_idx).data =
                d[0x18e + data_idx];
        }
    }

    // -- 32 secondary slots --
    for (int i = 0; i < 32 && i < GameConstants::SECONDARY_OBJECT_SLOTS; i++) {
        SecondaryObject& s = object_mgr_.secondary(i);
        s.x                    = d[0x2fa + i];
        s.y                    = d[0x31a + i];
        s.type                 = d[0x33a + i];
        s.energy_and_fractions = d[0x35b + i];
    }

    // d[0x37b] = secondary_object_update_next_object — our port doesn't
    //            mirror this scheduler byte; promotion is per-frame.
    // d[0x37c] = secondary_object_update_random_shuffle — same.
    // d[0x37d] = checksum_two — not verified in the disasm; skipped.

    // Reset transient inputs so the player isn't stuck holding a key
    // from before the load.
    input_.set_state(InputState{});

    // Frame counter: BBC saves don't store a frame counter directly
    // (only game_time at 1/50s ticks). Start from 0; the rewind ring is
    // already cleared above so nothing references the prior counter.
    frame_counter_ = 0;

    return true;
}

// Lowercase ext in place. Lambdas are banned by CLAUDE.md so the
// std::transform target is a named static helper.
static char to_lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

// Natural compare: split each path into runs of digits vs non-digits and
// compare runs lexically unless both are digits, in which case compare
// numerically. Makes "10.sav" sort after "2.sav" instead of after "1.sav".
static bool natural_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        bool a_dig = a[i] >= '0' && a[i] <= '9';
        bool b_dig = b[j] >= '0' && b[j] <= '9';
        if (a_dig && b_dig) {
            // Skip leading zeros on both runs.
            while (i < a.size() && a[i] == '0') ++i;
            while (j < b.size() && b[j] == '0') ++j;
            size_t a_start = i, b_start = j;
            while (i < a.size() && a[i] >= '0' && a[i] <= '9') ++i;
            while (j < b.size() && b[j] >= '0' && b[j] <= '9') ++j;
            size_t a_len = i - a_start, b_len = j - b_start;
            if (a_len != b_len) return a_len < b_len;     // shorter -> smaller
            int cmp = a.compare(a_start, a_len, b, b_start, b_len);
            if (cmp != 0) return cmp < 0;
        } else {
            char ca = to_lower_ascii(a[i]);
            char cb = to_lower_ascii(b[j]);
            if (ca != cb) return ca < cb;
            ++i; ++j;
        }
    }
    return a.size() < b.size();
}

// Recursive directory walk under `root`. Returns paths to *.sav files
// sorted numerically (so 1.sav, 2.sav, ..., 10.sav, 11.sav). Forward-
// slashed so the renderer's label-strip works on both Windows and Unix.
std::vector<std::string> Game::scan_save_files(const std::string& root) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(
                       root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path& p = it->path();
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), to_lower_ascii);
        if (ext != ".sav") continue;
        out.push_back(p.generic_string());
    }
    std::sort(out.begin(), out.end(), natural_less);
    return out;
}

