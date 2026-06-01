#pragma once
#include "objects/object.h"
#include "behaviours/npc_helpers.h"

class Landscape;

namespace Behaviors {

void update_active_grenade(Object& obj, UpdateContext& ctx);
void update_icer_bullet(Object& obj, UpdateContext& ctx);
void update_tracer_bullet(Object& obj, UpdateContext& ctx);
void update_cannonball(Object& obj, UpdateContext& ctx);
void update_blue_death_ball(Object& obj, UpdateContext& ctx);
void update_red_bullet(Object& obj, UpdateContext& ctx);
void update_pistol_bullet(Object& obj, UpdateContext& ctx);
void update_plasma_ball(Object& obj, UpdateContext& ctx);
void update_lightning(Object& obj, UpdateContext& ctx);
void update_red_mushroom_ball(Object& obj, UpdateContext& ctx);
void update_invisible_debris(Object& obj, UpdateContext& ctx);
void update_red_drop(Object& obj, UpdateContext& ctx);
void update_fireball(Object& obj, UpdateContext& ctx);
void update_moving_fireball(Object& obj, UpdateContext& ctx);
void update_explosion(Object& obj, UpdateContext& ctx);

// Port of explode_object_with_duration_A at &40db-&40ed. Mutates `obj`
// in-place into an OBJECT_EXPLOSION with the given duration (frames).
// Keeps the slot's tertiary_slot reference, so the tile-plot loop's
// dedup at tertiary_spawn.cpp:62-68 sees the explosion as occupying
// the slot and skips re-spawning the original primary until the
// duration counts down and the slot is reaped. Used by step 12 of
// update_objects (energy-zero explosion) and per-type updaters that
// destroy themselves in-place (active grenade etc.).
// `play_sound = true` mirrors &40db (which JSRs play_sound_on_channel_
// zero with bytes 17 03 11 04); `false` mirrors &40e2 explode_object_
// with_duration_A_but_no_sound, used by the bullet-pop / red-drop paths
// that already played a softer "ptack" pop on their own.
void explode_object_with_duration(Object& obj, uint8_t duration,
                                  bool play_sound = true);

// Port of accelerate_all_objects (&343a-&34b0) and
// accelerate_all_objects_within_angle (&343c) — applies damage and a
// directional push to every primary within blast range of `source`.
// Used by update_explosion for EXPLOSION primaries, the blaster
// discharge tick (&4a76-&4a83), and the engine-fire push (&4c5e).
//
// Skips slot `source_slot` so the source itself isn't damaged or pushed.
// When damage_events is non-null every hit and the source's effective
// tile radius are pushed for the "Damage" debug overlay.
//
// `centre_angle` / `angle_range` gate the affected targets to a cone
// (centre ± range, both 8-bit angle units where 0x40 = 90°). Default
// 0/0xff = no angle restriction (full radial).
//
// `force_damage = true` mirrors the engine-fire path which sets
// &28 acceleration_damages_targets unconditionally; otherwise damage is
// applied when `duration >= 8` (the explosion's &4fdc ROR gate).
void apply_explosion_radius(ObjectManager& mgr, const Landscape& landscape,
                            const Object& source,
                            int source_slot, uint8_t duration,
                            std::vector<DamageVisual>* damage_events = nullptr,
                            uint8_t centre_angle = 0,
                            uint8_t angle_range  = 0xff,
                            bool force_damage    = false);

} // namespace Behaviors
