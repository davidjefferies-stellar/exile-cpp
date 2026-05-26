#pragma once
#include "objects/object.h"
#include "core/types.h"
#include "core/random.h"
#include <array>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

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

    // Whole primary array for callers needing to walk the slot table
    // (object-object collision, door-substitution). Replaces reinterpret_
    // cast off &object(0) which assumed contiguous std::array layout.
    const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&
        primary_array() const { return primary_; }

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

    // Tertiary state lives in Landscape now; save/load goes through
    // the entry table directly (see save_load.cpp). Public accessors
    // for the entries are exposed by Landscape — Game already has both
    // the landscape and the object manager, so save/load can reach
    // them without a passthrough here.

    // Check if an object is far from the activation anchor (see below).
    bool is_far_from_anchor(uint8_t obj_x, uint8_t obj_y, uint8_t distance) const;

    // Activation anchor: world point lifecycle checks measure against.
    // 6502 uses the player; Game::run optionally follows the camera in
    // map mode. Defaults to player -> matches the original.
    void set_activation_anchor(uint8_t x, uint8_t y) {
        activation_anchor_x_ = x;
        activation_anchor_y_ = y;
    }
    uint8_t activation_anchor_x() const { return activation_anchor_x_; }
    uint8_t activation_anchor_y() const { return activation_anchor_y_; }

    // demote_distances_[x-1]: [0]=tertiary statics, [1]=moving/airborne,
    // [2]=slow+supported. promote_distance_ drives promote_selective.
    void set_demote_distances(uint8_t tertiary, uint8_t moving,
                               uint8_t settled) {
        demote_distances_[0] = tertiary;
        demote_distances_[1] = moving;
        demote_distances_[2] = settled;
    }
    void set_promote_distance(uint8_t d) { promote_distance_ = d; }
    uint8_t promote_distance() const { return promote_distance_; }

    // Effective active size dialed down from compile-time upper bounds.
    // Used by create_object's slot search and promote_selective's shuffle;
    // iteration still walks the full backing array.
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

    // Tertiary data byte access. The "offset" parameter is a tertiary
    // entry index (0..n_tertiary_entries-1) post Option-B. Null landscape
    // -> read=0, write=no-op for safe pre-init access.
    void set_landscape(class Landscape& l) { landscape_ = &l; }

    uint8_t tertiary_data_byte(int idx) const;
    uint8_t tertiary_type_byte(int idx) const;
    void    clear_tertiary_spawn_bit(int idx);
    void    set_tertiary_data_byte(int idx, uint8_t value);

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

    // Free-form diagnostic lines, flushed by Game::flush_debug_log
    // after the per-event lines. Behaviours push here when they need
    // to log a transient state machine — currently only update_imp's
    // at-home gift-drop trace. Capped to keep memory bounded if a
    // logger is left enabled across many frames.
    static constexpr size_t DIAG_LINE_CAP = 256;
    std::vector<std::string> diag_lines_;
    void log_diag(const char* fmt, ...) {
        if (diag_lines_.size() >= DIAG_LINE_CAP) return;
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        diag_lines_.emplace_back(buf);
    }

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
        diag_lines_.clear();
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

    // Tertiary state lives in Landscape (per-cell entries with own data
    // and type bytes). ObjectManager just holds a pointer set by
    // Game::init via set_landscape(); the inline accessors above
    // forward all reads and writes to the landscape's mutable entry
    // table.
    class Landscape* landscape_ = nullptr;

    // Selective promotion state
    uint8_t secondary_update_next_ = 0;
    uint8_t secondary_update_shuffle_ = 0;
    uint8_t secondary_update_distance_ = 0;

    // Activation anchor — Game::run refreshes each frame. Defaults to (0,0)
    // which is fine since the first set_activation_anchor runs before any
    // distance check.
    uint8_t activation_anchor_x_ = 0;
    uint8_t activation_anchor_y_ = 0;

    // Cache-range radii (settable via exile.ini through set_*). See
    // check_demotion / promote_selective in object_manager.cpp for how
    // they're applied.
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
