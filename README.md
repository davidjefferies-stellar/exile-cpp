# exile-cpp

A C++ port of the BBC Micro game **Exile** (Peter Irvin & Jeremy Smith, 1988),
working from the original 6502 disassembly. The goal is a faithful
reimplementation of the game's procedural world, physics, AI, and audio in
modern C++ — close enough that 6502 addresses cite straight across to the
ported source.

The disassembly used as the spec lives at `exile-standard-disassembly.txt`
in the project root.

## Build

Visual Studio solution. There's no CMake / headless build path right now.

```
msbuild exile.vcxproj /p:Configuration=Release /p:Platform=x64
```

The build pulls in two single-header dependencies bundled under `deps/`:

- `fenster.h` — windowing + framebuffer
- `fenster_audio.h` — audio (Windows backend is bypassed by our own
  `waveOut` implementation in `src/audio/`; the upstream sets `nBlockAlign`
  incorrectly for PCM 16-bit mono)

## Run

```
exile.exe
```

Configuration lives in `exile.ini` — start position, cache sizes, debug
flags, key bindings, and the landscape-generator A/B toggle.

## Controls

| Key            | Action                                    |
|----------------|-------------------------------------------|
| arrows         | move (with jetpack: thrust)               |
| `Z`            | toggle jetpack                            |
| Space          | fire selected weapon                      |
| Tab            | turn around                               |
| Left Ctrl / `L`| lie down                                  |
| Right Ctrl     | jetpack booster (held = 2× thrust)        |
| `,` `M` `.`    | pick up / drop / throw held object        |
| `S` / `G`      | store in pocket / retrieve from pocket    |
| `R` / `T`      | remember position / teleport to it        |
| `I` `K` `O`    | aim centre / down / up                    |
| `1`–`5`        | select weapon slot                        |
| `Y` / `U`      | whistle one / two                         |
| `P`            | pause                                     |
| `;` / `'`      | save / load                               |
| `Q`            | quit                                      |
| `B`            | toggle pixel-AABB overlay                 |
| `\`            | toggle map mode                           |
| mouse wheel    | zoom                                      |
| right-drag     | pan camera (map mode)                     |
| left-click     | tile info / toggle bottom-HUD checkbox    |

Bottom-HUD checkboxes drive the rest of the debug overlays (tile grid,
object labels, switch / transporter wires, collision shading, debug text).

## Source layout

```
src/
  core/         Types, fixed-point, RNG. No game-state dependencies.
  world/        Landscape generation, tertiary resolution, water, wind, tile data.
  objects/      Primary / secondary / tertiary storage, physics, collision.
  behaviours/   Per-type update routines (creature, robot, projectile,
                environment, collectable) + dispatch table + shared NPC helpers.
  particles/    Particle system (&207e update + per-type tables at &0206).
  rendering/    Renderer interface + fenster framebuffer backend, palette
                decode, sprite atlas, debug overlays.
  audio/        SN76489-style envelope synthesizer over fenster_audio.h.
  player/       Input, motion, action, sprite handling.
  game/         Top-level loop orchestration (game.cpp ≤ ~200 lines).
```

## Status

See `docs/PORTING_PROGRESS.md` for a system-by-system survey of what's
faithfully ported vs. partial vs. missing, with 6502 address ranges for
each entry. The disk-load supervisor, copy protection, and demo mode are
intentionally left out; everything else aims to match the disassembly.

`docs/TERTIARY.md`, `docs/OBJECT_SPAWNING.md`, and
`docs/ANGLES_FROM_VELOCITIES.md` document specific subsystems where the
6502's data layout / arithmetic semantics needed careful porting.
`docs/INTRO_TRIAX.md` traces the non-scripted Triax / destinator opening
beat through the per-frame update loop. `docs/COLLISION.md` covers the
tile and object collision system, including the 22.5°/45° slope
patterns.
