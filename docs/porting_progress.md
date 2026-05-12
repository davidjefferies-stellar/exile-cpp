# Exile C++ Port — Porting Progress

Survey of the 6502 disassembly at `exile-standard-disassembly.txt` versus
the current C++ port. Legend:

- **Done** — faithfully ported with 6502 address cited
- **Partial** — basic functionality present, but simplified / approximate /
  missing sub-behaviour. Usually indicated in-source by "Approximation",
  "simplified", or missing address citation.
- **Missing** — not in the port at all
- **Port-only** — new code added by the port (not 6502-derived)

Addresses throughout are taken from the disassembly's `;` labels.

---

## Boot / binary / disk

| System | Status | 6502 range | Notes |
|---|---|---|---|
| Disk load / supervisor | **Missing** | (separate file) | Not applicable — C++ port loads directly. |
| Binary relocation + decrypt | **Missing** | &793e-&798e, &14ca-&14e4 | 6502 moves `&1200-&78ec` to `&0100-&67ec`, decrypts in place. Port runs unencrypted. |
| Wipe screen / CRTC init | **Missing** | &01d0-&01ee, &14c5 | Framebuffer-based renderer; no CRTC. |
| `main_game_loop` skeleton | Done | &19b6-&19cf | `Game::run` in `src/game/game.cpp`. |
| Frame-counter timer flags (every 2/4/8/16/32/64) | Done | &19b6-&19c7 | `Game::update_timers`. |
| IRQ1 / vsync handler | **Missing** | &01ff-&0306 | Framebuffer presents on frame tick; no raster interrupts. Palette updates and sound channel service run from vsync in 6502. |
| Raster palette swap (water colour-0) | **Port-only** | — | `render_water_column` approximates the below-waterline background. |

## Copy protection / demo mode / save-load

| System | Status | 6502 range | Notes |
|---|---|---|---|
| Copy-protection prompt | **Missing** | &6235-&67ec, &3929-&39ff | Word-from-novella entry, screen formatting, 3-try lock-out. |
| Demo-mode hang | **Missing** | &2711, &394e | `check_copy_protection` path; 1-in-256 hang every 32 frames if wrong word. |
| `prepare_game_for_save` | Port-equivalent | &2b07-&2b3f | `Game::save_game` writes a **human-readable text save** (`exile.sav`) with player / event state, all active primaries + secondaries, the 235-byte tertiary-data dump and the RNG seed. `;` key triggers save, `'` triggers load. Landscape is regenerated from seed so isn't part of the save. Doesn't reproduce the 6502's on-disc layout or encryption. |
| Game-state encrypt / decrypt | **Missing (intentional)** | &7968-&79a5, &7a2e-&7aab | XOR-chain encryption of `&07f8-&5bf7` for save/load. Port uses plain text for debuggability. |
| Checksums (`game_state_checksum_one/two`) | **Missing** | &0b6b, &0b74 | Tamper-detection checksums for encrypted save. N/A for text save. |
| `handle_preparing_game_for_save` | Port-equivalent | &2aef | `;` key edge-triggered in `Game::process_input`. |

## Input / action routines

| System | Status | 6502 range | Notes |
|---|---|---|---|
| Action-key dispatch (`process_actions`) | Partial | &01a8-&01cf | `InputHandler::process_key` handles the gameplay keys but does not use the 6502's "routine pointer per key" indirection; rising-edge detection done in-game (`pickup_key_prev_` etc.) rather than at `&126b`. |
| Key scan matrix / `check_for_keys` | **Missing** | &67c9-&67ec | BBC keyboard matrix polling; `fenster` provides character-level keys. |
| Auto-repeat suppression | Partial | &01ac-&01c7 bit handling | Per-action rising-edge done for pickup/drop/throw/store/retrieve/pause/map. |
| SHIFT behaviours (`handle_pressing_shift`, sound toggle) | **Missing** | &14d6-&14e4 | |
| Pause | Done (port-only behaviour) | &359c `handle_pause` | 'P' freezes world updates while keeping input / render live; not 6502-faithful (original halts on COPY). |

## Rendering pipeline

