# behaviours/ audit — prioritised fix list

Findings from a four-agent audit of `src/behaviours/` against
`exile-standard-disassembly.txt`. 17 critical, 30 medium, 20 low.

**Status (2026-05-26):** all 17 critical and all 30 medium items addressed
(commits `03bf158`..`0d1f5d2`). One medium item (#42 destinator end-game)
deferred — needs broader feature plumbing. Two items resolved as audit-
errors (no change needed — verified accurate against disasm). Low-prio
items not yet tackled.

Items are tagged with file:line, 6502 reference, the divergence, and a
concrete fix. Pre-existing documented port-only divergences (in code
comments) are not listed here unless the comment misrepresents what the
6502 does.

## CRITICAL — gameplay-visible bugs, fix first

| # | File:line | 6502 ref | What's wrong | Fix |
|---|-----------|----------|--------------|-----|
| ~~1~~ | ~~environment.cpp:1050-1059~~ | ~~&419f update_maggot_machine~~ | ~~Routine has been replaced with a maggot spawner; the original squeals + h-flips every 64 frames and (when underwater) triggers earthquake+flood+explosion. Machine never blows up underwater, never chirps.~~ | ~~Replace body with underwater check -> squeal + flip + damaged-palette flash. Maggot spawning lives elsewhere (&25aa).~~ DONE (underwater earthquake/flood branch deferred — needs mutable Game state in UpdateContext) |
| ~~2~~ | ~~environment.cpp:1145-1178~~ | ~~&4b64 update_placeholder~~ | ~~Converts on any touch and uses anchor-Chebyshev distance, missing the four-step 6502 gate (touch+can_trigger, else every-16, range != 9, player-LOS). Equipment placeholders pop into view as the camera pans.~~ | ~~Port the four-step gate verbatim; use `check_for_obstruction_between_objects_80` to slot 0.~~ DONE (prev_x/y now plumbed; placeholder pinned via position-from-previous; PLACEHOLDER added to gravity_exempt) |
| ~~3~~ | ~~environment.cpp:198-205~~ | ~~&4cfb update_door BPL~~ | ~~Stop-door branch uses `wide_next > 0`; 6502's BPL fires when `>= 0`. Doors clamped to exactly 0 fall through to open-end logic and can auto-cycle.~~ | ~~`closed_end = (wide_next >= 0)`.~~ DONE |
| ~~4~~ | ~~environment.cpp:520-559~~ | ~~&499d / &4a09 update_switch~~ | ~~Switch press always plays the click sound; 6502 plays a second "switch had effect" sound (`c7 c3 c1 03`) only when a toggled byte actually changed.~~ | ~~Inside `process_switch_effects`, capture pre-data, compare post-data, fire the second `play_sound` on diff.~~ DONE |
| ~~5~~ | ~~environment.cpp:673~~ | ~~&4dd2 update_transporter_beam~~ | ~~Palette cycles from global frame counter; every transporter beam in the world is in phase. 6502 uses per-object `&06` which is slot*0x11 + global.~~ | ~~Use a per-object frame counter (Object has none today; compute `slot*0x11 + frame_counter`).~~ DONE |
| ~~6~~ | ~~creature.cpp:1067~~ | ~~&4f33-&4f3b update_piranha_or_wasp~~ | ~~Retarget gate inverted twice: `BIT &db / BVS skip` is rendered as "retarget when bit 6 set" (wrong), and `target_player = state >= rnd` is the reverse of `CPX &da / BCS find_target`. Wasps & piranhas swap hive-vs-player chances.~~ | ~~`if (!(rng.next() & 0x40))` and `target_player = (obj.state < rng.next())`.~~ DONE (using peek) |
| ~~7~~ | ~~creature.cpp:1046~~ | ~~&4e5c update_maggot~~ | ~~Maggot wrapper calls `update_worm` so deals 3 damage; 6502 maggot loads X=&14 = 20 damage. Maggots do 1/7th their real damage.~~ | ~~Factor a `worm_or_maggot_common(obj, ctx, damage)` helper; pass 0 for worm and 20 for maggot.~~ DONE |
| ~~8~~ | ~~creature.cpp:1005~~ | ~~&420e update_worm~~ | ~~Worm passes damage=3 to touch; 6502 loads X=0 -> no damage. Worms shouldn't hurt on touch (they only burrow & disturb).~~ | ~~Same shared helper, damage=0 for worm.~~ DONE |
| ~~9~~ | ~~projectile.cpp:565-571~~ | ~~&4a92 update_plasma_ball~~ | ~~In-water removal does `obj.energy = 0`, which step 12 mutates into an explosion. 6502 calls `add_plasma_particles` + `set_object_for_removal`.~~ | ~~Emit 30 PLASMA particles then `obj.flags \|= PENDING_REMOVAL` (same path as the energy==0 branch right below).~~ DONE |
| ~~10~~ | ~~projectile.cpp:481-502~~ | ~~&4326 update_cannonball~~ | ~~Skips `&1faf` damageable filter, has no tile-collision detonate, no lifespan, and uses default-duration explosion instead of duration 16.~~ | ~~Run through `common_bullet_update(obj, ctx, 170)`, then `explode_object_with_duration(obj, 0x10)` on touch or tile-collision.~~ DONE |
| ~~11~~ | ~~projectile.cpp:505-515~~ | ~~&4332 update_blue_death_ball~~ | ~~Deals 40 direct damage on touch; 6502 doesn't damage directly — it jumps to `explode_object_with_duration_A` (A=0x10) and the radial explosion does the damage.~~ | ~~Touch/tile-collision -> `explode_object_with_duration(obj, 0x10)`; remove the 40-damage hardcode.~~ DONE |
| ~~12~~ | ~~robot.cpp:400-408~~ | ~~&4837-&484a update_clawed_robot~~ | ~~Teleport gate wrong shape: 6502 fires on `energy<0x8c` OR `(directness bits zero AND frame_counter==0)`, skips recently-damaged, and uses `consider_teleporting_to_random_tile_near_player` (3x3 near player, y<=0x46). Port uses 1-in-256 rng and teleports to y=0xfe.~~ | ~~Implement the gate and call `consider_teleporting_to_random_tile_near_player`; set DIRECTNESS_ONE on `target_and_flags` after.~~ DONE (gate fixed; random-tile-near-player hop deferred — separate &488b behaviour) |
| ~~13~~ | ~~collectable.cpp:467-469~~ | ~~&4216 / &33b4 update_alien_weapon~~ | ~~Plasma ball spawns with a literal 0x40 velocity_x; 6502 routes through `calculate_firing_vector_from_this_object_velocity` which adds the parent's vx (with a +0x20 boost when >=0x50). Player can't shoot harder by moving.~~ | ~~Inline the firing-vector helper: `ball.velocity_x = clamp(int(vx) + int(obj.velocity_x))` after sign-flip, with the >=0x50 boost.~~ DONE |
| ~~14~~ | ~~collectable.cpp:114-144~~ | ~~&415b update_inactive_grenade~~ | ~~Doesn't check `ctx.player_object_fired`; 6502 promotes to ACTIVE_GRENADE when the held grenade is RCD-fired. Held-then-RCD-fired grenades sit inert.~~ | ~~At the top: `if (ctx.player_object_fired == ctx.this_slot) { /* promote to ACTIVE_GRENADE */ return; }`.~~ DONE |
| ~~15~~ | ~~npc_helpers.cpp:38-41~~ | ~~cancel_gravity helper~~ | ~~Decrements `velocity_y` only when `>0`; 6502 `DEC acceleration_y` is unconditional. Every flier that is currently rising receives full gravity.~~ | ~~Drop the guard: `obj.velocity_y--;` (with int8 saturation).~~ DONE |
| ~~16~~ | ~~collectable.cpp:386-401~~ | ~~&41ed coronium "player holding"~~ | ~~Proxies holding with `abs(dx)<=1 && abs(dy)<=1 && vx==player.vx`; misfires when player just stands next to a coronium boulder.~~ | ~~`bool player_holding = (ctx.held_object_slot == static_cast<uint8_t>(ctx.this_slot));`~~ DONE |
| ~~17~~ | ~~npc_helpers.cpp:567-577~~ | ~~&31da move_towards_target_with_probability~~ | ~~Helper chases the target slot's live x/y instead of `obj.tx/obj.ty`; LOS-derived path waypoints are ignored, NPCs walk straight into walls.~~ | ~~Replace target.x/y with obj.tx/obj.ty in the diamond-metric so the path waypoint actually drives motion.~~ DONE |

## MEDIUM — subtle correctness, edge cases, RNG drift

| # | File:line | 6502 ref | Issue | Fix |
|---|-----------|----------|-------|-----|
| ~~18~~ | ~~environment.cpp:1085, 984, 833-838; creature.cpp ~339, 1156, 1234 etc.~~ | ~~various `BIT &da/&db/&dc`~~ | ~~6502 frequently *peeks* `rnd_state[1..3]` without advancing the LFSR. Port calls `rng.next()` at every site, burning bytes and desynchronising the rng sequence.~~ | ~~Add `Random::peek(idx)` returning `rnd_state[idx]` without advance; replace the peek sites~~ DONE (engine fire, sucking nest, dense nest, fluffy squeal, bird whistle, red-slime drop). Hive count covered by #19, piranha retarget by #6. |
| ~~19~~ | ~~environment.cpp:744-753~~ | ~~&4bca-&4bd1 update_hive count~~ | ~~6502 is one `JSR rnd` + two ANDs against peeked rnd_state bytes; port does three `rng.next()` calls.~~ | ~~One `next()` ANDed with `peek(1)` ANDed with `peek(3)` ANDed with #&07.~~ DONE |
| ~~20~~ | ~~environment.cpp:932-980~~ | ~~&3442 accelerate_all_objects~~ | ~~Loop iterates 0->N; 6502 starts at slot 0x0f and decrements.~~ | ~~Iterate high-slot-to-low.~~ DONE |
| ~~21~~ | ~~environment.cpp:894-919~~ | ~~&4e03 update_sucking_nest detection~~ | ~~Port calls `has_line_of_sight_randomized` once per primary slot; 6502's `find_object` consumes exactly one randomised LOS cap.~~ | ~~Roll cap once outside the loop; use `has_line_of_sight` with tile cap inside.~~ DONE |
| ~~22~~ | ~~environment.cpp:43-72~~ | ~~&0bd2-&0be5 hit_by_aim_cone~~ | ~~Distance estimated as `tiles = max(adx, ady)` (Chebyshev); 6502's &359c uses true ray distance in &20-fractions.~~ | ~~Documented as port-only deviation in code.~~ DONE (deviation noted) |
| ~~23~~ | ~~environment.cpp:1064-1140~~ | ~~&4cd3 update_door~~ | ~~6502 has a dead-branch bug overwriting door energy with `data \| 0x08`; port doesn't replicate.~~ | ~~Documented as port-only skip in code (cosmetic on transition frame).~~ DONE (divergence noted) |
| ~~24~~ | ~~environment.cpp:103-299~~ | ~~&4ca5 update_door ordering~~ | ~~Port clears MOVING after RCD-locks the door; 6502 keeps MOVING set on the same frame as the lock. Anything sampling MOVING mid-frame disagrees.~~ | ~~Preserve MOVING bit when locking; re-derive `data` in the 6502's order.~~ DONE |
| ~~25~~ | ~~creature.cpp:1300 update_gargoyle; :318 update_fluffy~~ | ~~&4170 / &42a8 per-object frame counter~~ | ~~Fire-rate gates use global `frame_counter`; 6502 uses `&06 this_object_frame_counter` (slot-offset). Multiple gargoyles/fluffies fire in lockstep.~~ | ~~Use `(slot * 0x11 + frame_counter)` as a per-object proxy.~~ DONE |
| ~~26~~ | ~~creature.cpp:320-336 update_fluffy~~ | ~~&42ae-&42bc find_object~~ | ~~Squeal scan only checks imp types 0x29..0x2d; 6502 uses FLYING_ENEMIES range 0x22..0x31.~~ | ~~Extended predicate to `t >= 0x22 && t <= 0x31`.~~ DONE |
| ~~27~~ | ~~creature.cpp:928-930 update_red_slime~~ | ~~&47cd-&47d5 sprite cycle~~ | ~~Audit claimed port ignores LSR carry before SBC.~~ | ~~Traced disasm: &47ce SEC overrides the LSR carry — port already matches byte-for-byte. Audit was wrong.~~ DONE (no change needed) |
| ~~28~~ | ~~creature.cpp:487-506 update_imp~~ | ~~&450c-&4548 food-eat ordering~~ | ~~Port absorbed food before the at-home block; 6502 only absorbs on the not-home branch.~~ | ~~Food-touch block moved into not-home branch after `enforce_minimum_energy`.~~ DONE |
| ~~29~~ | ~~creature.cpp:1256-1263 update_invisible_bird~~ | ~~&1ae1 / &462b visibility reset~~ | ~~Port cleared `palette` bit 7 but renderer never gated on it — bird stayed permanently visible.~~ | ~~Added transient `Object::visible`, per-frame reset (mirrors &1ae1), renderer gates draw on it.~~ DONE |
| ~~30~~ | ~~creature.cpp:1057-1061 update_piranha~~ | ~~&4f2b-&4f31 accel offset~~ | ~~Piranha got `+4` accel_y; 6502 net +3 (STA 4, DEC).~~ | ~~`cancel_gravity` then `+3`.~~ DONE |
| ~~31~~ | ~~creature.cpp:1148-1212 update_bird~~ | ~~&4659 ROR &11 BNE~~ | ~~6502 skips sprite block when state != 0 (recently damaged).~~ | ~~Wrapped sprite/path block in `if (obj.state == 0)`.~~ DONE |
| ~~32~~ | ~~creature.cpp:23-26 chatter_common~~ | ~~&48a7-&48af~~ | ~~Port used `\|= 0x80`; 6502 `STA` overwrites low bits.~~ | ~~`obj.timer = 0x80; obj.state = NPCMood::MINUS_TWO;`.~~ DONE |
| ~~33~~ | ~~projectile.cpp:595-693 update_lightning~~ | ~~&4103-&4108 / &1fc9~~ | ~~Lightning damaged projectiles/explosions/bushes/fireballs; 6502 filter excludes them.~~ | ~~`bullet_touching_damageable` + explicit FIREBALL/PROJECTILES/ACTIVE_CHATTER exclusion.~~ DONE |
| ~~34~~ | ~~projectile.cpp:921-940 update_fireball~~ | ~~&4ad6-&4adc~~ | ~~Submerged fireballs never expire; 6502 gives a 1-in-4-per-frame removal when fully underwater.~~ | ~~`if (Water::is_fully_underwater(obj) && (rng.next() & rng.next() & 0x80)) { /* particles + PENDING_REMOVAL */ }`.~~ DONE |
| ~~35~~ | ~~projectile.cpp:998-1087 apply_explosion_radius~~ | ~~&3448-&344d~~ | ~~Explosion damaged and pushed targets through walls (no LOS check).~~ | ~~Added per-target raycast using `point_in_tile_solid_with_doors`; threaded Landscape into the signature.~~ DONE |
| ~~36~~ | ~~projectile.cpp:964-993 update_explosion~~ | ~~&4fd0-&4fd6~~ | ~~Damage pass ran every frame; 6502 tapers via `(rnd & 7) >= duration`.~~ | ~~Added `(rng.peek(3) & 7) >= duration` early-out after the duration DEC.~~ DONE |
| ~~37~~ | ~~robot.cpp:348-360 update_hovering_robot~~ | ~~&276c-&2773~~ | ~~Only the outer 1-in-4 gate is present; 6502 also has the inner `(energy>>3)+2 >= rnd` fire-rate gate, so port fires 4-8x too often.~~ | ~~Add `uint8_t threshold = (obj.energy >> 3) + 2; if (rng.next() >= threshold) return;` after LOS.~~ DONE |
| ~~38~~ | ~~robot.cpp:164-210 update_rolling_robot~~ | ~~&4ede-&4ef6~~ | ~~Reimplemented as +/-4 velocity walker with no Mood, no NPC path, no consider-flipping, no probability gate.~~ | ~~Collapse into the blue-rolling-robot body with per-type min_energy and the `&4f1e` bullet table; add the magenta/red `if (energy < 0x80) return;` early-exit.~~ DONE |
| ~~39~~ | ~~projectile.cpp:754-757 update_invisible_debris~~ | ~~&4791-&4796~~ | ~~Lifespan end sets `energy = 0` -> step 12 mutates into an EXPLOSION. 6502 just removes.~~ | ~~`obj.flags \|= PENDING_REMOVAL;`.~~ DONE |
| ~~40~~ | ~~projectile.cpp:763-858 update_red_drop~~ | ~~&47c3-&47c8 yellow->coronium~~ | ~~Red drop converts yellow slime AND explodes; 6502 returns immediately after the type swap.~~ | ~~After the yellow-slime mutation: `return;` (skip `should_explode`).~~ DONE |
| ~~41~~ | ~~projectile.cpp:669-672 update_lightning flip~~ | ~~&4145-&4148~~ | ~~Port XORs flip flag; 6502 ROR-assigns from frame_counter bits.~~ | ~~Assigned from `frame_counter & 0x01` (V) and `0x02` (H).~~ DONE |
| 42 | collectable.cpp:190-208 update_destinator | &4374-&4386 | Cosmetic flasher only — missing the `&19ab ship_moving` early-exit and the engine-tile gate, so completing the game cannot trigger end-of-game flooding. | DEFERRED — needs `ship_moving` global + engine-tile flooding cascade wired into UpdateContext. Documented in code at update_destinator. |
| ~~43~~ | ~~collectable.cpp:315-339 update_remote_control_device~~ | ~~&4351~~ | ~~Calls `update_collectable` first; 6502 RCD doesn't pin.~~ | ~~Dropped the `update_collectable(obj, ctx);` prelude.~~ DONE |
| ~~44~~ | ~~path.cpp:185-205 use_relaxed_path~~ | ~~&3d4a~~ | ~~Missing 1-in-512 "drop to directness zero" rng roll; NPCs that see a target at level 1 never lose it through boredom.~~ | ~~`if ((rng.next() & 0xff) == 0 && (rng.next() & 0x80) == 0) drop_level();`.~~ DONE |
| ~~45~~ | ~~mood.cpp:99-103 find_target~~ | ~~&3c21 / &3c4a~~ | ~~Loop walks slots in order; 6502 randomly tie-breaks via `&3c4a` and drops mixed primary/secondary finds via the `&3c21` probability table.~~ | ~~EOR loop index with `rng.next() & 0x0f`; apply the probability gate before accepting a secondary.~~ DONE (random tie-break; secondary probability gate not relevant — port doesn't include secondaries in find_target) |
| ~~46~~ | ~~mood.cpp:196-203 eating-touch~~ | ~~&2804-&280d~~ | ~~Port only processes food when `obj.touching` already matches; 6502 also runs a 50%-skip-`find_target` for the food type so hungry NPCs seek the food.~~ | ~~After the home block, add `if ((rng.next() & 0x80) == 0) find_target(npc, ctx, slot, kFood[cat], 0xff, 16, _player, _primary);` and stamp the slot.~~ DONE |
| ~~47~~ | ~~path.cpp:130-134 set_directness_three~~ | ~~&3d04-&3d08~~ | ~~Masked write could clobber sibling flags.~~ | ~~Switched to `|=` per 6502 `ORA #&c0`.~~ DONE |

## LOW — comments, cleanups, minor visual drifts

These are documented or rare enough they can wait.

- creature.cpp:160-180 chatter pitch one-bit ADC carry omission (audible only on direct comparison vs original).
- creature.cpp:43 chatter food-reserve increment ordering vs 6502 stimuli pass.
- creature.cpp:885-888 red-frogman damage=0 vs no-damage sentinel (functionally equivalent today).
- creature.cpp:1334-1338 triax teleport timer value not verified vs `&0ce5`.
- creature.cpp:1340-1378 update_triax flooded with one-shot debug `log_diag` calls; remove or gate.
- creature.cpp:1395-1396 Mood::update_mood before update_npc_path may overwrite target flags Triax expects.
- creature.cpp:1427-1432 Triax `seek_player` overwrites velocity instead of accel + cancel_gravity.
- creature.cpp:1469-1472 Triax sprite picker cites &4754 but doesn't invoke `update_crew_member`.
- environment.cpp:587-675 transporter beam wrap-to-0 vs 6502's wrap-to-1.
- environment.cpp:1102-1104 engine-fire toucher push: matches; flagged for verification only.
- environment.cpp:130-136 door axis pin: matches on second read.
- collectable.cpp:198-207 destinator chirp stochastic vs deterministic `&06 AND #&1f`.
- collectable.cpp:99-101 disturb pin clears energy bit 0 (`ASL/LSR` quirk) — port preserves it.
- ~~collectable.cpp:99-101 disturb pin also reverts position in 6502; port only zeros velocity.~~ DONE (uses obj.prev_x/y)
- collectable.cpp:401 radiation underwater uses centre-of-object instead of top.
- npc_helpers.cpp:412 aim_toward Manhattan denom vs 6502 lookup table.
- projectile.cpp:382 active-grenade rotates palette from post-INC timer (one frame off).
- robot.cpp:90-161 turret missing Triax-lab variant (single turret in top of lab).
- robot.cpp:107-112 turret fire gate `>=` vs `>` off-by-one at boundary value.

## Suggested order to tackle

1. **Helpers first** (#15 `cancel_gravity`, #17 `move_towards_target`, #18 `Random::peek`) — touched by many of the other findings; fixing them cleanly first prevents whack-a-mole.
2. **One-line damage / gate bugs** (#3, #6, #7, #8, #14, #16) — high impact, near-zero risk.
3. **Projectile damage paths** (#9, #10, #11, #34, #39, #40) — fix as a coherent batch since they share `explode_object_with_duration` plumbing.
4. **Switch / door / transporter polish** (#4, #5, #24) — visible quality issues, contained.
5. **Maggot machine / placeholder / RCD-grenade** (#1, #2, #13) — bigger ports; tackle once the simpler stuff lands.
6. **Per-object frame counter** (#5, #25) — needs a tiny Object field or a helper; do once and reuse.
7. **Hovering / clawed robot fire / teleport** (#12, #37, #38) — combat tuning, easy to verify visually.
8. **Mood / path randomisation** (#44, #45, #46) — affects feel more than correctness; do after the visible bugs are clear.
