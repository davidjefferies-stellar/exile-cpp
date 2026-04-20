# exile-cpp — conventions for Claude

This repo is a faithful port of the BBC Micro game **Exile** (6502) to C++.
The 6502 disassembly at `../exile-standard-disassembly.txt` (one directory up
from the project root) is the authoritative specification. Match it closely.

## 6502 porting rules

- **Cite 6502 addresses** in ported code. Function comments should name the
  routine and its address, e.g. `// &4F21 update_piranha_or_wasp`. When a
  particular line's logic is subtle, cite the exact source address in the
  body (`// &1d24 ORA #&80 — set spawn flag`). This is the #1 way I verify
  correctness during review.
- **Preserve the 6502's arithmetic semantics.** World coordinates wrap at
  256 — keep using `uint8_t` for positions, `int8_t` for deltas. Only
  promote to signed `int` when a comment explains why the wrap would be
  wrong (e.g. camera panning past the map edge).
- **Faithfulness over cleanup.** Don't "improve" on the original's quirks
  without asking. Weird tables, off-by-one tricks, and shared zero-page
  variables are usually intentional — reproduce them.
- **No speculative feature flags, no backwards-compat shims.** This is a
  fresh port; we can just change the code.
- When unsure which of several 6502 branches applies, read the disassembly
  before guessing. Grep the address range and trace the flow.

## File organisation

```
src/
  core/         Types, fixed-point, random — no dependencies on game state.
  world/        Landscape generation, tertiary resolution, water, tile data.
  objects/      Primary / secondary / tertiary object storage, physics,
                collision. No AI, no rendering.
  ai/
    behaviors/  Per-type update routines (creature.cpp, robot.cpp,
                projectile.cpp, environment.cpp). One .cpp per behaviour
                family; dispatch lives in behavior_dispatch.cpp.
    npc_helpers.{h,cpp}  Shared helpers used across behaviours
                         (seek_player, cancel_gravity, …).
  game/         Top-level loop orchestration. game.cpp ≤ ~200 lines —
                if it grows, split a concern into a sibling TU
                (e.g. tertiary_spawn.cpp, player_motion.cpp, render.cpp).
  particles/    Particle system (&207e update + per-type tables).
  rendering/    Renderer abstraction + fenster (framebuffer) backend.
                palette.cpp handles BBC MODE 2 logical colour decoding.
```

Put new ported behaviour in the directory that matches the 6502 region
(`ai/behaviors/*` for update routines, `world/*` for tile / landscape
code, etc.). Don't stuff everything into `game/`.

## C++ style

- **Comments explain WHY, not WHAT.** Well-named code documents itself. Use
  comments for invariants, non-obvious 6502 references, workarounds, and
  the reason something was chosen over an alternative. One short line
  unless an invariant genuinely needs more.
- Don't reformat or rename unrelated code in an edit. Minimal diffs.
- Prefer editing existing files over creating new ones.
- Don't create `README.md` / documentation files unless I ask.
- No emojis.

## Diagnostics and the build

- **Ignore clangd noise in the agent feedback.** Messages like
  `'foo/bar.h' file not found`, `Unknown type name 'uint8_t'`,
  `Use of undeclared identifier 'Game'` are pre-existing include-path
  issues in my editor's clangd config, not real compiler errors. They
  reliably appear after every edit. Don't chase them — only real build
  output from `msbuild` is ground truth.
- The build is a Visual Studio solution at `exile-cpp.sln` /
  `exile.vcxproj`. Build with msbuild (not CMake). There's no headless
  tool-runnable build; tell me when you need a build result.
- Never modify files under `deps/`.

## Key bindings

Authoritative list lives in `src/player/input.cpp`. Current map:

```
arrow keys   move (with jetpack engaged: thrust)
Z            toggle jetpack
X            fire selected weapon
,            pick up touching object
M            drop held object straight down
.            throw held object (drop + horizontal kick)
Enter        legacy pickup/drop toggle
S            store held in pocket
R            retrieve top of pocket stack
I / K / O    aim centre / down / up
1..5         select weapon slot
Y / U        play whistle one / two
\            toggle map-activation mode (camera centre vs player)
P            pause world updates (input + render still run)
Q            quit
```

Renderer-level toggles handled in `fenster_renderer.cpp`:

```
G            tile grid + activation rings + tile-tier overlay
T            object-tier swatches
B            pixel AABBs (collision boxes)
mouse wheel  zoom
right-drag   pan camera (map mode)
left-click   select tile, populate top-right tile-info overlay
```

If you change a binding, update this section. If you're tempted to
mention an action without naming the key, look it up here first.

## Debugging workflow

- The map-mode HUD (`\` key) shows per-frame diagnostics when
  investigating behaviour: `try` / `made` spawn counts, lifecycle
  counters (`ret rem dem`), primary and secondary lists with object
  names. When a mechanism looks broken, add to the banner rather than
  spamming stdout — the banner freezes when paused (`P`), making it
  readable frame-by-frame.
- When a bug is hard to reproduce, add a tile-click diagnostic in
  `render.cpp` that prints the tertiary state for the clicked tile —
  that's how we caught the invisible-switch-with-door-graphic case.
- Do not poll for compile errors after every edit. Let me run the build
  and report.

## Known mechanisms to be careful with

- **Camera clamp** (map mode). Camera centre is clamped to
  `[vp_half, 255-vp_half]` so panning can't drift past the map. Don't
  remove this.
- **Spawn distance gate** in `spawn_tertiary_object`: tiles further
  than 12 tiles from the activation anchor don't spawn. This compensates
  for our wider-than-BBC viewport; the 6502 didn't need it.
- **Tile foreground mask** in `FensterRenderer`: tile pixels drawn with
  BBC logical colour 8-15 mark the `fg_mask` buffer; object blits skip
  those pixels. This is the 6502's `EOR / BMI skip_byte` at &1066 and
  is how objects hide behind foliage.
- **Invisible-switch-with-door-graphic redirect**: a tile with landscape
  type `TILE_INVISIBLE_SWITCH` (0x00) can have a tertiary `tile_and_flip`
  that resolves to a door graphic. In that case the data byte is the
  switch-effects number, NOT the door's "needs creating" bit 7 — see the
  `switch_redirect` branch in `tertiary_spawn.cpp`.
- **Waterline raster** (`render_water_column`): uses signed int, not
  `uint8_t`, for the `waterline − vp_top` subtraction. The 6502's
  unsigned SBC only works because its camera can't pan outside the
  playable area; our map mode can, and an underflow flips every
  comparison.
