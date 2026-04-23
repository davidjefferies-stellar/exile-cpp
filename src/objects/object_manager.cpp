#include "objects/object_manager.h"
#include "objects/object_data.h"
#include "objects/object_tables.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

void ObjectManager::init() {
    // Clear all primary slots
    for (auto& obj : primary_) {
        obj = Object{};
        obj.y.whole = 0; // Mark as inactive
    }

    // Secondary slots — load the 19 ROM-initialised entries from &0af2-&0b73.
    // These are the starting world items (grenades in bushes, the cannon,
    // the destinator, fluffy, etc.) that the player's activation radius
    // promotes to primary when they come close enough. Without this the
    // secondary list starts empty and those items never appear.
    for (int i = 0; i < static_cast<int>(GameConstants::SECONDARY_OBJECT_SLOTS); ++i) {
        SecondaryObject& sec = secondary_[i];
        sec.type = initial_secondary_type[i];
        sec.x    = initial_secondary_x[i];
        sec.y    = initial_secondary_y[i];
        sec.energy_and_fractions = initial_secondary_energy_and_fracs[i];
    }

    // Copy mutable tertiary data (creature counts etc. change at runtime)
    std::memcpy(tertiary_data_, tertiary_objects_data_bytes, sizeof(tertiary_data_));

    secondary_update_next_ = 0;
    secondary_update_shuffle_ = 0;
    secondary_update_distance_ = 0;
}

// ============================================================================
// Object Creation - port of &1e62
// ============================================================================

void ObjectManager::init_object_from_type(Object& obj, ObjectType type) {
    uint8_t idx = static_cast<uint8_t>(type);
    if (idx >= static_cast<uint8_t>(ObjectType::COUNT)) idx = 0;

    // Slots are reused; every mutable field needs resetting or stale
    // state leaks from the previous occupant into the new object. The
    // tile_collision flag is the worst offender — a bullet slot reused
    // after the previous bullet exploded inherits `tile_collision = true`
    // and the new bullet immediately re-explodes on frame 1.
    obj.type = type;
    obj.sprite = object_types_sprite[idx];
    obj.palette = object_types_palette_and_pickup[idx] & 0x7f;
    obj.energy = get_initial_energy(idx);
    obj.flags = ObjectFlags::NEWLY_CREATED | ObjectFlags::NOT_PLOTTED;
    obj.touching = 0xff; // Not touching anything
    obj.target_and_flags = 0; // Targets nothing
    obj.tertiary_data_offset = 0;
    obj.tertiary_slot = 0;
    obj.state = 0;
    obj.timer = 0;
    obj.tx = 0;
    obj.ty = 0;
    obj.velocity_x = 0;
    obj.velocity_y = 0;
    obj.tile_collision = false;
}

