#include "ai/behaviors/collectable.h"
#include "core/types.h"

namespace Behaviors {

// &4B88: Generic collectable item (keys, weapons, equipment, etc.)
// Port of update_collectable_object.
//
// `energy` is overloaded as a "disturbed" flag — bit 7 SET means
// undisturbed (still pinned to spawn). The 6502 uses ASL/LSR at &4ba1
// to clear that bit when something touches the object, then BIT/BPL at
// &4ba5 to skip the rest of the routine if disturbed. Our previous
// version had this inverted: it SET bit 7 on touch, leaving the pin
// behaviour permanently active and letting collectables drift under
// gravity / tile collision (then demote to secondary, then promote, and
// so on — over time the secondary pool fills with one entry per tile
// the player has wandered through).
//
// Faithful behaviour:
//   - If the player is currently holding this object, mark it collected
//     and remove it (set_object_for_removal &2529 → PENDING_REMOVAL).
//   - If something other than the player is touching us, clear bit 7
//     of energy ("disturbed").
//   - If still undisturbed, zero our velocity and snap back to the
//     previous frame's position — the collectable is glued to its tile.
//   - If disturbed, fall through and let the main physics step take
//     over (gravity, water, tile collision).
void update_collectable(Object& obj, UpdateContext& ctx) {
    // &4b88-&4b97: if the player is currently holding this exact object
    // (held_object_slot == this_slot), it counts as collected. The 6502
    // decrements `player_collected[type - 0x51]` and triggers the
    // "set_object_for_removal" path which sets PENDING_REMOVAL — the
    // main update loop reaps the slot next frame.
    //
    // We don't yet track per-type collected counts (TODO: wire to a
    // PlayerInventory struct), but flagging PENDING_REMOVAL is enough
    // for the object to disappear and the held slot to free.
    if (ctx.held_object_slot == static_cast<uint8_t>(ctx.this_slot)) {
        obj.flags |= ObjectFlags::PENDING_REMOVAL;
        return;
    }

    // &4b9d-&4ba3: any non-self touch clears bit 7 of energy. The 6502
    // does ASL/LSR rather than AND #&7f, but the visible effect is the
    // same — the "undisturbed" flag is gone.
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        obj.energy &= 0x7f;
    }

    // &4ba5-&4ba7: if undisturbed, pin position. We don't have access to
    // a "previous position" buffer, so approximate by zeroing velocity
    // each frame — gravity in step 15 will reintroduce a downward push,
    // but as long as the object is supported the integrator will undo
    // it. Net effect: object stays put on its tile.
    bool undisturbed = (obj.energy & 0x80) != 0;
    if (undisturbed) {
        obj.velocity_x = 0;
        obj.velocity_y = 0;
    }
}

// &4158: Inactive grenade - can be picked up and thrown
void update_inactive_grenade(Object& obj, UpdateContext& ctx) {
    update_collectable(obj, ctx);
}

// &4360: Power pod — limited lifespan; pulses visibly and audibly twice
// every 16 frames (when frame_counter_sixteen < 2).
// Port of &4360-&4373:
//   JSR reduce_energy_by_one
//   LDA frame_counter_sixteen; CMP #&02
//   JSR use_damaged_palette_if_carry_clear ; flash when fc16 < 2
//   BCS leave                                ; skip sound otherwise
//   play pulsing sound
void update_power_pod(Object& obj, UpdateContext& ctx) {
    // Drive the collectable "disturbed" bookkeeping first so pickups work.
    update_collectable(obj, ctx);

    // Lifespan: reduce_energy_by_one. Energy==0 ends the object.
    if (obj.energy > 0) obj.energy--;
    if (obj.energy == 0) return;

    // frame_counter & 0x0f is the 16-frame phase. 2 of every 16 frames we
    // flash the "damaged" palette and play the pulsing sound; otherwise
    // keep the regular palette (restored by the pickup-system default).
    uint8_t fc16 = ctx.frame_counter & 0x0f;
    if (fc16 < 2) {
        // use_damaged_palette_if_carry_clear: toggle colour-3 of the
        // object's default palette (EOR #&30 on bits 4-5). We approximate
        // by XOR-ing those bits on the current palette byte.
        obj.palette ^= 0x30;
        // TODO: play pulsing sound (&436c) when audio is ported.
    }
}

// &4374: Destinator - key item for completing the game
void update_destinator(Object& obj, UpdateContext& ctx) {
    update_collectable(obj, ctx);
    // Flash more vigorously
    if (ctx.every_four_frames) {
        obj.palette ^= 0x04;
    }
}

// &43A7: Empty flask - can be filled with water
// &43A7: Empty flask - fills with water when submerged
void update_empty_flask(Object& obj, UpdateContext& ctx) {
    // Port of &43a7-&43ab: BIT this_object_in_water; BPL change_to_full
    // The original checks a flag set by the collision/water system.
    // We check if the object is below the waterline for its x position.
    // (Water tiles also count, but waterline check covers the main case.)
    if (NPC::is_underwater(obj)) {
        obj.type = ObjectType::FULL_FLASK;
        // Palette changes from cwK (empty) to cwB (full, blue tint)
        obj.palette = 0x4f; // cwB
    }
}

