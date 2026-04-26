#pragma once
#include <cstdint>

// Audio module — synthesises BBC Micro-style sounds (4-channel, square
// waves with envelopes) and pushes the mix through fenster_audio.h.
//
// The 6502 calls play_sound at &13fa with a 4-byte parameter block
// describing volume + frequency envelopes; we mirror that interface so
// each play_sound call site in the disassembly maps to one Audio::play
// call here.
//
// Lifecycle:
//   Audio::open();    // Game::init
//   ...
//   Audio::play(channel_hint, params);   // game logic, anywhere
//   Audio::tick();                       // once per game frame (50 Hz)
//   ...
//   Audio::close();   // Game shutdown
//
// All entry points are no-ops if open() failed (no audio hw, denied,
// etc.) so the rest of the game keeps running silently.
namespace Audio {

// Channel hint matches the 6502's two play_sound entry points:
//   PRIORITY : SEC entry at &13f8 — always lands on channel 0, used for
//              loud / important effects (fire, explosions).
//   ANY      : CLC entry at &13fa — picks the first inactive channel
//              from 1..3, falling back to whichever is currently
//              quietest if all three are busy.
constexpr int CH_PRIORITY = 0;
constexpr int CH_ANY      = 1;

// Open the audio device. Returns true on success; false means we'll
// run silent for the rest of the session. Idempotent.
bool open();
void close();

// Queue a sound. `params` is the 4-byte block that follows the 6502's
// JSR play_sound:
//   params[0] : volume envelope id (index into envelope table)
//   params[1] : top nibble = initial volume 0..15
//               low nibble = duration in game ticks
//   params[2] : frequency envelope id
//   params[3] : top nibble = initial frequency 0..15
//               low nibble = frequency-envelope duration
// Use this overload for sounds tied to the player (weapon fire, R/T,
// pocket retrieve) — distance attenuation is treated as zero.
void play(int channel_hint, const uint8_t params[4]);

// Queue a world-located sound. Computes Chebyshev distance from
// (src_x, src_y) to the listener position set via set_listener();
// objects ≥16 tiles away don't play, and closer-than-16 sounds get a
// per-channel volume reduction of `distance × 0x10` subtracted from
// every envelope frame (port of get_object_distance_from_screen_
// centre + the SBC at &1379). Use for creature, door, robot, hive,
// etc. sounds that should fade with distance.
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

// Debug: when on, tick() emits a continuous 440 Hz square wave at
// moderate volume regardless of channel state. Useful for confirming
// that the audio device is open, tick() is being called, and samples
// are reaching the speakers — independent of play() and the channel
// selection logic. Off by default.
void set_debug_tone(bool on);

}  // namespace Audio