int ObjectManager::create_object(ObjectType type, int min_free_slots,
                                  uint8_t spawn_x, uint8_t spawn_x_frac,
                                  uint8_t spawn_y, uint8_t spawn_y_frac) {
    int slot = -1;

    if (min_free_slots > 0) {
        // Port of &1eb5: search active primary slots, count free ones. The
        // Nth free slot found (where N = min_free_slots) is used. Capped
        // at active_primary_slots_ so exile.ini's primary_slots limit is
        // honoured.
        int remaining = min_free_slots;
        for (int i = 1; i < active_primary_slots_; i++) {
            if (!primary_[i].is_active()) {
                remaining--;
                if (remaining == 0) {
                    slot = i;
                    break;
                }
            }
        }
        if (slot < 0) return -1; // Not enough free slots
    } else {
        // Find empty slot first
        slot = find_free_primary_slot();
        if (slot < 0) {
            // No free slot: find the most distant replaceable object
            uint8_t max_dist = 0;
            for (int i = 1; i < active_primary_slots_; i++) {
                const Object& obj = primary_[i];
                if (!obj.is_active()) continue;

                uint8_t idx = static_cast<uint8_t>(obj.type);
                if (idx >= static_cast<uint8_t>(ObjectType::COUNT)) continue;
                uint8_t type_flags = object_types_flags[idx];

                // Port of &1e7b-&1e82: LDA #&50; AND type_flags; CMP #&40; BNE skip
                // Only replace objects with DO_NOT_KEEP_AS_SECONDARY set
                // and KEEP_AS_TERTIARY clear (flags & 0x50) == 0x40
                uint8_t lifecycle = type_flags & (ObjectTypeFlags::DO_NOT_KEEP_AS_SECONDARY |
                                                   ObjectTypeFlags::KEEP_AS_TERTIARY);
                if (lifecycle != ObjectTypeFlags::DO_NOT_KEEP_AS_SECONDARY) continue;

                // Must not be currently plotted (bit 0 must be set)
                if (!(obj.flags & ObjectFlags::NOT_PLOTTED)) continue;

                // Chebyshev distance from the activation anchor (set by
                // Game::run; normally the player, optionally the camera).
                int8_t dx = static_cast<int8_t>(obj.x.whole - activation_anchor_x_);
                int8_t dy = static_cast<int8_t>(obj.y.whole - activation_anchor_y_);
                uint8_t abs_dx = (dx < 0) ? static_cast<uint8_t>(-dx) : static_cast<uint8_t>(dx);
                uint8_t abs_dy = (dy < 0) ? static_cast<uint8_t>(-dy) : static_cast<uint8_t>(dy);
                uint8_t dist = (abs_dx > abs_dy) ? abs_dx : abs_dy;

                if (dist > max_dist) {
                    max_dist = dist;
                    slot = i;
                }
            }
        }
    }

    if (slot < 0) return -1;

    Object& obj = primary_[slot];
    init_object_from_type(obj, type);
    obj.x = {spawn_x, spawn_x_frac};
    obj.y = {spawn_y, spawn_y_frac};

    return slot;
}

int ObjectManager::create_object_at(ObjectType type, int min_free_slots, const Object& source) {
    return create_object(type, min_free_slots,
                         source.x.whole, source.x.fraction,
                         source.y.whole, source.y.fraction);
}

// ============================================================================
// Secondary Pack/Unpack - ports of &0c6e and &0c38
// ============================================================================

// Compact format: bits 7-4 = energy & 0xF0, bits 3-2 = y_frac >> 6, bits 1-0 = x_frac >> 6
// Port of the &0b53 packed-byte layout (see comment at &0b53 in the
// disassembly): 8421.... = energy high bits, ....84.. = x fraction high
// bits, ......21 = y fraction high bits.
uint8_t ObjectManager::pack_energy_fractions(uint8_t energy, uint8_t x_frac, uint8_t y_frac) {
    uint8_t packed = energy & 0xf0;
    packed |= (x_frac >> 6) << 2;
    packed |= (y_frac >> 6);
    return packed;
}

void ObjectManager::unpack_energy_fractions(uint8_t packed, uint8_t& energy, uint8_t& x_frac, uint8_t& y_frac) {
    energy = packed & 0xf0;
    x_frac = ((packed >> 2) & 0x03) << 6;
    y_frac = (packed & 0x03) << 6;
}