// &43AE: Full flask - spills water on impact, can extinguish fireballs
// Timer is used as emptying countdown (0 = not emptying, >0 = spilling)
void update_full_flask(Object& obj, UpdateContext& ctx) {
    // Check if flask was hit hard enough to start emptying
    bool start_emptying = false;

    // If touching another object with combined velocity >= 0x0A (&43b0-&43b5)
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        uint8_t max_vel = static_cast<uint8_t>(
            std::max(std::abs(static_cast<int>(obj.velocity_x)),
                     std::abs(static_cast<int>(obj.velocity_y))));
        if (max_vel >= 0x0a) {
            start_emptying = true;
        }
    }

    // Or if pre-collision velocity was >= 0x14 (hit a wall hard) (&43b7-&43bb)
    // We approximate this by checking if velocity was zeroed by collision
    // (indicating a wall hit) and was previously high
    // Simplified: check if flask is moving fast
    uint8_t speed = static_cast<uint8_t>(
        std::max(std::abs(static_cast<int>(obj.velocity_x)),
                 std::abs(static_cast<int>(obj.velocity_y))));
    if (speed >= 0x14) {
        start_emptying = true;
    }

    if (start_emptying && obj.timer == 0) {
        obj.timer = 0x10; // Start emptying countdown (16 frames)
    }

    // Process emptying
    if (obj.timer > 0) {
        // If touching a fireball: extinguish it (&43c5-&43d0)
        if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
            Object& touched = ctx.mgr.object(obj.touching);
            if (touched.type == ObjectType::FIREBALL) {
                ctx.mgr.remove_object(obj.touching);
            }
        }

        // Water particles would be spawned here (8 particles upward)
        // Particle system not yet implemented

        // Countdown
        obj.timer--;
        if (obj.timer == 0) {
            // Flask is now empty (&43e2)
            obj.type = ObjectType::EMPTY_FLASK;
            obj.palette = 0x0f; // cwK (empty flask palette)
        }
    }
}

// &4351: Control device (remote control and cannon control)
void update_control_device(Object& obj, UpdateContext& ctx) {
    update_collectable(obj, ctx);
}

// Shared coronium behavior (port of &41ca-&4209)
// Both boulders and crystals cause chain-reaction explosions on contact,
// radiation damage to the player, and glow with random palettes.
static void coronium_common(Object& obj, UpdateContext& ctx) {
    // Check if touching another coronium object -> chain explosion (&41ca-&41e8)
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS && obj.touching != 0) {
        Object& other = ctx.mgr.object(obj.touching);
        if (other.type == ObjectType::CORONIUM_BOULDER ||
            other.type == ObjectType::CORONIUM_CRYSTAL) {
            // Remove the other coronium object
            uint8_t other_weight = other.weight();
            ctx.mgr.remove_object(obj.touching);

            // Explosion duration = (this_weight + other_weight) * 2 + 3
            // Boulder weight=5, crystal weight=2
            uint8_t duration = (obj.weight() + other_weight) * 2 + 3;

            // Create explosion at this location
            int slot = ctx.mgr.create_object_at(ObjectType::EXPLOSION, 0, obj);
            if (slot >= 0) {
                ctx.mgr.object(slot).tertiary_data_offset = duration;
            }

            // This object is also consumed
            obj.energy = 0;
            return;
        }
    }

    // Radiation damage to player (&41eb-&4203)
    // If touching player directly, always damage.
    // If player is holding this object, 1 in 4 chance per frame.
    bool touching_player = (obj.touching == 0);
    bool player_holding = false;

    // Approximate held-check: player at adjacent position with same velocity
    const Object& player = ctx.mgr.player();
    int8_t dx = static_cast<int8_t>(obj.x.whole - player.x.whole);
    int8_t dy = static_cast<int8_t>(obj.y.whole - player.y.whole);
    if (std::abs(dx) <= 1 && std::abs(dy) <= 1 &&
        obj.velocity_x == player.velocity_x) {
        player_holding = true;
    }

    if (touching_player || (player_holding && (ctx.rng.next() & 0xc0) == 0)) {
        // Check radiation immunity (player_radiation_immunity_pill_collected)
        // and whether coronium is underwater (radiation blocked by water)
        bool immune = false; // Would check player inventory
        bool underwater = NPC::is_underwater(obj);

        if (!immune && !underwater) {
            // Deal 8 radiation damage to player
            NPC::damage_player_if_touching(obj, ctx.mgr.player(), 8);
        }
    }

    // Random palette: radiation glow effect (&4203-&4207)
    obj.palette = (ctx.rng.next() >> 1); // LSR clears top bit for background plotting
}

// &41CA: Coronium boulder
void update_coronium_boulder(Object& obj, UpdateContext& ctx) {
    coronium_common(obj, ctx);
}

// &41C2: Coronium crystal - also has a lifespan timer
// Timer increments by 2 each frame; explodes when it overflows (128 frames)
void update_coronium_crystal(Object& obj, UpdateContext& ctx) {
    // Lifespan countdown: timer increases by 2, explodes at overflow (&41c4-&41c8)
    obj.timer += 2;
    if (obj.timer & 0x80) {
        // Timer overflowed: explode with duration 10
        int slot = ctx.mgr.create_object_at(ObjectType::EXPLOSION, 0, obj);
        if (slot >= 0) {
            ctx.mgr.object(slot).tertiary_data_offset = 10;
        }
        obj.energy = 0;
        return;
    }

    // Then shared coronium behavior (touching, radiation, glow)
    coronium_common(obj, ctx);
}

// &4216: Alien weapon - special weapon pickup
void update_alien_weapon(Object& obj, UpdateContext& ctx) {
    update_collectable(obj, ctx);
}

// &439C: Giant block - heavy physics object
void update_giant_block(Object& obj, UpdateContext& ctx) {
    // Giant blocks just follow physics, no active behavior
}

// &43AD: Inert physics object (piano, boulder, invisible inert)
void update_inert(Object& obj, UpdateContext& ctx) {
    // Pure physics objects - no active behavior.
    // Gravity, collision, and velocity are handled by the main physics loop.
}

} // namespace Behaviors
