#include "behaviours/mood.h"
#include "behaviours/path.h"
#include "core/types.h"
#include <algorithm>
#include <cstdlib>

// &27c9-&2866 check_for_npc_stimuli + &316b-&31ab tables. We mirror the
// 6502's stim/response bit pairing (mask both sides identically — the
// mirror cancels). Damage (0x10) sits BELOW explode (0x20) per the
// actual &3193 code, not the swapped disassembly comment.

namespace {

// 6502 NPC stimuli categories (&316b comment).
constexpr int kCatRedMagentaImp = 0;
constexpr int kCatRedYellowImp  = 1;
constexpr int kCatBlueCyanImp   = 2;
constexpr int kCatCyanYellowImp = 3;
constexpr int kCatRedCyanImp    = 4;
constexpr int kCatRollingRobot  = 5;
constexpr int kCatFluffy        = 6;
constexpr int kCatChatter       = 7;
constexpr int kCatGreenSlime    = 8;
constexpr int kCatRedFrogman    = 9;

// &316b npc_stimuli_types_phobia_table. Bit 7 set ⇒ also fear the player.
constexpr uint8_t kPhobia[10] = {
    0x81, 0x81, 0xba, 0xcd, 0x81, 0xa9, 0x37, 0x37, 0x8a, 0x0f
};
// &3175 npc_stimuli_types_target_table. Values >=0x80 are
// OBJECT_RANGE_* categories the 6502 resolves via &2db0; not yet ported
// -> those targets contribute nothing on this side.
constexpr uint8_t kTarget[10] = {
    0x29, 0x29, 0x55, 0x37, 0x86, 0x86, 0x86, 0x86, 0x86, 0x0f
};
// &317f npc_stimuli_types_food_table — touched-and-absorbed type.
constexpr uint8_t kFood[10] = {
    0x11, 0x2f, 0x10, 0x34, 0x30, 0x35, 0x34, 0x58, 0x58, 0x0f
};
// &3189 npc_stimuli_types_home_table. Same encoding as phobia: bit 7
// set ⇒ also count the player as home; type 0 = player itself.
constexpr uint8_t kHome[10] = {
    0x40, 0x40, 0x40, 0x40, 0x40, 0x88, 0x00, 0x00, 0x37, 0x3a
};
// &3193 npc_stimuli_types_responses_table — bit set ⇒ stimulus is
// "good" (mood +); bit clear ⇒ "bad" (mood −).
constexpr uint8_t kResponse[10] = {
    0xcc, 0xf6, 0x8c, 0x72, 0xf2, 0x76, 0x88, 0xa4, 0x1a, 0x0e
};

// 6502 ranges (object_type -> category). Returns -1 for object types
// outside any stimuli range.
int category_for_type(uint8_t t) {
    switch (t) {
        case 0x29: return kCatRedMagentaImp;
        case 0x2a: return kCatRedYellowImp;
        case 0x2b: return kCatBlueCyanImp;
        case 0x2c: return kCatCyanYellowImp;
        case 0x2d: return kCatRedCyanImp;
        case 0x1c: case 0x1d: case 0x1e: return kCatRollingRobot;
        case 0x03: return kCatFluffy;
        case 0x01: case 0x38: return kCatChatter;
        case 0x0a: return kCatGreenSlime;
        case 0x06: return kCatRedFrogman;
        default:   return -1;
    }
}

// &3bfe find_a_target -> &3c2a find_object. Reduced: Chebyshev scan,
// no range-category resolution (target_byte >=0x80 skipped).
// phobia_byte bit 7 includes player. Outputs phobia/player match flags.
int find_target(const Object& npc, UpdateContext& ctx, int self_slot,
                uint8_t phobia_byte, uint8_t target_byte, int range_tiles,
                bool& matched_player_out, bool& matched_primary_out) {
    uint8_t phobia_type = phobia_byte & 0x7f;
    bool include_player_phobia = (phobia_byte & 0x80) != 0;
    bool target_is_range = (target_byte & 0x80) != 0;
    matched_player_out = false;
    matched_primary_out = false;
    int best_slot = -1;
    int best_dist = 0x7fff;
    bool best_primary = false, best_player = false;
    // &3c4a randomises the walking order so equidistant phobia matches
    // tie-break non-deterministically. XOR the loop index with a nibble
    // of rnd so each call visits slots in a different sequence; the
    // first-best-wins comparison then yields varied picks across calls.
    uint8_t order_xor = static_cast<uint8_t>(ctx.rng.next() & 0x0f);
    for (int raw = 0; raw < GameConstants::PRIMARY_OBJECT_SLOTS; raw++) {
        int i = (raw ^ order_xor) & 0x0f;
        if (i >= GameConstants::PRIMARY_OBJECT_SLOTS) continue;
        if (i == self_slot) continue;
        const Object& other = ctx.mgr.object(i);
        if (!other.is_active()) continue;
        uint8_t t = static_cast<uint8_t>(other.type);
        bool is_player = (t == 0);
        bool primary_match = (is_player && (include_player_phobia || phobia_type == 0)) ||
                             (!is_player && t == phobia_type);
        bool target_match = !target_is_range && t == target_byte && !is_player;
        if (!primary_match && !target_match) continue;
        int8_t dx = static_cast<int8_t>(npc.x.whole - other.x.whole);
        int8_t dy = static_cast<int8_t>(npc.y.whole - other.y.whole);
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        if (adx > range_tiles || ady > range_tiles) continue;
        int d = adx > ady ? adx : ady;
        // &3cb2-&3cbd find_object LOS probe. Closed doors block stimuli
        // via point_in_tile_solid_with_doors, gating sealed-room NPCs.
        if (!NPC::has_line_of_sight_randomized(npc, static_cast<uint8_t>(i), ctx)) {
            continue;
        }
        if (d < best_dist) {
            best_dist = d;
            best_slot = i;
            best_primary = primary_match;
            best_player  = is_player && primary_match;
        }
    }
    matched_player_out  = best_player;
    matched_primary_out = best_primary;
    return best_slot;
}

} // namespace

