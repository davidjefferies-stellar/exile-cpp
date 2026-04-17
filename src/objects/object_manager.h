#pragma once
#include "objects/object.h"
#include "core/types.h"
#include "core/random.h"
#include <array>

class ObjectManager {
public:
    ObjectManager() = default;

    // Initialize all three tiers. Call once at game start.
    void init();

    // Access primary objects
    Object& player() { return primary_[0]; }
    const Object& player() const { return primary_[0]; }
    Object& object(int slot) { return primary_[slot]; }
    const Object& object(int slot) const { return primary_[slot]; }

    // ========================================================================
    // Object Creation (port of &1e62)
    // ========================================================================

    // Create a new primary object. Returns slot index (1-15) or -1 on failure.
    // min_free_slots: minimum free slots required to create (0 = allow replacing furthest)
    // The new object is placed at (spawn_x, spawn_y) with default properties from tables.
    int create_object(ObjectType type, int min_free_slots,
                      uint8_t spawn_x, uint8_t spawn_x_frac,
                      uint8_t spawn_y, uint8_t spawn_y_frac);

    // Create with position copied from another object
    int create_object_at(ObjectType type, int min_free_slots, const Object& source);

    // ========================================================================
    // Secondary Object Management (ports of &0c6e and &0c38)
    // ========================================================================

    // Demote a primary object to secondary storage.
    // Packs position + energy into compact format.
    void demote_to_secondary(int primary_slot);

    // Promote a secondary object to primary.
    // Returns primary slot or -1 if no free slot.
    int promote_from_secondary(int secondary_slot, int min_free_slots);

    // Selective promotion: check one random secondary per frame (port of &0be8).
    void promote_selective(Random& rng);

    // Distance-based promotion: check all secondary objects (port of &0c4e).
    void promote_distance_check();

    // ========================================================================
    // Tertiary Object Management
    // ========================================================================

    // Return a primary object to the tertiary list.
    // Increments creature count if applicable.
    void return_to_tertiary(int primary_slot);

    // Remove a primary object entirely (set y=0).
    void remove_object(int primary_slot);

    // ========================================================================
    // Demotion Decision (port of &1bb7-&1d26)
    // ========================================================================

    // Check if a primary object should be demoted based on distance from player.
    // Returns true if the object was demoted/removed.
    bool check_demotion(int primary_slot, uint8_t frame_counter);

    // ========================================================================
    // Utility
    // ========================================================================

    int count_active_primary() const;
    int find_free_primary_slot() const;
    int find_free_secondary_slot() const;

    // Check if an object is far from the player
    bool is_far_from_player(uint8_t obj_x, uint8_t obj_y, uint8_t distance) const;

    // ========================================================================
    // Tertiary data byte access (used by the tile update routines)
    // ========================================================================
    // Each tertiary object may have a mutable data byte — its top bit is set
    // while the object still lives in tertiary storage, cleared once a
    // primary object has been spawned from it (&4089).
    uint8_t tertiary_data_byte(int offset) const {
        return (offset > 0 && offset < static_cast<int>(sizeof(tertiary_data_)))
             ? tertiary_data_[offset] : 0;
    }
    void clear_tertiary_spawn_bit(int offset) {
        if (offset > 0 && offset < static_cast<int>(sizeof(tertiary_data_))) {
            tertiary_data_[offset] &= 0x7f;
        }
    }

private:
    std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS> primary_;
    std::array<SecondaryObject, GameConstants::SECONDARY_OBJECT_SLOTS> secondary_;

    // Tertiary objects: mutable because creature counts change
    // We store copies of the original data that get modified at runtime
    uint8_t tertiary_data_[235];  // Mutable copy of tertiary_objects_data_bytes

    // Selective promotion state
    uint8_t secondary_update_next_ = 0;
    uint8_t secondary_update_shuffle_ = 0;
    uint8_t secondary_update_distance_ = 0;

    // Initialize object from type tables
    void init_object_from_type(Object& obj, ObjectType type);

    // Pack/unpack secondary compact format
    static uint8_t pack_energy_fractions(uint8_t energy, uint8_t x_frac, uint8_t y_frac);
    static void unpack_energy_fractions(uint8_t packed, uint8_t& energy, uint8_t& x_frac, uint8_t& y_frac);
};