void ObjectManager::demote_to_secondary(int primary_slot) {
    if (primary_slot <= 0 || primary_slot >= GameConstants::PRIMARY_OBJECT_SLOTS) return;

    const Object& obj = primary_[primary_slot];
    if (!obj.is_active()) return;
    debug_demotes_++;

    // Anti-duplicate guard: the 6502 implicitly avoids saving the same
    // tertiary entry to secondary twice because tertiary spawn cleared
    // bit 7 of the data byte after the first spawn — no respawn = no
    // chance to demote again. Our wider viewport plus eager re-spawn
    // checks can occasionally bypass that, leaving the secondary pool
    // with multiple copies of the same item (e.g. 10× PROTECTION_SUIT
    // showing up over time as the player wanders the map).
    //
    // Skip the demotion if a secondary at the same world-tile already
    // holds an object of this type — physically the "same" pickup. This
    // is a deviation from the 6502 (which has no such check) but matches
    // its observable behaviour given a working spawn gate.
    for (int i = 0; i < GameConstants::SECONDARY_OBJECT_SLOTS; i++) {
        const SecondaryObject& s = secondary_[i];
        if (s.y == 0) continue;
        if (s.type == static_cast<uint8_t>(obj.type) &&
            s.x == obj.x.whole && s.y == obj.y.whole) {
            // Existing equivalent — discard this primary instead of
            // creating a duplicate slot. Bit 7 of the matching tertiary
            // (if any) is already cleared, so it won't respawn.
            remove_object(primary_slot);
            return;
        }
    }

    int sec_slot = find_free_secondary_slot();
    if (sec_slot < 0) {
        // No free secondary slot: just remove the object
        remove_object(primary_slot);
        return;
    }

    SecondaryObject& sec = secondary_[sec_slot];
    sec.type = static_cast<uint8_t>(obj.type);
    sec.x = obj.x.whole;
    sec.y = obj.y.whole;
    sec.energy_and_fractions = pack_energy_fractions(obj.energy, obj.x.fraction, obj.y.fraction);

    // Clear primary slot
    primary_[primary_slot].y.whole = 0;
}

int ObjectManager::promote_from_secondary(int secondary_slot, int min_free_slots) {
    if (secondary_slot < 0 || secondary_slot >= GameConstants::SECONDARY_OBJECT_SLOTS) return -1;

    SecondaryObject& sec = secondary_[secondary_slot];
    if (sec.y == 0) return -1; // Empty slot

    int pri_slot = -1;
    if (min_free_slots > 0) {
        int free_count = 0;
        int first_free = -1;
        for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            if (!primary_[i].is_active()) {
                free_count++;
                if (first_free < 0) first_free = i;
            }
        }
        if (free_count < min_free_slots) return -1;
        pri_slot = first_free;
    } else {
        pri_slot = find_free_primary_slot();
    }

    if (pri_slot < 0) return -1;

    Object& obj = primary_[pri_slot];
    ObjectType type = static_cast<ObjectType>(sec.type);
    init_object_from_type(obj, type);

    obj.x.whole = sec.x;
    obj.y.whole = sec.y;

    uint8_t energy, x_frac, y_frac;
    unpack_energy_fractions(sec.energy_and_fractions, energy, x_frac, y_frac);
    obj.energy = energy;
    obj.x.fraction = x_frac;
    obj.y.fraction = y_frac;

    // Clear secondary slot
    sec.y = 0;

    return pri_slot;
}

// ============================================================================
// Promotion Modes
// ============================================================================

void ObjectManager::promote_selective(Random& rng) {
    // Port of selective promotion at &0bed-&0c4d
    // Check one random secondary object per frame
    secondary_update_next_--;
    if (secondary_update_next_ == 0xff) {
        // Wrapped: reshuffle
        secondary_update_shuffle_ = rng.next();
        secondary_update_next_ =
            static_cast<uint8_t>(active_secondary_slots_ - 1);
    }

    // The 6502 uses an XOR-shuffle then AND with (size-1); that relied on
    // a power-of-2 size. exile.ini lets the user pick any size up to the
    // compile-time max, so fall back to modulo which handles the
    // non-power-of-2 case correctly.
    int check_slot =
        (secondary_update_next_ ^ secondary_update_shuffle_) %
        active_secondary_slots_;

    if (check_slot < active_secondary_slots_) {
        const SecondaryObject& sec = secondary_[check_slot];
        if (sec.y != 0) {
            // Within promote_distance_ tiles → bring back to primary.
            if (!is_far_from_anchor(sec.x, sec.y, promote_distance_)) {
                promote_from_secondary(check_slot, 4);
            }
        }
    }
}

void ObjectManager::promote_distance_check() {
    // Port of &0c4e-&0c6d: check all secondary objects
    for (int i = active_secondary_slots_ - 1; i >= 0; i--) {
        const SecondaryObject& sec = secondary_[i];
        if (sec.y == 0) continue;

        if (!is_far_from_anchor(sec.x, sec.y, promote_distance_)) {
            promote_from_secondary(i, 1);
        }
    }
}

