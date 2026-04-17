#include "objects/object_manager.h"
#include "objects/object_data.h"
#include "objects/object_tables.h"
#include <cstring>

void ObjectManager::init() {
    // Clear all primary slots
    for (auto& obj : primary_) {
        obj = Object{};
        obj.y.whole = 0; // Mark as inactive
    }

    // Clear all secondary slots
    for (auto& sec : secondary_) {
        sec = SecondaryObject{};
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

    obj.type = type;
    obj.sprite = object_types_sprite[idx];
    obj.palette = object_types_palette_and_pickup[idx] & 0x7f;
    obj.energy = get_initial_energy(idx);
    obj.flags = ObjectFlags::NEWLY_CREATED | ObjectFlags::NOT_PLOTTED;
    obj.touching = 0xff; // Not touching anything
    obj.target_and_flags = 0; // Targets nothing
    obj.tertiary_data_offset = 0;
    obj.state = 0;
    obj.timer = 0;
    obj.velocity_x = 0;
    obj.velocity_y = 0;
}

int ObjectManager::create_object(ObjectType type, int min_free_slots,
                                  uint8_t spawn_x, uint8_t spawn_x_frac,
                                  uint8_t spawn_y, uint8_t spawn_y_frac) {
    int slot = -1;

    if (min_free_slots > 0) {
        // Port of &1eb5: search slots 1-15, count free slots.
        // The Nth free slot found (where N = min_free_slots) is used.
        int remaining = min_free_slots;
        for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
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
            for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
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

                // Calculate Chebyshev distance from screen center
                int8_t dx = static_cast<int8_t>(obj.x.whole - primary_[0].x.whole);
                int8_t dy = static_cast<int8_t>(obj.y.whole - primary_[0].y.whole);
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
uint8_t ObjectManager::pack_energy_fractions(uint8_t energy, uint8_t x_frac, uint8_t y_frac) {
    uint8_t packed = energy & 0xf0;
    packed |= (y_frac >> 6) << 2;
    packed |= (x_frac >> 6);
    return packed;
}

void ObjectManager::unpack_energy_fractions(uint8_t packed, uint8_t& energy, uint8_t& x_frac, uint8_t& y_frac) {
    energy = packed & 0xf0;
    x_frac = (packed & 0x03) << 6;
    y_frac = ((packed >> 2) & 0x03) << 6;
}

void ObjectManager::demote_to_secondary(int primary_slot) {
    if (primary_slot <= 0 || primary_slot >= GameConstants::PRIMARY_OBJECT_SLOTS) return;

    const Object& obj = primary_[primary_slot];
    if (!obj.is_active()) return;

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
        secondary_update_next_ = GameConstants::SECONDARY_OBJECT_SLOTS - 1;
    }

    uint8_t check_slot = secondary_update_next_ ^ secondary_update_shuffle_;
    check_slot &= (GameConstants::SECONDARY_OBJECT_SLOTS - 1); // Mask to valid range

    if (check_slot < GameConstants::SECONDARY_OBJECT_SLOTS) {
        const SecondaryObject& sec = secondary_[check_slot];
        if (sec.y != 0) {
            // Check if within 4 tiles of player
            if (!is_far_from_player(sec.x, sec.y, 4)) {
                promote_from_secondary(check_slot, 4);
            }
        }
    }
}

void ObjectManager::promote_distance_check() {
    // Port of &0c4e-&0c6d: check all secondary objects
    for (int i = GameConstants::SECONDARY_OBJECT_SLOTS - 1; i >= 0; i--) {
        const SecondaryObject& sec = secondary_[i];
        if (sec.y == 0) continue;

        if (!is_far_from_player(sec.x, sec.y, 4)) {
            promote_from_secondary(i, 1);
        }
    }
}

// ============================================================================
// Tertiary Management
// ============================================================================

void ObjectManager::return_to_tertiary(int primary_slot) {
    if (primary_slot <= 0 || primary_slot >= GameConstants::PRIMARY_OBJECT_SLOTS) return;

    Object& obj = primary_[primary_slot];
    uint8_t tidx = static_cast<uint8_t>(obj.type);
    uint8_t type_flags = (tidx < static_cast<uint8_t>(ObjectType::COUNT))
                         ? object_types_flags[tidx] : 0;

    // tertiary_data_offset is the index directly into tertiary_data_, with
    // 0 meaning "no tertiary storage" (matches the 6502's &bd convention at
    // &4083 / &4087).
    if (obj.tertiary_data_offset > 0 &&
        obj.tertiary_data_offset < sizeof(tertiary_data_)) {
        uint8_t& data = tertiary_data_[obj.tertiary_data_offset];

        if (type_flags & ObjectTypeFlags::SPAWNED_FROM_NEST) {
            // Return spawn to nest: increment creature count
            data += 0x04; // Add 1 creature (count is in upper bits)
        } else {
            // Mark as present as tertiary object — bit 7 set re-enables
            // spawning when the tile comes back into view.
            data |= 0x80;
        }
    }

    // Clear primary slot
    obj.y.whole = 0;
}

void ObjectManager::remove_object(int primary_slot) {
    if (primary_slot <= 0 || primary_slot >= GameConstants::PRIMARY_OBJECT_SLOTS) return;
    primary_[primary_slot].y.whole = 0;
}

// ============================================================================
// Demotion Decision - port of &1bb7-&1d26
// ============================================================================

bool ObjectManager::check_demotion(int primary_slot, uint8_t frame_counter) {
    if (primary_slot <= 0 || primary_slot >= GameConstants::PRIMARY_OBJECT_SLOTS) return false;

    // Only check every 4 frames
    if ((frame_counter & 0x03) != 0) return false;

    Object& obj = primary_[primary_slot];
    if (!obj.is_active()) return false;

    uint8_t tidx = static_cast<uint8_t>(obj.type);
    uint8_t type_flags = (tidx < static_cast<uint8_t>(ObjectType::COUNT))
                         ? object_types_flags[tidx] : 0;
    uint8_t lifecycle = type_flags & 0x70; // bits 6-4 determine lifecycle behavior

    if (lifecycle == 0x00) {
        // Keep as primary always
        return false;
    }

    // Determine check distance based on type flags
    uint8_t check_distance;
    if (type_flags & ObjectTypeFlags::KEEP_AS_PRIMARY_FOR_LONGER) {
        check_distance = 12; // Extended range
    } else {
        check_distance = 4; // Standard range
    }

    // For types with DO_NOT_KEEP_AS_SECONDARY (&50 = return to tertiary nearby)
    if (lifecycle == 0x50) {
        check_distance = 1;
    }

    if (!is_far_from_player(obj.x.whole, obj.y.whole, check_distance)) {
        return false; // Not far enough to demote
    }

    // Object is far away - decide what to do based on flags
    if (type_flags & ObjectTypeFlags::KEEP_AS_TERTIARY) {
        return_to_tertiary(primary_slot);
        return true;
    }

    if (type_flags & ObjectTypeFlags::DO_NOT_KEEP_AS_SECONDARY) {
        // Remove entirely (don't store as secondary)
        if (type_flags & ObjectTypeFlags::SPAWNED_FROM_NEST) {
            return_to_tertiary(primary_slot);
        } else {
            remove_object(primary_slot);
        }
        return true;
    }

    // Default: demote to secondary
    demote_to_secondary(primary_slot);
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
    for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        if (!primary_[i].is_active()) return i;
    }
    return -1;
}

int ObjectManager::find_free_secondary_slot() const {
    for (int i = 0; i < GameConstants::SECONDARY_OBJECT_SLOTS; i++) {
        if (secondary_[i].y == 0) return i;
    }
    return -1;
}

bool ObjectManager::is_far_from_player(uint8_t obj_x, uint8_t obj_y, uint8_t distance) const {
    uint8_t px = primary_[0].x.whole;
    uint8_t py = primary_[0].y.whole;

    // Signed distance (handles wrapping)
    int8_t dx = static_cast<int8_t>(obj_x - px);
    int8_t dy = static_cast<int8_t>(obj_y - py);

    uint8_t abs_dx = (dx < 0) ? static_cast<uint8_t>(-dx) : static_cast<uint8_t>(dx);
    uint8_t abs_dy = (dy < 0) ? static_cast<uint8_t>(-dy) : static_cast<uint8_t>(dy);

    return abs_dx > distance || abs_dy > distance;
}