namespace Mood {

uint8_t get_mood(const Object& npc) {
    return npc.state & NPCMood::MASK;
}

void set_mood(Object& npc, uint8_t mood) {
    npc.state = (npc.state & ~NPCMood::MASK) | (mood & NPCMood::MASK);
}

bool has_category(ObjectType type) {
    return category_for_type(static_cast<uint8_t>(type)) >= 0;
}

void update_mood(Object& npc, UpdateContext& ctx) {
    int cat = category_for_type(static_cast<uint8_t>(npc.type));
    if (cat < 0) return;

    // &1a8d-&1a97 per-object fc = ((slot<<4)|slot) + global_fc. Slots
    // fire on staggered global frames; using global_fc directly synced
    // all NPCs and masked this stagger.
    uint8_t s = static_cast<uint8_t>(ctx.this_slot);
    uint8_t per_obj_fc = static_cast<uint8_t>(
        ((s << 4) | (s & 0x0f)) + ctx.frame_counter);

    uint8_t stimuli = 0;

    // &27cf-&2810 every-64-frames target search: (1) phobia/target probe
    // sets DIRECTNESS_ONE; (2) home re-find always runs for MINUS_TWO
    // imps so fed angry imps retarget the bush. Phobia hit applies AVOID
    // first and rolls 50% to skip the home re-find (keep fleeing).
    if ((per_obj_fc & 0x3f) == 0) {
        bool was_player = false, was_primary = false;
        int hit = find_target(npc, ctx, ctx.this_slot,
                              kPhobia[cat], kTarget[cat], 16,
                              was_player, was_primary);
        if (hit >= 0) {
            // &3c03-&3c15 find_a_target's success path: store slot in
            // target_object and write DIRECTNESS_ONE to flags. Wipes
            // any old AVOID bit.
            npc.target_and_flags = static_cast<uint8_t>(
                (hit & TargetFlags::OBJECT_MASK) | TargetFlags::DIRECTNESS_ONE);

            if (was_primary) stimuli |= 0x02; // phobia bit (port mask)
            if (was_player)  stimuli |= 0x04; // player bit (port mask)
        }

        // &27e9-&2802: MINUS_TWO branch.
        if (get_mood(npc) == NPCMood::MINUS_TWO) {
            bool stim_present = (stimuli & 0x06) != 0;  // phobia or player
            bool skip_home = false;
            if (stim_present) {
                // &27f3 avoid_target — flag the existing target so the
                // walker flees instead of approaches.
                npc.target_and_flags |= TargetFlags::AVOID;
                // &27f6-&27f9: 50% rng skip on the home re-find when
                // we're already fleeing something.
                skip_home = (ctx.rng.next() & 0x80) != 0;
            }
            if (!skip_home) {
                bool _player = false, _primary = false;
                int home_slot = find_target(npc, ctx, ctx.this_slot,
                                            kHome[cat], 0xff, 16,
                                            _player, _primary);
                if (home_slot >= 0) {
                    // find_a_target's success path overwrites flags —
                    // AVOID gets cleared because home is something we
                    // approach, not flee.
                    npc.target_and_flags = static_cast<uint8_t>(
                        (home_slot & TargetFlags::OBJECT_MASK) |
                        TargetFlags::DIRECTNESS_ONE);
                }
            }
        }
    }

    // &2804-&280d: 50%-skip find_target for the variant's food so a
    // hungry NPC can target a visible food primary even when not yet
    // adjacent. Stamps target_and_flags the same way the phobia branch
    // does on a hit (DIRECTNESS_ONE, AVOID cleared).
    if ((ctx.rng.next() & 0x80) == 0) {
        bool _player = false, _primary = false;
        int food_slot = find_target(npc, ctx, ctx.this_slot,
                                    kFood[cat], 0xff, 16,
                                    _player, _primary);
        if (food_slot >= 0) {
            npc.target_and_flags = static_cast<uint8_t>(
                (food_slot & TargetFlags::OBJECT_MASK) |
                TargetFlags::DIRECTNESS_ONE);
        }
    }

    // &2812-&281c eating: touched object's type matches the variant's
    // food. Mark the food for removal so it's not re-detected next
    // frame, matching consider_absorbing_object_touched at &3bef.
    if (npc.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        Object& touched = ctx.mgr.object(npc.touching);
        if (touched.is_active() &&
            static_cast<uint8_t>(touched.type) == kFood[cat]) {
            touched.flags |= ObjectFlags::PENDING_REMOVAL;
            stimuli |= 0x08;
        }
    }

    // &2827-&282a damage: WAS_DAMAGED set by damage_object when ≥8
    // damage landed this frame. Clear after reading so the bit is a
    // one-shot stimulus, matching the 6502's "rolled out" treatment.
    // Damage maps to mask 0x10 (pairs with response bit 4).
    if (npc.flags & ObjectFlags::WAS_DAMAGED) {
        stimuli |= 0x10;
        npc.flags &= ~ObjectFlags::WAS_DAMAGED;
    }

    // &282b-&2833 explode: explosion_timer is INC'd toward 0 from -50.
    // The frame it equals -49 (0xcf) is "one tick after start". Maps to
    // mask 0x20 (pairs with response bit 5).
    if (ctx.explosion_timer && *ctx.explosion_timer == static_cast<int8_t>(0xcf)) {
        stimuli |= 0x20;
    }

    // &2834-&2839 flood: bit 7 of flooding_state set during endgame.
    if (ctx.flooding_state && (*ctx.flooding_state & 0x80)) {
        stimuli |= 0x40;
    }

    // &283a-&283e time: every 256 frames (when per-object fc == 0xff).
    // Per-object — see per_obj_fc construction above; the slot-offset
    // phasing prevents every NPC from ticking over on the same frame.
    if (per_obj_fc == 0xff) {
        stimuli |= 0x80;
    }

    // &283f AND rnd_state+1 — 1-in-2 chance per stimulus bit.
    stimuli &= ctx.rng.next();
    if (stimuli == 0) return;

    // &2843-&2854 response loop. For each set stimulus bit, look at
    // the matching response bit: set ⇒ net+1, clear ⇒ net-1.
    uint8_t response = kResponse[cat];
    int net = 0;
    for (int b = 1; b <= 7; b++) {
        uint8_t mask = static_cast<uint8_t>(1u << b);
        if (stimuli & mask) {
            net += (response & mask) ? 1 : -1;
        }
    }
    if (net == 0) return;

    // &2856-&2864 apply mood delta. Sign matters, magnitude doesn't —
    // PLUS_ONE delta (+0x40) for net positive, MINUS_ONE delta (0xc0
    // = -0x40) for net negative. ADC into state with V-flag clamp:
    // refuse the update if it would wrap past MINUS_TWO (0x80).
    int delta = (net > 0) ? 0x40 : -0x40;
    int s_signed = static_cast<int8_t>(npc.state);
    int new_signed = s_signed + delta;
    if (new_signed < -128 || new_signed > 127) return;
    int new_s = static_cast<int>(npc.state) + delta;
    npc.state = static_cast<uint8_t>(new_s & 0xff);
}

} // namespace Mood