// ============================================================================
// Tertiary Management
// ============================================================================

void ObjectManager::return_to_tertiary(int primary_slot) {
    if (primary_slot <= 0 || primary_slot >= GameConstants::PRIMARY_OBJECT_SLOTS) return;

    debug_returns_++;
    Object& obj = primary_[primary_slot];
    uint8_t tidx = static_cast<uint8_t>(obj.type);
    uint8_t type_flags = (tidx < static_cast<uint8_t>(ObjectType::COUNT))
                         ? object_types_flags[tidx] : 0;

    // tertiary_slot is the index directly into tertiary_data_, with
    // 0 meaning "no tertiary storage" (matches the 6502's &bd convention at
    // &4083 / &4087).
    if (obj.tertiary_slot > 0 &&
        obj.tertiary_slot < sizeof(tertiary_data_)) {
        uint8_t& data = tertiary_data_[obj.tertiary_slot];

        if (type_flags & ObjectTypeFlags::SPAWNED_FROM_NEST) {
            // Return spawn to nest: increment creature count
            data += 0x04; // Add 1 creature (count is in upper bits)
        } else {
            // For doors / switches / transporter beams the primary has been
            // mutating its data byte while onscreen (locked→unlocked from a
            // switch, beam destination changed, etc.). Copy the current
            // state back so the next spawn sees it. Then set the spawn gate
            // bit 7 to re-enable creation when the tile returns to view.
            data = static_cast<uint8_t>(obj.tertiary_data_offset | 0x80);
        }
    }

    // Clear primary slot
    obj.y.whole = 0;
}

void ObjectManager::remove_object(int primary_slot) {
    if (primary_slot <= 0 || primary_slot >= GameConstants::PRIMARY_OBJECT_SLOTS) return;
    if (primary_[primary_slot].is_active()) debug_removes_++;
    primary_[primary_slot].y.whole = 0;
}

// ============================================================================
// Demotion Decision - port of &1bb7-&1d26
// ============================================================================