| System | Status | 6502 range | Notes |
|---|---|---|---|
| Sprite plotter (bitmap → screen) | Partial | &0d4d-&10cf | `FensterRenderer::blit_sprite` draws from an RGBA atlas rather than decoding the 128×81 four-colour sheet; BBC palette decoding via `palette.cpp`. Bit-7 foreground mask (hides objects behind foliage) is reproduced via `fg_mask`. |
| Sprite plot modes (unplot, crop, offscreen flags) | **Missing** | &0e8f-&0ebe, &10d6-&1113 | Port redraws whole framebuffer each frame. |
| `plot_tile_strip` / tile caching | **Missing** | &0fd2-&1005 | No BBC-style strip caching; port loops tiles every frame. |
| Double-buffer / CRTC start address | **Missing** | &1f58-&1f88 | Not needed with a framebuffer. |
| Screen scrolling (physical, `&19a8 suppress_physical`, `&1ae7 handle_scrolling`) | **Missing** | &1ae7-&1b53, &2c19-&2c98 | Camera is free-form in the port; no pixel-column wipe code. |
| `redraw_screen` / viewport wipe | **Missing** | &1ba3-&1bfb | |
| Palette decoding (logical → RGB, colour-3, colour-1&2 pair table) | Done | &0b78-&0b8e, &11e5 | `palette.cpp`. |
| Palette cycling (`rotate_colour_from_A`) | Done (where used) | &4dd4 | `rotate_colour_from_A` in projectile.cpp. Per-level palette register updates (&148a) missing. |
| Background flash (`flash_background`, `update_background_flash`) | **Missing** | &1f8f-&1fa5 | Used on: no-free-slot, coronium explosion. |
| Tile rendering & palette resolution | Done | &1715-&198e resolve, &2200-&229c palette | `landscape.cpp`, `tertiary.cpp`, `render.cpp`. |
| Water column / waterline raster | Done (port-only) | &12a6-&12d8 approximated | `render_water_column`. |
| HUD / energy bar / weapon select | Partial | (no direct 6502 equivalent — BBC used text area) | `render_hud` draws energy, pockets, weapon slot. Score / deaths / game time not shown. |
| Debug overlays (grid, AABB, tier swatches, tile-click info) | **Port-only** | — | Keys G / T / B and left-click. |
| Zoom / right-drag pan | **Port-only** | — | Mouse wheel + drag; box-filter zoom-out. |

## Audio

