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

    // View of the secondary slot array. The mutable overload is used by
    // save/load to rewrite the whole pool.
    const SecondaryObject& secondary(int slot) const { return secondary_[slot]; }
    SecondaryObject&       secondary(int slot)       { return secondary_[slot]; }

    // Direct access to the tertiary data byte array for save/load. Size is
    // fixed at 235 bytes (see the array member below).
    uint8_t* tertiary_data_ptr()       { return tertiary_data_; }
    const uint8_t* tertiary_data_ptr() const { return tertiary_data_; }
    static constexpr int TERTIARY_DATA_SIZE = 235;

    // Check if an object is far from the activation anchor (see below).
    bool is_far_from_anchor(uint8_t obj_x, uint8_t obj_y, uint8_t distance) const;

    // ========================================================================
    // Activation anchor
    // ========================================================================
    // The anchor is the world point distance-based lifecycle checks (demotion,
    // promotion, placeholder conversion) measure against. By default the 6502
    // uses the player; Game::run updates this each frame so it can optionally
    // follow the camera centre when the user has scrolled the viewport off
    // the player ("map mode"). When left at the player's position the
    // behaviour matches the original exactly.
    void set_activation_anchor(uint8_t x, uint8_t y) {
        activation_anchor_x_ = x;
        activation_anchor_y_ = y;
    }
    uint8_t activation_anchor_x() const { return activation_anchor_x_; }
    uint8_t activation_anchor_y() const { return activation_anchor_y_; }

    // ========================================================================
    // Cache-range radii (settable from exile.ini — see StartupConfig).
    // ========================================================================
    // check_demotion reads demote_distances_[x-1] (x in {1,2,3}); mapping:
    //   [0] KEEP_AS_TERTIARY (statics — doors, switches)
    //   [1] KEEP_AS_PRIMARY_FOR_LONGER, moving/airborne
    //   [2] KEEP_AS_PRIMARY_FOR_LONGER, slow + supported
    // promote_distance_ is used by promote_selective / promote_distance_check
    // to decide whether a secondary is close enough to repromote.
    void set_demote_distances(uint8_t tertiary, uint8_t moving,
                               uint8_t settled) {
        demote_distances_[0] = tertiary;
        demote_distances_[1] = moving;
        demote_distances_[2] = settled;
    }
    void set_promote_distance(uint8_t d) { promote_distance_ = d; }
    uint8_t promote_distance() const { return promote_distance_; }

    // ========================================================================
    // Cache sizes (settable from exile.ini — see StartupConfig).
    // ========================================================================
    // The backing arrays are sized at compile time to the generous upper
    // bounds in GameConstants; these setters dial down the effective
    // "active size" used by create_object's slot search and promote_
    // selective's shuffle. Iteration in collision and update loops still
    // walks the whole backing array but skips inactive slots, so raising
    // the active size above the 6502's 16 / 32 just makes more simultaneous
    // primaries / secondaries possible without code changes.
    void set_active_primary_slots(int n) {
        if (n < 1) n = 1;  // need at least slot 0 (the player)
        if (n > GameConstants::PRIMARY_OBJECT_SLOTS)
            n = GameConstants::PRIMARY_OBJECT_SLOTS;
        active_primary_slots_ = n;
    }
    void set_active_secondary_slots(int n) {
        if (n < 1) n = 1;
        if (n > GameConstants::SECONDARY_OBJECT_SLOTS)
            n = GameConstants::SECONDARY_OBJECT_SLOTS;
        active_secondary_slots_ = n;
    }
    int active_primary_slots()   const { return active_primary_slots_; }
    int active_secondary_slots() const { return active_secondary_slots_; }

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
    // Whole-byte write, used by process_switch_effects when a switch toggles
    // bits in another object's tertiary data slot. Preserves no fields — the
    // caller is responsible for keeping / stripping bit 7 as appropriate.
    void set_tertiary_data_byte(int offset, uint8_t value) {
        if (offset > 0 && offset < static_cast<int>(sizeof(tertiary_data_))) {
            tertiary_data_[offset] = value;
        }
    }

    // ========================================================================
    // Per-frame lifecycle counters (debug)
    // ========================================================================
    // Incremented from return_to_tertiary / remove_object / demote_to_secondary
    // / promote_from_secondary so the HUD can tell us which path is clearing
    // or creating primaries each frame. Game::run resets these at the top of
    // each non-paused tick.
    uint32_t debug_returns_  = 0;
    uint32_t debug_removes_  = 0;
    uint32_t debug_demotes_  = 0;
    uint32_t debug_promotes_ = 0;         // secondary -> primary promotions
    uint32_t debug_creates_  = 0;         // direct create_object calls
    uint32_t debug_switch_presses_ = 0;   // lifetime press count (not reset)

    // Per-frame lifecycle event log — populated by create / promote /
    // demote / return / remove sites so the HUD can show WHICH specific
    // primary slot + object type is churning, not just how many times
    // per frame. Reset at the top of each non-paused tick. Fixed small
    // cap; overflow entries are dropped (counters still tick).
    enum DebugEventKind : uint8_t {
        EVT_CREATE   = 1,   // create_object: new primary (tertiary spawn,
                            // nest spawn, random event, firing, etc.)
        EVT_PROMOTE  = 2,   // secondary -> primary
        EVT_DEMOTE   = 3,   // primary    -> secondary
        EVT_RETURN   = 4,   // primary    -> tertiary
        EVT_REMOVE   = 5,   // primary    -> gone
        EVT_FLIP     = 6,   // horizontal sprite flip toggled. Uses x
                            // field for velocity_x (signed), y field
                            // for new facing (0=right, 1=left).
        EVT_SEC_INIT = 7,   // ROM-seeded secondary entry. Fired once per
                            // non-empty slot during ObjectManager::init so
                            // the lifecycle log shows the starting pool.
                            // Slot field reuses the secondary index.
    };
    struct DebugEvent {
        uint8_t kind;
        uint8_t slot;   // primary slot (or secondary for EVT_PROMOTE source)
        uint8_t type;   // ObjectType value
        uint8_t x;
        uint8_t y;
    };
    static constexpr int DEBUG_EVENT_CAP = 24;
    DebugEvent debug_events_[DEBUG_EVENT_CAP] = {};
    uint8_t    debug_events_n_ = 0;

    // Port of &0819 door_timer — the 6502's single global "hold door open"
    // countdown. update_door reads/writes this; the main loop decrements it
    // once per frame (same place as the mushroom timers at &19d4-&19dd,
    // which iterate X=2,1,0 — the X=0 case lands on &0819).
    uint8_t door_timer_ = 0;
    void reset_debug_counters() {
        debug_returns_  = 0;
        debug_removes_  = 0;
        debug_demotes_  = 0;
        debug_promotes_ = 0;
        debug_creates_  = 0;
        debug_events_n_ = 0;
    }
    void record_debug_event(uint8_t kind, uint8_t slot, uint8_t type,
                            uint8_t x, uint8_t y) {
        if (debug_events_n_ >= DEBUG_EVENT_CAP) return;
        debug_events_[debug_events_n_++] = {kind, slot, type, x, y};
    }

    // Initialise a primary-slot Object from the 6502 type tables. Used by
    // `Game::init` to place the initial ROM-defined objects (TRIAX at
    // slot 1, etc.) into specific slots, and by `create_object` /
    // `promote_from_secondary` internally.
    void init_object_from_type(Object& obj, ObjectType type);

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

    // Activation anchor — Game::run refreshes each frame. Defaults to (0,0)
    // which is fine since the first set_activation_anchor runs before any
    // distance check.
    uint8_t activation_anchor_x_ = 0;
    uint8_t activation_anchor_y_ = 0;

    // Cache-range radii (settable via exile.ini through set_*). Defaults
    // match the values we used to have hard-coded; see check_demotion /
    // promote_selective in object_manager.cpp for how they're applied.
    uint8_t demote_distances_[3] = { 12, 12, 4 };
    uint8_t promote_distance_    = 4;

    // Runtime cache sizes. Default to the 6502 ROM values so behaviour
    // matches the original unless exile.ini raises them.
    int active_primary_slots_   = 16;
    int active_secondary_slots_ = 32;

    // Pack/unpack secondary compact format
    static uint8_t pack_energy_fractions(uint8_t energy, uint8_t x_frac, uint8_t y_frac);
    static void unpack_energy_fractions(uint8_t packed, uint8_t& energy, uint8_t& x_frac, uint8_t& y_frac);
};
