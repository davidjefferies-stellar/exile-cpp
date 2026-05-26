#include "objects/object_manager.h"
#include "objects/object_data.h"
#include "objects/object_tables.h"
#include "world/landscape.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

// =============================================================================
// Tertiary state accessors. Forward to Landscape's per-cell entry list.
// idx == 0 is reserved for "none" (matches the legacy data_offset==0
// sentinel that meant "no tertiary"); valid entries are 1..N.
// =============================================================================
uint8_t ObjectManager::tertiary_data_byte(int idx) const {
    if (!landscape_ || idx <= 0) return 0;
    return landscape_->tertiary_entry(idx).data;
}
uint8_t ObjectManager::tertiary_type_byte(int idx) const {
    if (!landscape_ || idx <= 0) return 0;
    return landscape_->tertiary_entry(idx).type;
}
void ObjectManager::clear_tertiary_spawn_bit(int idx) {
    if (!landscape_ || idx <= 0) return;
    TertiaryEntry& e = landscape_->tertiary_entry_mut(idx);
    e.data = static_cast<uint8_t>(e.data & 0x7f);
}
void ObjectManager::set_tertiary_data_byte(int idx, uint8_t value) {
    if (!landscape_ || idx <= 0) return;
    landscape_->tertiary_entry_mut(idx).data = value;
}

void ObjectManager::init() {
    // Clear all primary slots
    for (auto& obj : primary_) {
        obj = Object{};
        obj.y.whole = 0; // Mark as inactive
    }

    // Load 19 ROM-initialised secondary entries from &0af2-&0b73 (cannon,
    // destinator, fluffy, etc). Iterate only the source-array size (32) —
    // our backing array is larger than the 6502's, so reading past 31
    // would pull garbage and spawn phantom secondaries.
    constexpr int kInitialSecondaries =
        sizeof(initial_secondary_type) / sizeof(initial_secondary_type[0]);
    static_assert(
        kInitialSecondaries ==
            sizeof(initial_secondary_x) / sizeof(initial_secondary_x[0]),
        "secondary init tables out of sync");
    for (int i = 0; i < kInitialSecondaries; ++i) {
        SecondaryObject& sec = secondary_[i];
        sec.type = initial_secondary_type[i];
        sec.x    = initial_secondary_x[i];
        sec.y    = initial_secondary_y[i];
        sec.energy_and_fractions = initial_secondary_energy_and_fracs[i];
        // Log the ROM-seeded entries so the lifecycle log shows "PIANO was
        // already at (a3,5d) when the game started, before any promote"
        // — disambiguates starting-pool membership from runtime promotions.
        if (sec.y != 0) {
            record_debug_event(EVT_SEC_INIT, static_cast<uint8_t>(i),
                               sec.type, sec.x, sec.y);
        }
    }

    // Tertiary mutable state lives in the Landscape's entry table,
    // populated by Landscape::bake_tertiary_lookup or load_from_file
    // before init() runs.

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

    // Slots are reused; every mutable field needs resetting
    obj.type = type;
    obj.sprite = object_types_sprite[idx];
    obj.palette = object_types_palette_and_pickup[idx] & 0x7f;
    obj.energy = get_initial_energy(idx);
    // Arm the "undisturbed" pin (energy bit 7) for collectables 0x4a..0x64
    // — step 15 reads it to skip physics until update_collectable's touch
    // clears it. Upper bound excludes pre-release NPCs DOG/CRAB at 0x65+.
    if (idx >= 0x4a && idx <= 0x64) {
        obj.energy |= 0x80;
    }
    obj.flags = ObjectFlags::NEWLY_CREATED | ObjectFlags::NOT_PLOTTED;
    obj.touching = 0x80; // Not touching anything; matches struct default
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
    // Transient collision/render fields: must reset on slot reuse so a
    // freshly-spawned object doesn't inherit a wedged-supported flag
    // (causes premature demote-radius bump) or an invisible state
    // (renderer skipped the draw on the spawn frame).
    obj.bottom_collision = false;
    obj.visible = true;
    obj.has_left_home = false;
}