bool ObjectManager::check_demotion(int primary_slot, uint8_t frame_counter) {
    if (primary_slot <= 0 || primary_slot >= GameConstants::PRIMARY_OBJECT_SLOTS) return false;

    Object& obj = primary_[primary_slot];
    if (!obj.is_active()) return false;

    // Port of &1bb7-&1d5b check_demotion_or_removal. Only objects with
    // KEEP_AS_TERTIARY (&10) or KEEP_AS_PRIMARY_FOR_LONGER (&20) ever get
    // distance-checked; everything else stays primary until removed some
    // other way (e.g. energy==0 explosion path).
    uint8_t tidx = static_cast<uint8_t>(obj.type);
    uint8_t type_flags = (tidx < static_cast<uint8_t>(ObjectType::COUNT))
                         ? object_types_flags[tidx] : 0;

    // Build X = (KEEP_AS_PRIMARY_FOR_LONGER << 1) | KEEP_AS_TERTIARY
    // (matches the shift chain at &1bbe-&1bc6).
    uint8_t x = 0;
    if (type_flags & ObjectTypeFlags::KEEP_AS_TERTIARY)           x |= 0x01;
    if (type_flags & ObjectTypeFlags::KEEP_AS_PRIMARY_FOR_LONGER) x |= 0x02;
    if (x == 0) return false;  // &1bc7 BEQ skip_distance_check

    // &1bca-&1bda: for X=2 (KEEP_AS_PRIMARY_FOR_LONGER only), bump to X=3
    // (4-tile distance) when the object is slow AND supported. Moving or
    // airborne objects stick to the 12-tile range so they aren't demoted
    // while still in travel.
    if (x == 0x02) {
        uint8_t max_v = std::max<uint8_t>(
            static_cast<uint8_t>(std::abs(obj.velocity_x)),
            static_cast<uint8_t>(std::abs(obj.velocity_y)));
        bool slow = max_v < 5;
        bool supported = (obj.flags & ObjectFlags::SUPPORTED) != 0;
        if (slow && supported) x = 0x03;
    }

    // &1bdb: distances_to_remove_objects_table[X-1]. 6502 ROM was
    // {1, 12, 4}; our port exposes these as demote_distances_ so
    // exile.ini's [distances] section can tune each independently — see
    // StartupConfig::demote_tertiary / demote_moving / demote_settled.
    //
    // Port hazard: in the 6502, tertiary objects only spawned while a
    // tile was being plotted — i.e. inside the ~4-tile visible window —
    // so the 1-tile demote radius for KEEP_AS_TERTIARY statics (doors,
    // switches) bordered the spawn edge and churn was rare. Our port's
    // viewport can be much larger, so spawn_tertiary_object gates on a
    // larger radius. A small demote distance inside the spawn radius
    // would cause 1-in-4-frame churn: the object demotes, next render
    // tick re-spawns it. Keep demote_distances_[0] ≥ the spawn radius
    // so the two boundaries coincide.
    uint8_t check_distance = demote_distances_[x - 1];

    // &1bde-&1be4: gate on (per-object frame counter & 3) == 3. We don't
    // have per-object staggering so use the global counter; still fires
    // 1 in 4 frames.
    if ((frame_counter & 0x03) != 0x03) return false;

    // &1be6-&1bf4: skip the distance check for newly-created or teleporting
    // objects (they haven't had a chance to settle yet).
    if (obj.flags & (ObjectFlags::NEWLY_CREATED | ObjectFlags::TELEPORTING)) {
        return false;
    }

    if (!is_far_from_anchor(obj.x.whole, obj.y.whole, check_distance)) {
        return false;
    }

    // &1d07-&1d28: if KEEP_AS_TERTIARY or SPAWNED_FROM_NEST, update the
    // tertiary data byte (bit 7 set to respawn, +4 to increment nest count).
    // Handled inside return_to_tertiary which inspects the flags.
    bool update_tertiary =
        (type_flags & (ObjectTypeFlags::KEEP_AS_TERTIARY |
                       ObjectTypeFlags::SPAWNED_FROM_NEST)) != 0;

    // &1d3c: DO_NOT_KEEP_AS_SECONDARY skips the secondary demotion step.
    bool to_secondary =
        (type_flags & ObjectTypeFlags::DO_NOT_KEEP_AS_SECONDARY) == 0;

    if (update_tertiary) {
        return_to_tertiary(primary_slot);  // also clears the primary slot
    } else if (to_secondary) {
        demote_to_secondary(primary_slot); // also clears the primary slot
    } else {
        remove_object(primary_slot);
    }
    return true;
}

// ============================================================================
// Utility
// ============================================================================

int ObjectManager::count_active_primary() const {
    int count = 0;
    for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        if (primary_[i].is_active()) count++;
    }
    return count;
}

int ObjectManager::find_free_primary_slot() const {
    // Cap at the runtime active size — slots above it are off-limits so
    // exile.ini's [caches] primary_slots setting actually constrains how
    // many primaries exist at once, regardless of the backing array size.
    for (int i = 1; i < active_primary_slots_; i++) {
        if (!primary_[i].is_active()) return i;
    }
    return -1;
}

int ObjectManager::find_free_secondary_slot() const {
    for (int i = 0; i < active_secondary_slots_; i++) {
        if (secondary_[i].y == 0) return i;
    }
    return -1;
}

bool ObjectManager::is_far_from_anchor(uint8_t obj_x, uint8_t obj_y, uint8_t distance) const {
    // Anchor is set once per frame by Game::run. Default is the player's
    // position (matching the 6502), but "map mode" repoints it at the
    // camera centre so scrolling the viewport activates objects around it.
    int8_t dx = static_cast<int8_t>(obj_x - activation_anchor_x_);
    int8_t dy = static_cast<int8_t>(obj_y - activation_anchor_y_);

    uint8_t abs_dx = (dx < 0) ? static_cast<uint8_t>(-dx) : static_cast<uint8_t>(dx);
    uint8_t abs_dy = (dy < 0) ? static_cast<uint8_t>(-dy) : static_cast<uint8_t>(dy);

    return abs_dx > distance || abs_dy > distance;
}