| System | Status | 6502 range | Notes |
|---|---|---|---|
| Sound channel bookkeeping, envelopes | Done | &1320-&149c | `src/audio/audio.cpp` — 4-channel software synth, envelope table at `kEnvelopesTable` ported byte-for-byte from `&2db9`, channel-0 routed through an SN76489-style LFSR noise generator (white / periodic / rate-pair mapping from the 6502's `&119c`/`&11a0` tables). |
| `play_sound` / `play_sound_on_channel_zero` | Done | &13f8-&149c | `Audio::play` / `Audio::play_at`. Channel arbitration (slot 0 priority, first-inactive of 1..3 else quietest) matches `&1421-&144f`. |
| Distance attenuation (`get_object_distance_from_screen_centre`, volume reduction) | Done | &1415-&1465 | `Audio::play_at` — Chebyshev distance to listener, >=16-tile cut-off, `distance * 0x10` per-channel volume reduction applied each render frame. |
| `play_middle_beep` / `play_high_beep` / `play_low_beep` / `play_squeal` | Done | &149d-&14b8 | Inline `kSound*` blocks at each call site (R-key remember, RCD fire, red-drop pop, plasma fire). |
| Damage scream (when WAS_DAMAGED set ≥ 8) | **Missing** | &2492-&24a1 | Not centrally hooked; would need a hub through `damage_object` which the port currently inlines at each call site. |
| Imp / bird / piranha / wasp / fluffy / chatter / worm / hover / clawed / nest / engine-fire / hive sounds | Done | &45f0, &4609, &463b, &4811, &485b, &42c1, &42d7, &492b, &4eac, &4c61, &4be3, &4e30 | Per-NPC ambient sounds, all `play_at` so they attenuate with distance. |
| Door open / close / lock, switch, teleport, collect, retrieve, blaster, explosion, mushroom poof, bullet pop, grenade tick | Done | &4d2d, &4d36, &31d3, &49b9, &4410, &4b96, &351d, &4a80, &40de, &3ffc, &4428, &4321 | All wired at the corresponding ported routine. |
| Waterfall / earthquake sound loop | Partial | &2619 | Engine-fire sound is wired but the waterfall/earthquake gate (`every_eight_frames` + `flooding_state`) isn't — engine-fire only plays from `update_engine_fire`. |
| Whistle one / two | Done | &2cb7, &2ca1 | Player keys Y / U, plus the white-yellow-bird whistle-two trigger at `&4625`. |
| Self-modifying envelope pitch (imp variant pitch, chatter pitch) | Partial | &4609, &4925 | The 6502 stores a computed byte into `envelopes_table+offset` before calling play_sound; the port uses the default-table block, so all imps sound identical. |
| Energy-level bell tick | **Missing** | &4a61-&4a64 | Player-energy-step chime not yet hooked into the player update path. |
| `sound_enabled` toggle / runtime config | Done | &17fb (analogue) | `Audio::set_enabled` plus `exile.ini` `[audio] enabled = true/false`. Master enable silences ringing envelopes immediately; device stays open. |

## World generation & tiles

| System | Status | 6502 range | Notes |
|---|---|---|---|
| Procedural landscape (`get_tile`, f1/f2 funcs, passages, slopes, caverns) | Done | &1715-&19a6 | `src/world/landscape.cpp` with exact `Alu` helper. |
| Map-data overlay (spacecraft, set-piece puzzles) | Done | &0bec-&0e0f | `map_overlay.h`. |
| Tile palette resolution (stone / bush / earth / leaf / mushroom / spacecraft branches) | Done | &2200-&229c | In `render.cpp` / `tertiary.cpp`. |
| Tile update routines (for collisions: cannon, engine, doors, transporter, etc. — the tile half) | Partial | &3e4a-&3fcc | `spawn_tertiary_object` covers the spawn path; tile-side bookkeeping (e.g. `update_nest_or_pipe_tile` burrow-on-collision) isn't systematically ported. |
| Obstruction patterns | Done | &0100-&01a7 | `obstruction.cpp/h`, `tile_data.h`. |
| `update_variable_wind_tile` / `update_constant_wind_tile` | Partial | &3f1c-&3f61 | Wind field is global-only (`wind.cpp` applies a centred-at-(&9B,&4E) field) rather than per-tile parametric. |
| `update_variable_water_tile` / `update_water_tile` | **Missing** | &3f7a-&3fae | Tile type `WATER` uses global waterline only; in-tile still-water vs moving-water animation not ported. |
| `update_mushroom_tile` | **Missing** | &400b-&402c | Mushroom contact → timer / spawn mushroom ball. Object-side (`update_red_mushroom_ball`) is ported. |
| Tile flip / rotation tables, feature tiles | Done | &112f-&1195 | In `landscape.cpp`. |

## Object storage & lifecycle

| System | Status | 6502 range | Notes |
|---|---|---|---|
| Primary / secondary / tertiary lists | Done | &0828-&0b51 | `ObjectManager` with 16 primary, 32 secondary, 220 tertiary slots. |
| `promote_secondary_object_to_primary_if_Y_slots_free` | Done | &1dbe-&1e18 | `promote_selective`, `promote_distance_check`. |
| `demote_primary_object_to_secondary` | Done | &1e35-&1e61 | `demote_to_secondary` with a port-only anti-duplicate guard. |
| `check_demotion_or_removal` (KEEP_AS_TERTIARY / PRIMARY_FOR_LONGER distance gate) | Done | &1bb7-&1d26 | `check_demotion`. Gate distance tweaked (port-only) to match the wider viewport. |
| `create_new_object_if_Y_slots_free`, `find_most_distant_object` | Done | &1e62-&1ecf | `ObjectManager::create_object`. |
| `return_spawn_to_tertiary_object` / `demote_to_tertiary_object` | Done | &1d21, &4081 | `return_to_tertiary`. |
| `handle_teleporting` | Done | &1bfd-&1c44 | Mid-point position swap, timer counter, `player_is_completely_dematerialised_` flag (suppresses star-field spawn while teleporting), and the &1c35 position-change chime are all ported. |
| `consider_demoting_or_returning_object` full chain | Done | &1d24-&1d5b | In `check_demotion` + `return_to_tertiary`. |

## Physics / collision

| System | Status | 6502 range | Notes |
|---|---|---|---|
| `apply_acceleration_to_velocities` (gravity bit, skip-limit, 16-frame inertia) | Done | &1f01-&1f3c | `Physics::apply_acceleration`. |
| `calculate_seven_eighths` (ground friction) | Done | &3235 | In `integrate_player_motion`. |
| `apply_buoyancy`, `apply_water_effects`, splash particles | Partial | &2f01-&2f82 | Simplified weight ladder in `water.cpp`; faithful splash emission in the main loop. |
| `apply_surface_wind` | Done | &1c47-&1c92 | `Wind::apply_surface_wind` with exact weight / strength math. |
| Wind particle emission | Done | &3f73 | In `object_update.cpp`. |
| `check_for_collisions` (object-object AABB + `touching` field) | Done | &2a64-&2bed | `Collision::check_object_collision`. |
| `apply_collision_to_objects_velocities` (mass-ratio transfer) | Done | &2bee-&2c18 | `Collision::apply_mass_ratio_velocity`. Wired for player only (`integrate_player_motion`); NPC-vs-NPC collisions still just halt. |
| `check_for_tile_collisions_on_top_and_bottom_edges` | Partial | &2e8c-&2f00 | Player motion does section-sweep; other objects use an any-tile-solid revert rather than the 32-fraction section loop. |
| `check_for_obstruction_between_objects` (line-of-sight raycast) | Done | &359c-&3683 | `NPC::has_line_of_sight` in `behaviours/path.cpp`. Tile-by-tile raycast, 1/8-tile steps, per-section obstruction via `Collision::point_in_tile_solid`. Skips the door-at-target suppression (`door_to_suppress` at &3599) and the waterline-crosses-as-obstruction flag — both still simplified in the few callers that need them. |
| `handle_swapping_direction` | **Missing** | &1d63 | |
| `consider_npc_burrowing`, `reduce_velocities_for_burrowing` | **Missing** | &29a4-&29c9 | Worm / maggot burrowing through earth. Worm behaviour falls back to simple seek. |
| Held-object positioning (`update_position`, `should_drop`) | Done | &1afd-&1c43 | `held_object.cpp`. |

## NPC AI — shared helpers

| System | Status | 6502 range | Notes |
|---|---|---|---|
| `check_for_npc_stimuli`, phobia / target / food / home tables | Partial | &27c9-&2866, &316b-&31ab | `mood.cpp` reimplements with a simplified response table; per-category stimulus lookup lossy compared to original 10-entry tables. |
| `alter_npc_behaviour` (time / flood / damage / explode / eating / phobia) | Partial | &2841 | Only "damage", "player nearby", "time" and "random" stimuli implemented. |
| `consider_updating_npc_path` + directness levels | Partial | &3c75-&3d25 | `NPC::update_npc_path` in `behaviours/path.cpp`. Dispatches on DIRECTNESS_TWO/ONE/ZERO to select direct / slightly-relaxed / relaxed target (tx, ty). `find_route_to_target` (the 4-angle route-attempt loop at &3d5b) still approximated — relaxed path just jitters toward target. |
| `consider_if_npc_can_see_target` | Done | &3cf6-&3d25 | `NPC::update_target_directness` — 16-frame cadence, updates DIRECTNESS bits from LOS. |
| `find_route_to_target` (route-attempt loop) | **Missing** | &3d5b-&3db5 | Multi-angle fallback when direct path is blocked. |
| `npc_walking_types_*` tables, `update_walking_npc` | **Missing** | &39a5-&39b8, &3a3b-&3ab8 | `check_for_space_below_object`, `check_if_npc_can_walk` not ported. Creatures walk via ad-hoc velocity sets. |
| `consider_setting_npc_jumping`, `set_npc_jumping_with_speed` | **Missing** | &3a65-&3a7a | Frogman jumping open-coded. |
| `update_walking_state` (climbing, state bookkeeping) | **Missing** | &3a4e-&3a63 | |
| `find_object`, `find_nearest_object`, `count_objects_of_type_A` | Partial | &3c21-&3c74 | `count_objects_of_type_A` inlined where needed; nearest-object probability table at `&3c33` not used. |
| `consider_absorbing_object_touched` | Partial | &3bf4 | `NPC::consider_absorbing_object_touched` shared by bird and big fish (PENDING_REMOVAL + low beep, no &3bd5 angle gate). Hive / nest variants still inline their own version. |
| `move_towards_target` + `apply_weighted_acceleration_to_this_object_velocity` | Partial | &3192-&31fc | `NPC::move_towards_target_with_probability` reduces the vector math to axis-nudges. |
| `calculate_angle_from_vector` | Done | &22d4-&22fb | In `player_sprite.cpp` and `projectile.cpp`. |
| `calculate_firing_vector_from_angle` / diamond trig | Done | &2357-&239c | `diamond_vector` in `weapon.cpp`, `NPC::vector_from_magnitude_and_angle` in `npc_helpers.cpp`. |
| `calculate_vector_from_magnitude_and_angle` | Done | &2357 | `NPC::vector_from_magnitude_and_angle`, magnitude-parameterised diamond. |
| `calculate_angle_of_object_X_to_this_object` | Done | &22a0-&22fd | `NPC::angle_from_deltas` (core of `&22d4`) + centre math folded into `compute_firing_vector`. |
| `calculate_firing_vector_from_distance` (full firing chain) | Done | &3355-&33a4 | `NPC::compute_firing_vector` — centre-to-centre angle + magnitude, diamond aim, gravity compensation proportional to flight time, target-velocity leading, speed cap. `NPC::fire_at_target` wraps it with the `&278a` random firing-velocity pick. Currently wired into the turret; other NPCs (clawed robot, hovering robot, blue rolling robot, Triax) still use the simpler aim path. |

## NPC AI — per-type updates

The 6502 has ~50 `update_<object>` routines; the port dispatches all
object types via `behavior_dispatch.cpp`. Quality varies per routine:

| Group | Status | 6502 range | Notes |
|---|---|---|---|
| Player (`update_player` &4A11) | Mostly Done (split across files) | &4a11-&4a83 | Input, motion, sprite split into 3 TUs. "Energy-level bells" (&4a74), discharge-blaster, fire suppression when pinned not ported. |
| Crew member / Fluffy / Chatter | Partial | &46f0, &4288, &48c1-&48d7, &48ef | Mood wiring rough; Chatter whistle / power-pod-gift path approximates but works. |
| Imps (5 variants, feeding, gifts) | Partial | &44ef-&460f | Per-variant min-energy / projectile / gift-type tables ported; at-pipe gift-drop with per-imp `imp_gifts_remaining` counter ported (`Game::imp_gifts_remaining_[5]`). Walking / climbing state and per-imp pitch self-modification (&4609) still simplified. |
| Frogmen (red / green / invisible) | Partial | &4463-&4475 | Jump cadence / damage present; NPC walking, water-facing tile collision (`set_npc_facing_tile_collision` &2562) **missing**. Invisible-frogman visibility flag **missing**. |
| Slimes (red / green / yellow + conversions) | Partial | &422a-&47d8 | Touch damage works; mushroom-immunity, coronium-crystal absorption → yellow slime conversion **missing**. |
| Nests (dense, sucking) | Partial | &4789, &4ded | Sucking pulls via linear scan; `sucking_nests_trigger/power/palette_direction` tables at `&4e89` **unused** (power ladders simplified). |
| Big fish | Done | &4761 | Eats touched piranha (PENDING_REMOVAL + low beep), every-16f nearest-piranha target acquisition, NPC path / move-towards-target with 2× speed when DIRECTNESS_TWO bit set. Underwater gate uses `Water::is_underwater` instead of the 6502's per-object waterline (intentional port divergence). |
| Worms / maggots (update, animate, emerge logic) | Partial | &420a, &4e52-&4ebe | Simple seek, no burrowing. Emerge-from-earth event gating **missing**. |
| Piranha / Wasp | Done | &4f21-&4f9b | Faithful port including sign convention and aggressiveness state. Home-hive fallback targeting TODO. |
| Birds (green / white / red-magenta / invisible) | Done | &4621-&4672 | Per-type damage/energy tables, whistle-two gate, invisibility-on-damage all ported. |
| Gargoyle | Partial | &4170-&419e | Fires fireball periodically; per-type projectile / velocity / frequency tables at `&419b` **missing**. |
| Triax | Partial | &4704-&475f | Intro destinator steal done; teleport-away / random-firing approximated, no proper "see or has seen player" state machine. |
| Clawed robots (4 variants) | Partial | &481f-&4891 | Per-variant min-energy table done; teleport energy management partly in `game.cpp` events. Availability flag semantics match 6502. |
| Turrets / rolling / blue rolling / hovering robots | Partial | &4ed8-&4ee2, &4804-&481e | Simplified firing; `rolling_robots_minimum_energy_table` and `_bullet_table` (&4ed0) used but line-of-sight check skipped. |
| Cannon / Maggot machine / Alien weapon | Partial | &40ee, &419f, &4216 | Fire intervals approximate; maggot machine doesn't respect tertiary `tertiary_objects_data + &02` counter (the Triax-lab hookup). |
| Grenade (inactive + active) | Partial | &4158, &42f7 | Fuse + palette cycle done; "thrown grenade reverts to inactive if player drops" (&42f7 `check_if_object_fired`) **missing**. |
| Bullets (icer / tracer / pistol / red / blue-death / cannonball / plasma / lightning) | Mostly Done | &441b, &4614, &46bf, &434a, &4332, &4326, &4a88, &4101 | Energy/damage/trail emission faithful; homing logic simplified for tracer / death-ball. Lightning fully ported. |
| Mushroom ball (red / blue) | Done | &4698-&46b9 | Player mushroom timer + fireball conversion + particle burst. |
| Fireball / moving / temporary / permanent fireball | Partial | &4ad6-&4b63 | Fireball damage + palette + particles done; "temporary fireball" duration logic exists only for `EXPLOSION`. |
| Hovering ball / invisible hovering ball | Partial | &43e7-&43eb | Wander + touch damage; sound, remote-control hit **missing**. |
| Placeholder object | Done (port-friendly) | &4b64-&4b87 | Chebyshev visibility instead of raycast (annotated). |
| Doors (4 colour pairs × 2 orientations) | Done | &4c83-&4d71 | Full state machine with energy ladder, speed halving, V-flag endpoint handling, per-colour auto-close, palette lock-pip mask. Remote-control toggle **missing**. |
| Switches (visible + invisible) | Done | &499d-&49d9, &3ef2 | Leading-edge detection via rolling `tx`; `process_switch_effects` + `switch_effects_table` ported byte-for-byte. |
| Transporter beam | Partial | &4d86-&4ddc | Destination table, sweep, palette cycle ported; remote-control lock toggle (`consider_toggling_lock` &319c) and stationary vs sweeping initial state from data byte **missing**. |
| Hive (small + large, spawn-from-data-byte) | Done | &4baf-&4c14 | Spawn type in data byte, aggressiveness palette tint. Home-hive target-of-spawn math approximate; no `find_object`-based "don't spawn if BIG_FISH present" check. |
| Sucking nest | Partial | &4ded-&4eba | Basic suck + absorb; per-type power / palette direction tables at `&4e79` **missing**. |
| Engine fire | Done | &4c15-&4c82 | Timer, random flip, palette, particle emission, radial knock-back. |
| Power pod / destinator / remote & cannon control | Partial | &4360-&438f | Power pod lifespan + flash done; destinator end-of-game trigger at `update_destinator` is a palette flash only, no ship-ready check. |
| Full / empty flask | Partial | &43a7-&43e6 | Fill-on-submerge + spill-on-impact works; per-frame water particle stream, fireball extinguish distance, flask-as-water-source for animals (&43d0 big fish drink) missing. |
| Giant block / Bush / Piano (inert) | Partial | &439c-&43ad | Bush regenerates energy; crash-damage collision (bush shatter) **missing**. |
| Explosion | Done | &4f9c-&4ff1 | Timer, particle emission, radial push + damage. Tertiary-object damage via `check_for_tertiary_objects_around_explosion` (&4fcb) **missing** (e.g. nests, hives shouldn't auto-destroy, but doors / pipes should). |
| Coronium boulder / crystal | Partial | &41c2-&4209 | Chain explosion + radiation damage done; radiation-immunity-pill check is a TODO stub. |
| Collectables (keys, weapons, immunity pills, whistles, suits) | Partial | &4b88-&4ba8 | Disturbed-pin works; auto-collect on held (`player_collected[type]` decrement) deliberately skipped — see comment, conflicts with S/R pocket model. |

## Events / world state

| System | Status | 6502 range | Notes |
|---|---|---|---|
| `update_events` top-level | Partial | &259a-&2742 | Star-field spawn, Triax-lab door + maggot machine, waterline tick, Triax summoning, clawed-robot respawn ticked; earthquake progression flag ticked but no screen-shudder. |
| `update_triax_lab` | Done | &25a1-&25df | `Game::update_triax_lab` — every-frame maggot count refill, every-64-frame maggot spawn at the transporter tile, every-32-frame door drive from Water::get_y(1), desired_y[1] = 0xe2 (drain) / 0xd2 (fill) / 0x67 (flooding). |
| Waterline updates (`update_waterlines_loop` &2626-&265b) | Done | &2626-&265b | `Water::update_waterlines` — 4 ranges, step (y, y_fraction) toward desired_y by ±2 per frame with the 32-of-64-frames cyclic ripple. State + getters/setters live in `src/world/water.cpp`; save/load persists all three arrays. |
| `consider_emerging_worm_or_maggot` | **Missing** | &2660-&26c5 | Random tile pick + earth-tile gate + biome-preferred type; spawn_object_in_event chain. |
| Earthquake shudder (CRTC R2 writes) | **Missing** | &2604-&260a | State variable `earthquake_state_` ticks, but no visible effect. |
| Flooding end-game trigger | **Missing** | &081e / &259a early path | `flooding_state_` exists but nothing in the port sets it. |
| Waterfall / earthquake sound loop | **Missing** | &2619 | |
| `automatically_teleport_player`, `consider_teleporting_damaged_player` | **Missing** | &4034-&403f | Low-energy player auto-teleport to safe zone. |
| Fallback-teleport correctness check | **Missing** | &7a33 | `infinite_loop_if_fallback_teleport_not_as_expected`. |

## Player state & HUD

| System | Status | 6502 range | Notes |
|---|---|---|---|
| Player energy, flash-on-damage | Partial | &2492-&24c5, &35f1 | Energy decrements on touch; `gain_energy_and_flash_if_damaged` flash effect **missing**. |
| `damage_object` (protection suit multiplier, scream) | Partial | &2482-&24c5 | Flat damage in helpers; suit multiplier (`multiply_damage_loop`) **missing**. |
| `update_player_angle_facing_and_sprite` | Done | &3795-&3906 | `player_sprite.cpp`. |
| `update_player_aiming_angle`, aim particle | Partial | &30fc-&3143 | Simple step-per-frame model; original accel → velocity → angle chain **not** ported. |
| Pocket store / retrieve (`handle_storing_object`, `handle_retrieving_object`) | Done | &34b4-&3517 | `player_actions.cpp`. |
| `handle_throwing_object` | Done | &35ac-&35d0 | |
| Whistle one / two | Partial | &36f4-&3713 | Per-frame activation flag works; Chatter deactivation by whistle-two (bird-sourced) approximated. |
| Weapon select / fire / energy transfer | Partial | &36ff-&37ae | Fire works; `handle_changing_weapon_or_transferring_energy` in-game UI (weapon-to-weapon drain) **missing**. |
| `check_reliability` (weapon jam chance) | **Missing** | &37f2 | 1-in-8 jam for poor weapons at low energy. |
| `player_has_functioning_jetpack` gate | Partial | &3593 | Jetpack always functional; damaged-jetpack state **missing**. |
| Player death / restart cycle | **Missing** | &0820 (`player_deaths`) increment | No death → respawn sequence. |
| Game-time counter | **Missing** | &081f `game_time` / &19c8 | Not ticked. |
| Score / deaths display | **Missing** | MODE-7 text area | No text-HUD area in port. |

## Particles

| System | Status | 6502 range | Notes |
|---|---|---|---|
| Particle types table, TTL / speed / colour / flags | Done | &0206-&0276 | `particle_system.cpp::TYPES`. |
| `update_particles` (accel, water invert, colour cycle, remove on TTL) | Done | &207e-&210a | |
| `add_particles` / position / velocity randomization | Done | &218c-&2281 | Includes sprite-flip-aware base position and optional centre. |
| `remove_particle` / compaction | Done (simplified) | &20fc | Swap-with-last compaction; original list uses dedicated opcodes. |
| Jetpack / star / fireball / explosion / plasma / wind / water / engine / aim / flask / projectile-trail emitters | Done | various | All emit via `ParticleSystem::emit`. |
| `plot_or_unplot_particle` with bitmap | Partial | &2061-&207d | Particle rendering uses renderer's `render_particle`, no individual colour-3 pair swap; "plot on foreground" flag ignored. |

## Core helpers

| System | Status | 6502 range | Notes |
|---|---|---|---|
| `rnd` LFSR | Done | &2587-&2599 | `Random::next`. |
| `calculate_angle_from_vector`, octant tables | Done | &22d4-&22fb | |
| `keep_within_range`, `invert_if_negative/positive`, `divide_by_*` | Partial / inline | &3231-&3278 | Several inlined; `calculate_seven_eighths` ported in player. |
| `get_object_weight`, `get_object_type_flags` | Done | &1d5c-&1d62 | `Object::weight()`. |
| `object_type_ranges_table` / `_energy_table` | Done | &2802-&2810 | Used in `mood.cpp`. |
| `change_object_type`, `change_object_sprite_to_A` | Done | &3287-&329f | `npc_helpers.cpp`. |
| `flash_if_damaged` helper | **Missing** | &3540 | |
| `explode_object_by_turning_into_fireball` + fireball duration variants | Partial | &403f-&408d | Explosion creation works; direct `explode_object_with_loud_squeal` / `explode_object_with_duration_A_but_no_sound` entry points **not exposed**. |

## Port-only / non-6502 additions

| Feature | Purpose |
|---|---|
| `src/game/config.cpp` + `exile.ini` | Designer override of spawn state (player x/y/energy, weapon, pockets, weapon energies), audio enable, landscape generator A/B. |
| `\` key — save current landscape to `exile.map` | Auto-loaded next launch (`Game::init` tries `load_from_file` before falling back to `bake()`). Lets a hand-edited map persist across runs. |
| `;` / `'` keys — save / load game state to `exile.sav` | Text-format snapshot (frame, RNG, player, events, 16 primaries, 32 secondaries, 235 tertiary bytes). Landscape isn't in the file — comes from the seed or `exile.map`. |
| Pause toggle (`P` key) | Freeze updates without halting input/render. |
| Map-mode HUD banner (tiers, switch presses, spawn counters) | Debug / inspection overlay, toggled via the bottom-HUD "Map mode" checkbox. |
| Tile-click overlay (left-click) | Prints tile type, flips, obstruction, tertiary data, anchor distance. |
| Tile grid / object-tier swatches / AABB overlay (`G`, `T`, `B`) | Visual debugging. |
| Zoom + right-drag pan | Scrollable / zoomable viewport beyond the 6502's fixed 8-tile window. |
| Viewport-width-aware spawn gate (`spawn_tertiary_object` 4-tile limit) | Compensates for our wider viewport so off-screen tertiary objects aren't thrash-spawned. |
| Duplicate-secondary guard in `demote_to_secondary` | Prevents pool pollution from wider-viewport re-spawning loops. |
| Waterline raster clamp using signed int | Map-mode pan can put camera above world; 6502's unsigned SBC assumed camera is playable. |
| Box-filter zoom-out | `fenster_renderer` resamples framebuffer for fractional zoom. |
| Headless test suite (`tests/`, `exile_tests.vcxproj`) | Boot smoke, walking-gate, fixed-point, RNG, and weapons-firing tests, run against `NullRenderer`. GitHub Actions builds and runs the suite on every push to master via `.github/workflows/tests.yml`. |

---

## Highest-impact gaps

Ranked roughly by player-visible impact / porting cost:

1. **Full NPC walking / pathfinding / line-of-sight** — simplified
   `seek_player` means creatures pour through walls, don't climb slopes,
   can't use `directness` levels, and can't see/avoid targets.
2. **Flooding end-game trigger** — `flooding_state_` is wired into the
   waterline integrator (sets desired_y[1] to 0x67 when bit 7 is set),
   but nothing in gameplay yet flips that bit; the flood never starts.
3. **Copy-protection / demo-mode** — no startup prompt, no demo-mode
   hang. Save / load are present (text format) but don't reproduce the
   6502's encrypted on-disc layout.
4. **Screen scrolling + sprite unplot/crop + tile caching** — port brute-
   redraws every frame; fine for modern hosts, but means the 6502's
   `calculate_amount_of_scrolling_needed_in_direction` chain and its
   half-tile alignment logic aren't exercised.
5. **Mood / NPC stimuli tables** — only 4 of the 8 stimulus bits
   implemented; mood transitions are coarse; no "explode" / "eating" /
   "flood" stimuli.
6. **Explosion → tertiary damage** (`check_for_tertiary_objects_around_
   explosion`) — doors / pipes can't be demolished by grenades yet.
7. **Player-state polish** — no deaths counter, no game-time, no
   flash-on-damage, no protection-suit damage multiplier, no automatic
   teleport when near-dead. Damage-scream chord and energy-level bells
   also still missing on the audio side.
8. **NPC sound polish** — per-imp pitch self-modification at `&4609`
   and the chatter `&4925` pitch byte are not yet ported; all imps
   sound identical.