// min_free_slots is a spawn-priority knob, mirroring the 6502's
// create_new_object_if_{Y}_slots_free entry points (&1e5a / &1e5d / &1e60).
// N=0 takes any slot (evicts the most-distant replaceable if full); N>=1
// fails unless N slots are free and uses the Nth one found. The reserve
// works because low-N callers also iterate from slot 1 and pick the first
// free slot they hit, so skipping past N-1 leaves those slots untouched —
// and outright failing when <N are free keeps high-priority spawns alive
// when the table is full.
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

    debug_creates_++;
    record_debug_event(EVT_CREATE, static_cast<uint8_t>(slot),
                       static_cast<uint8_t>(type), spawn_x, spawn_y);
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
    // &0c32 ORA #&0f — set low nibble so promoted object reads as
    // full-fraction energy. Without this, energy is 0x0f short.
    energy = packed | 0x0f;
    x_frac = ((packed >> 2) & 0x03) << 6;
    y_frac = (packed & 0x03) << 6;
}

void ObjectManager::demote_to_secondary(int primary_slot) {
    if (primary_slot <= 0 || primary_slot >= GameConstants::PRIMARY_OBJECT_SLOTS) return;

    const Object& obj = primary_[primary_slot];
    if (!obj.is_active()) return;
    debug_demotes_++;

    // Port-only anti-dup guard. 6502 relied on the spawn-gate bit to avoid
    // re-saving the same tertiary; our wider viewport + eager respawn can
    // bypass that. Skip demotion if a matching secondary already exists
    // at this world-tile.
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

    record_debug_event(EVT_DEMOTE, static_cast<uint8_t>(primary_slot),
                       static_cast<uint8_t>(obj.type),
                       obj.x.whole, obj.y.whole);

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
    // &0c2e-&0c34: secondary's high nibble dictates bit 7. ROM places
    // some grenades pinned (&48,&56 and &84,&5b have nibble 0xf) and
    // others unpinned (&c0,&4e has nibble 0x4) — respect both.
    obj.x.fraction = x_frac;
    obj.y.fraction = y_frac;

    // Clear secondary slot
    sec.y = 0;

    debug_promotes_++;
    record_debug_event(EVT_PROMOTE, static_cast<uint8_t>(pri_slot),
                       static_cast<uint8_t>(type), obj.x.whole, obj.y.whole);
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
    record_debug_event(EVT_RETURN, static_cast<uint8_t>(primary_slot),
                       static_cast<uint8_t>(obj.type),
                       obj.x.whole, obj.y.whole);
    uint8_t tidx = static_cast<uint8_t>(obj.type);
    // Diag: imp paths into return_to_tertiary (caller-agnostic).
    if (tidx >= 0x29 && tidx <= 0x2d) {
        log_diag(
            "imp p%d RETURN_TO_TERTIARY type=0x%02x @%u,%u flags=0x%02x "
            "energy=0x%02x tslot=%u",
            primary_slot, tidx, obj.x.whole, obj.y.whole,
            obj.flags, obj.energy,
            static_cast<unsigned>(obj.tertiary_slot));
    }
    uint8_t type_flags = (tidx < static_cast<uint8_t>(ObjectType::COUNT))
                         ? object_types_flags[tidx] : 0;

    // tertiary_slot is the index into the Landscape's tertiary entry
    // table. 0 means "no tertiary storage" (matches the 6502's &bd
    // convention at &4083 / &4087); valid entries start at 1.
    if (obj.tertiary_slot > 0 && landscape_) {
        TertiaryEntry& entry =
            landscape_->tertiary_entry_mut(obj.tertiary_slot);

        if (type_flags & ObjectTypeFlags::SPAWNED_FROM_NEST) {
            // Return spawn to nest: increment creature count.
            entry.data = static_cast<uint8_t>(entry.data + 0x04);
        } else {
            // For doors / switches / transporter beams the primary has
            // been mutating its data byte while onscreen (locked →
            // unlocked from a switch, beam destination changed, etc.).
            // Copy the current state back so the next spawn sees it,
            // and set bit 7 to re-arm the spawn gate.
            entry.data = static_cast<uint8_t>(obj.tertiary_data_offset | 0x80);
        }
    }

    // Clear primary slot
    obj.y.whole = 0;
}

