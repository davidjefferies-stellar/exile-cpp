# Audio

How the 6502's SN76489 4-channel chip-tune driver is ported to host PCM.
All `&xxxx` addresses refer to `exile-standard-disassembly.txt`.

The whole implementation lives in `src/audio/audio.cpp` (with the
declarations in `src/audio/audio.h`). It is callable as a flat namespace
`Audio::` — the game does not own an audio object.

## What the original chip did

The BBC's SN76489AN is a 4-channel programmable sound generator:

| Channel | Generator | 6502 usage |
|---|---|---|
| 0 | 15-bit LFSR noise (white or periodic) | Explosions, hits, water — anything percussive |
| 1, 2, 3 | Square-wave tone | Pitched effects, NPC chirps, door slides, weapon fire |

Each channel has a 4-bit attenuation (`0x00`-`0x0f`) and a frequency
register. The chip plays continuously; the driver mutates the frequency
and attenuation via envelopes to make a sound.

In Exile, the driver at `&1320-&14b5` runs once per game tick (50 Hz)
and walks each channel through two parallel envelopes — one for volume,
one for frequency. A sound is just a pair of envelope IDs plus an
initial value and a stage count for each.

## API surface

```
Audio::open()                     // idempotent device open; ok to call repeatedly
Audio::close()
Audio::play(channel_hint, params)
Audio::play_at(channel_hint, params, src_x, src_y)
Audio::set_listener(x, y)         // updates the centre used by play_at
Audio::tick()                     // run one frame: advance envelopes, render samples
Audio::set_debug_tone(bool)       // 440 Hz square — proves the device is reaching the speakers
Audio::set_enabled(bool)          // master mute; device stays open
```

`channel_hint` is one of:

| Constant | 6502 entry | Behaviour |
|---|---|---|
| `CH_PRIORITY = 0` | `SEC` at `&13f8` | Always lands on channel 0 (also the noise channel). Used for loud / important effects. |
| `CH_ANY = 1` | `CLC` at `&13fa` | Picks the first non-busy channel from 1..3, or the quietest if all three are busy. Port of `&1421-&144f`. |

`params[4]` is the same 4-byte block that follows a 6502 `JSR play_sound`
call. Identical layout, byte for byte:

```
params[0] : volume-envelope id   (index into kEnvelopesTable)
params[1] : initial volume in top nibble | stage count in low nibble
params[2] : frequency-envelope id
params[3] : initial frequency in top nibble | stage count in low nibble
```

The high nibble of `params[1] / params[3]` is the raw 0x00..0xf0 starting
value (0x10 steps — that's all the chip's 4-bit register could express).
The low nibble is the total number of envelope stages the sub-envelope
will walk before stopping.

## Lifecycle of a single sound

1. Caller passes `params` to `play()` or `play_at()`.
2. `pick_channel(hint)` picks a slot (`&1421-&144f`). `CH_PRIORITY` -> 0.
   `CH_ANY` -> first free in 1..3, else quietest.
3. **Re-trigger gate** (port of `&1426-&144f`): if the chosen channel is
   still busy and the new sound's initial volume is *lower* than the
   currently playing value, the new call is dropped. This stops
   per-frame callers (engine fire, power-pod tick) from re-arming the
   same loud sound forever.
4. The 4 params are split across two `EnvState` records inside the
   chosen `Channel` (volume + frequency). `stage_duration` is set to 0
   so the very next `update_envelope` call enters the start-stage path.
5. For channel 0 the noise LFSR is reseeded to `0x4000` (a single 1 at
   the top stage); for tones the phase accumulator is left alone — the
   table-driven `phase_inc` will start advancing it on the next `tick`.
