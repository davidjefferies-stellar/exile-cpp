#pragma once
#include <cstdint>

// BBC Micro-style 4-channel synthesis. Mirrors play_sound at &13fa so
// each call site in the disassembly maps to one Audio::play here.
// All entry points are no-ops if open() failed.
namespace Audio {

// PRIORITY = 6502 SEC entry &13f8 (always ch 0, loud effects).
// ANY = CLC entry &13fa (first inactive of 1..3, else quietest).
constexpr int CH_PRIORITY = 0;
constexpr int CH_ANY      = 1;

// Open the audio device. Returns true on success; false means we'll
// run silent for the rest of the session. Idempotent.
bool open();
void close();

// Tick rate in Hz (25/50/75/100). Samples-per-tick = 44100/hz; called
// after open() to align audio cadence with [debug] target_fps. Default
// is the slowest supported rate (25 Hz) if never called.
void set_logic_rate(int hz);

// 6502 JSR play_sound 4-byte block: [0]=vol env id, [1]=hi:vol/lo:dur,
// [2]=freq env id, [3]=hi:freq/lo:dur. Player-attached sounds (zero
// distance attenuation).
void play(int channel_hint, const uint8_t params[4]);

// World-located sound. Chebyshev distance to listener; >=16 tiles
// = no play, else volume reduced by `distance * 0x10` per envelope
// frame. Port of &1379 SBC.
void play_at(int channel_hint, const uint8_t params[4],
             uint8_t src_x, uint8_t src_y);

// Tell the audio module where the listener is, for play_at's distance
// attenuation. Called once per game tick from Game::run with the
// player's tile position.
void set_listener(uint8_t x, uint8_t y);

// Per-frame tick: advance envelopes, render one frame's worth of
// samples (882 at 44100 Hz / 50 fps), push to the device. Call once
// per game tick. Safe before open() (silently does nothing).
void tick();

// Debug 440 Hz tone — confirms device open + tick + speakers,
// independent of play/channel logic. Off by default.
void set_debug_tone(bool on);

// Master enable. False silences ringing envelopes + makes play no-op;
// device stays open. Settable from exile.ini [audio] enabled, default true.
void set_enabled(bool on);
bool is_enabled();

// Enable the exile-audio.log file. Must be called before open() — gated
// via [logs] enabled. Default off: the log file is never created.
void set_log_enabled(bool on);

}  // namespace Audio