void ObjectManager::remove_object(int primary_slot) {
    if (primary_slot <= 0 || primary_slot >= GameConstants::PRIMARY_OBJECT_SLOTS) return;
    Object& obj = primary_[primary_slot];
    if (obj.is_active()) {
        debug_removes_++;
        record_debug_event(EVT_REMOVE, static_cast<uint8_t>(primary_slot),
                           static_cast<uint8_t>(obj.type),
                           obj.x.whole, obj.y.whole);
        uint8_t tidx = static_cast<uint8_t>(obj.type);
        if (tidx >= 0x29 && tidx <= 0x2d) {
            log_diag(
                "imp p%d REMOVE_OBJECT type=0x%02x @%u,%u flags=0x%02x "
                "energy=0x%02x",
                primary_slot, tidx, obj.x.whole, obj.y.whole,
                obj.flags, obj.energy);
        }
    }
    // Port of &1e29 remove_object_for_touching_and_targeting: walk every
    // other slot and clear any touching/target field that pointed at the
    // slot we're removing. Without this, sticky touching semantics leave
    // doors / hives / other heavy primaries pointing at empty slots after
    // the toucher dies, and update_door's gate-fail path then misreads
    // the dead slot's stale type/weight bytes.
    uint8_t self = static_cast<uint8_t>(primary_slot);
    for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; ++i) {
        if (i == primary_slot) continue;
        Object& other = primary_[i];
        if (other.touching == self) other.touching = 0x80;
        // Port of &1e34 — clear targeting too. target_and_flags low 5 bits
        // are the slot index; bits 5-7 are AVOID/DIRECTNESS. Reset to
        // self-target (slot i) so the NPC re-resolves on its next path tick.
        if ((other.target_and_flags & 0x1f) == self) {
            other.target_and_flags =
                static_cast<uint8_t>((other.target_and_flags & 0xe0) | i);
        }
    }
    obj.y.whole = 0;
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
    // when the object is slow AND any-bottom-collision, so stationary
    // objects use the tighter demote_distances_[2] radius. &1bd6 reads
    // &19 = &18 | &29e5 — object-supported objects count, not just tile.
    if (x == 0x02) {
        uint8_t max_v = std::max<uint8_t>(
            static_cast<uint8_t>(std::abs(obj.velocity_x)),
            static_cast<uint8_t>(std::abs(obj.velocity_y)));
        bool slow = max_v < 5;
        if (slow && obj.bottom_collision) x = 0x03;
    }

    // &1bdb distances_to_remove_objects_table[X-1]. ROM defaults {1,12,4};
    // configured via exile.ini's [distances] block. Port hazard: our wider
    // viewport means spawn_tertiary_object's radius can exceed demote_
    // distances_[0], causing 1-in-4-frame churn — keep [0] >= spawn radius.
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

    // Diag: imp despawn via check_demotion. Captures every variable
    // entering the gate so we can see whether x==3 (tighter radius),
    // bottom_collision, or velocities pushed an imp out too early.
    if (tidx >= 0x29 && tidx <= 0x2d) {
        log_diag(
            "imp p%d CHECK_DEMOTION fire: x=%u dist=%u tflags=0x%02x "
            "vx=%d vy=%d bot_coll=%d @%u,%u anchor=%u,%u path=%s",
            primary_slot, x, check_distance, type_flags,
            (int)obj.velocity_x, (int)obj.velocity_y,
            obj.bottom_collision ? 1 : 0,
            obj.x.whole, obj.y.whole,
            activation_anchor_x_, activation_anchor_y_,
            update_tertiary ? "tertiary" : (to_secondary ? "secondary" : "remove"));
    }

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