6. `play_at()` additionally computes Chebyshev distance from the
   listener (`&1415-&141a`); if `>= 16` tiles, it returns without
   playing. Otherwise it sets the channel's `volume_reduction =
   distance × 0x10`, which `tick()` subtracts from the envelope's live
   `value` every frame (`&1376-&137e`).

## Envelope engine

`Channel::vol` and `Channel::freq` each hold:

| Field | Role |
|---|---|
| `value` | Current 8-bit output (only top nibble drives the chip — bottom is sub-quanta interpolation in our renderer) |
| `duration` | Total stages remaining; 0 = sub-envelope ended |
| `stage_offset` | Index into `kEnvelopesTable`, pointing at the current stage's *delta* byte |
| `stage_duration` | Frames remaining in the current stage |
| `loops_remaining` | Mid-loop iteration counter; 0 outside a loop |
| `loop_offset` | Where the current loop body starts |

`update_envelope` (`&1399-&13e3`) runs once per tick per envelope:

1. If `duration == 0`, the envelope has already exhausted — return false
   so the per-channel renderer can apply its fade.
2. If `stage_duration == 0`, the current stage is over. Walk to the next
   stage: decrement `duration`, read the next duration byte, then either
   (a) interpret it as a stage duration, or (b) if bit 7 is set, treat
   it as a loop marker (bits 0-6 = repeat count, body starts on the
   next byte). Mid-loop iterations don't decrement `duration`.
3. Apply the stage's delta byte to `value` (`&13d4 ADC`). The 6502 has
   `CLC` at `&1399`, so carry-in is always 0 on this path.
4. Decrement `stage_duration`.

`kEnvelopesTable` is the byte-for-byte port of `&2db9-&2e88`. Adding new
sounds means appending to this table (or reusing one of the existing
entries) — the params just point at it by index.

### Tail-off

After a volume sub-envelope ends (`update_envelope` returns false), the
renderer fades `value` by 4 per frame (`&1328-&132f` is -2/frame but the
chip's 4-bit register doubles the perceived rate, so we lose 4/frame to
match). The channel reads as **busy** while `duration != 0` and as
**audible** while the top nibble of `value` is non-zero. Both states
matter — `pick_channel` skips busy channels, but `tick` keeps rendering
audible-but-finished channels through the tail-off.

## Tone synthesis (channels 1-3)

Each tone channel is a 32-bit phase accumulator. The high bit of the
accumulator is the square-wave output. Per sample:

```
ch.phase += ch.phase_inc;
wave = (ch.phase & 0x80000000) ? +1.0 : -1.0;
```

`phase_inc` is looked up from `kPhaseIncTable` (256 entries, built at
compile time) indexed by `ch.freq.value`. The mapping is a geometric
5-octave sweep:

```
freq_hz(byte) = 125 * 2^(byte * 5 / 255)
```

So `freq.value = 0x00` -> 125 Hz, `0xff` -> 4 kHz. The 6502 fed an
identical curve into the chip via its 10-bit divisor register; the curve
is encoded directly here because the chip's divisor maths don't map
cleanly to PCM phase accumulators.

`phase_inc` is reloaded whenever `update_envelope` advances `freq.value`
during `tick`.

## Noise synthesis (channel 0)

Channel 0 is the noise channel. It runs an SN76489-style 15-bit LFSR
clocked at one of four discrete rates plus a white/periodic toggle. The
6502's `&1345-&135f` translates `freq.value` into a 4-bit control word;
that translation is ported byte-for-byte by `noise_reg_for_byte`, then
specialised into two compile-time tables:

| Table | Indexed by | Yields |
|---|---|---|
| `kNoiseTable.step[256]` | `freq.value` | per-sample phase increment for the LFSR stepper |
| `kNoiseTable.white[256]` | `freq.value` | true -> white noise (tap XOR); false -> periodic (15-stage pulse) |

The LFSR stepper is a 32-bit accumulator: it wraps on overflow, and
each wrap clocks the LFSR once. The taps match the BBC's SN76489AN
exactly (bit 0 XOR bit 1 -> bit 14). On reseed (`play_at` / `play` for
ch0), the LFSR is set to `0x4000` so the first audible output is one
sample of "1" propagating through the 15 stages — that's the chip's
characteristic periodic-mode "buzz".

`update_envelope` advances `freq.value` per frame; that re-indexes the
noise table next sample. The 6502 wrote the new noise-control nibble to
the chip the same way.

## Mixing and output

`tick()` runs once per game frame and:

1. Advances both envelopes on every channel. Channels whose vol envelope
   has ended are still audible during the tail-off; channels that are
   fully silent are skipped.
2. Renders `kSamplesPerTick = 882` mono float samples (44100 Hz / 50
   fps). Each channel contributes `wave × (vol_reduced_top_nibble / 15)
   × 0.25` to the mix — the `0.25` headroom keeps the four-channel sum
   inside ±1.0. The debug-tone path (a 440 Hz square at half-scale)
   sums in *on top* of the channels, so you can leave it on while the
   game is making real sounds.
3. Pushes the float buffer to whichever backend is active. The PCM
   stream is continuous: `tick()` runs every frame regardless of
   whether anything is playing. Skipping a frame produces audible
   underrun clicks because the device's ring buffer drains
   continuously.

### `vol.value − volume_reduction`

The chip's actual attenuation register is 4 bits, so only the top nibble
of `vol.value` reaches the speakers. The 6502 subtracts
`sound_channels_volume_reduction[ch]` (`&11dc`) before pushing the
nibble out, flooring at zero (`&1376-&137e`). The port does the same in
the per-sample loop:

```
reduced = vol.value - volume_reduction;       // floored at 0
amp     = (reduced >> 4) / 15.0;              // 4-bit chip attenuation
mix    += wave * amp * 0.25;                  // 4-channel headroom
```

`play()` always zeros `volume_reduction`. `play_at()` re-sets it to
`distance × 0x10` immediately after `play()` returns. So:

- Sounds tied to the player (weapon fire, R/T, pocket retrieve) are
  always full-volume.
- World-located sounds (creature, door, transporter, hive) fade with
  Chebyshev distance to the listener, clipped to "no playback at all"
  beyond 16 tiles.

The listener position is set every frame by `Game::run` from the
player's tile coordinates.

## Platform backends

Selected at compile time:

```
EXILE_NO_AUDIO   -> no-op stubs; the device never opens
_WIN32           -> hand-rolled waveOut (winmm.lib) with two ping-pong int16 buffers
otherwise        -> fenster_audio.h's float PCM path (deps/fenster_audio.h)
```

The Windows path exists because `fenster_audio.h` sets `nBlockAlign = 1`
on its `WAVEFORMATEX`. Modern `waveOutOpen` rejects that for 16-bit
mono (which requires `nBlockAlign = 2`); the rejection silently leaves
the device handle null and every subsequent `waveOutWrite` becomes a
no-op. The ~30-line in-tree replacement uses the correct alignment.

The non-Windows path uses `fenster_audio.h` directly — the C
implementation is built in `src/audio/fenster_audio_impl.c` to keep the
implicit `int16_t* -> LPSTR` cast (legal C, hard error in C++) out of
the C++ TUs.

`FENSTER_AUDIO_BUFSZ` is hard-coded at 882, identical to
`kSamplesPerTick`. Any mismatch leaves a stale tail in the device
buffer that audibly clicks every frame; both values must change together
if the tick rate ever moves off 50 Hz.

## Configuration

`exile.ini` has one knob:

```
[audio]
enabled = true
```

When `false`, `play` / `play_at` become no-ops and `tick` pushes silence.
The device stays open so the toggle is cheap at runtime
(`Audio::set_enabled` from anywhere). When the master is toggled off,
in-flight envelopes are wiped to zero immediately — without that, a
sound currently fading out would keep ringing for up to a second after
the disable.

## Diagnostics

`exile-audio.log` is opened lazily on the first `audio_log` call. It
captures:

- The result of `Audio::open` (success / which backend / failure reason).
- The first `tick()` (a one-shot diagnostic confirming the loop is
  actually being driven and a buffer slot was found).
- Every `set_enabled` / `set_debug_tone` transition.

If the log is missing or empty, `Audio::open` never reached its first
log line — usually because audio is compile-disabled
(`EXILE_NO_AUDIO`).

`Audio::set_debug_tone(true)` is the fastest way to confirm the chain
end-to-end: it bypasses the channel/envelope code entirely and emits a
constant 440 Hz square wave. If the tone is audible, the device, the
write loop, and the game's tick cadence are all working; the problem is
somewhere in `play` / envelopes / `params`.

## Where to look when …

| Symptom | First file to read |
|---|---|
| Sound never starts | `pick_channel` and the re-trigger gate in `play()` |
| Sound starts but is too quiet | `Audio::play_at`'s `volume_reduction = distance × 0x10` |
| Sound starts but is wrong pitch | `kPhaseIncTable` (tones) or `kNoiseTable.step` (noise) |
| Sound starts but cuts off early | The stage count in `params[1] & 0x0f` / `params[3] & 0x0f` |
| Sound plays continuously | Caller is hitting the re-trigger gate every frame but with rising volume — guard the call site, not the audio code |
| Click every frame | Backend buffer size mismatch (Win32 `kSamplesPerTick`, fenster `FENSTER_AUDIO_BUFSZ`) |
| Crash on shutdown | Make sure `Audio::close()` runs (idempotent — safe to call from any path) |
