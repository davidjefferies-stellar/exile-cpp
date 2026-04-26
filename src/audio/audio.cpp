#include "audio/audio.h"

// fenster_audio.h is a single-header PCM playback library by the same
// author as fenster.h (Serge Zaitsev — github.com/zserge/fenster).
// Drop the file alongside fenster.h in deps/.
//
// fenster_audio.h was written for C, and its Windows backend assigns
// `int16_t*` to `LPSTR` (`char*`) — implicit in C, a hard error in
// C++. We get around it by compiling the implementation in a separate
// C TU (fenster_audio_impl.c) and including only the declarations
// here, wrapped in `extern "C"` so the symbols match.
//
// FENSTER_AUDIO_BUFSZ must match the per-tick sample count: the Windows
// backend's waveOutWrite plays the entire fixed-size buffer regardless
// of how many samples our write filled, so a mismatch leaves a stale
// tail that audibly clicks. Setting it to 882 = 44100/50 means each
// game tick exactly fills one buffer. Define on both sides (this file
// and fenster_audio_impl.c) — they need to agree.
//
// On Windows, fenster_audio uses the legacy waveOut* API which lives in
// winmm.lib. Auto-link it via the MSVC #pragma so the project's linker
// config stays clean (matches how fenster.h relies on default-set libs).
#ifndef EXILE_NO_AUDIO
  #if defined(_WIN32)
    // We bypass fenster_audio.h on Windows. Its waveOut backend sets
    // WAVEFORMATEX::nBlockAlign = 1, but for PCM 16-bit mono Windows
    // requires nBlockAlign == nChannels * wBitsPerSample/8 == 2.
    // Modern waveOutOpen rejects the wrong-aligned format and returns
    // an error — which the upstream code discards, leaving the device
    // handle NULL and every subsequent waveOutWrite a silent no-op.
    // Easier to write the ~30 lines of waveOut* glue ourselves than
    // patch deps/. On macOS / Linux we still use fenster_audio.h.
    #if defined(_MSC_VER)
      #pragma comment(lib, "winmm.lib")
    #endif
    #include <windows.h>
    #include <mmsystem.h>
  #else
    #define FENSTER_AUDIO_BUFSZ 882
    #define FENSTER_HEADER        // declarations only; impl lives in the .c TU
    extern "C" {
      #include "fenster_audio.h"
    }
    #undef FENSTER_HEADER
  #endif
#endif

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace {

// fenster_audio.h uses float PCM at 44100 Hz mono. At 50 fps that
// gives us 882 samples per game tick. (If you tweak the game tick rate
// or the audio device's rate, update both — they need to stay in sync
// or the device will glitch.)
constexpr int kSampleRate     = 44100;
constexpr int kSamplesPerTick = 882;
constexpr int kNumChannels    = 4;

// Per-channel state. Mirrors the 6502's sound_channels_* arrays at
// &11ac onwards. Each channel runs two parallel envelopes (volume +
// frequency), each with its own duration / stage / loop bookkeeping.
//
// One sub-envelope's lifecycle:
//   value           - current 8-bit output (0..0xff). Initial value
//                     comes from params[1] / params[3] high nibble.
//   duration        - total stages remaining; 0 = sub-envelope ended.
//                     Decremented when a top-level (non-loop) stage
//                     starts.
//   stage_offset    - index into kEnvelopesTable; points at the DELTA
//                     byte of the current stage (i.e. duration byte
//                     was at stage_offset-1, already consumed).
//   stage_duration  - frames remaining in current stage. When 0,
//                     update_envelope advances to the next stage.
//   loops_remaining - mid-loop iteration counter. 0 means "not in a
//                     loop" or "next encounter starts a fresh loop".
//   loop_offset     - byte index inside kEnvelopesTable where the
//                     current loop body begins (the first stage's
//                     duration byte).
struct EnvState {
    uint8_t value;
    uint8_t duration;
    uint8_t stage_offset;
    uint8_t stage_duration;
    uint8_t loops_remaining;
    uint8_t loop_offset;
};

struct Channel {
    uint32_t phase;        // square-wave accumulator (bit 31 = output sign)
    uint32_t phase_inc;    // per-sample phase increment, derived from freq.value
    EnvState vol;
    EnvState freq;
    // 6502 sound_channels_volume_reduction at &11dc. Subtracted from
    // the live volume value every render frame (&1379 SBC); makes
    // distant world sounds quieter. Set by play_at; play() leaves it
    // at zero so player-anchored sounds stay full-volume.
    uint8_t  volume_reduction;
};
// 6502 &1426: a channel is "busy" (can't be reused for a new sound)
// while its volume envelope is still running. When vol_duration hits 0
// the channel is free even though its volume may still be fading
// quietly to silence in the background.
static bool channel_busy(const Channel& ch) {
    return ch.vol.duration != 0;
}

// Channel produces audible output when the SN76489's 4-bit volume
// nibble is non-zero. The chip uses only the top nibble of the
// envelope byte for actual attenuation; values below 0x10 are
// silent. Truncating here means the fade reaches inaudibility
// faster than a linear amplitude check would suggest, which matches
// the 6502's perceived sound duration.
static bool channel_audible(const Channel& ch) {
    return (ch.vol.value & 0xf0) != 0;
}

Channel g_channels[kNumChannels];
bool g_debug_tone = false;
uint32_t g_debug_phase = 0;
// Listener (player) position; play_at compares against this to compute
// the volume reduction. Updated each tick by Audio::set_listener.
uint8_t g_listener_x = 0;
uint8_t g_listener_y = 0;

// Diagnostic log. Lives at exile-audio.log next to exile-debug.log so
// the two stay decoupled — Audio doesn't need to talk to Game just to
// emit a one-shot setup message. Opened lazily on first call; if the
// open fails (read-only working dir, etc.) we silently drop messages.
//
// std::ofstream rather than std::fopen to dodge MSVC's C4996
// deprecation, and to match how Game::debug_log_ writes its lifecycle
// log.
std::ofstream g_log;
bool g_log_tried = false;

void audio_log(const char* fmt, ...) {
    if (!g_log_tried) {
        g_log_tried = true;
        g_log.open("exile-audio.log",
                   std::ios::out | std::ios::trunc);
        if (g_log.is_open()) {
            g_log << "# exile-cpp audio log\n";
            g_log.flush();
        }
    }
    if (!g_log.is_open()) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log << buf;
    g_log.flush();
}
// 440 Hz at 44100 Hz: phase increment that walks bit 31 over once
// every 1/440 s.
constexpr uint32_t kDebugToneInc =
    static_cast<uint32_t>((static_cast<uint64_t>(440) << 32) / kSampleRate);

#ifndef EXILE_NO_AUDIO
  #if defined(_WIN32)
    // Our own waveOut state on Windows. Two ping-pong int16 buffers
    // matching the per-tick render size; tick() picks whichever has
    // WHDR_DONE set, fills it, and queues it back.
    HWAVEOUT g_wo = nullptr;
    WAVEHDR  g_hdr[2] = {};
    int16_t  g_buf[2][kSamplesPerTick] = {};
  #else
    // fenster_audio.h declares this as a C struct. In C++ the bare
    // name works as a type, but `struct fenster_audio` is unambiguous.
    struct fenster_audio g_fa;
  #endif
#endif
bool g_open = false;

// Map an 8-bit frequency-envelope output to a sample-mixer phase
// increment. After the 6502's EOR #&ff inversion at &1345 and the
// piecewise mapping to the SN76489's 12-bit divisor, higher envelope
// byte = higher audible pitch. We approximate with a 5-octave
// geometric sweep matching the SN76489's actual range
// (~125 Hz..4 kHz):
//     freq_hz(byte) = 125 * 2^(byte * 5 / 255)
// Compile-time: build the full 256-entry table.
constexpr uint64_t phase_inc_for_byte(unsigned byte_value) {
    const uint64_t b = byte_value & 0xff;
    const uint64_t scaled = b * 5u;
    const uint64_t octaves = scaled / 255u;                      // 0..5
    const uint64_t frac    = (scaled % 255u) * 65536u / 255u;    // 0..65535
    // 2^frac approximated linearly between 2^0=1 and 2^1=2.
    const uint64_t mantissa = 65536u + frac;
    const uint64_t freq_q16 = (125ull << octaves) * mantissa;    // freq * 65536
    // phase_inc = freq * 2^32 / sample_rate; we have freq * 65536
    // already, so multiply by 65536 / sample_rate.
    return (freq_q16 * 65536ull) / kSampleRate;
}

constexpr uint32_t make_phase_inc(unsigned n) {
    return static_cast<uint32_t>(phase_inc_for_byte(n));
}

// 256-entry phase-inc table indexed by the live frequency byte. Built
// at compile time.
struct PhaseTable {
    uint32_t v[256];
    constexpr PhaseTable() : v{} {
        for (unsigned i = 0; i < 256; i++) v[i] = make_phase_inc(i);
    }
};
constexpr PhaseTable kPhaseIncTable = PhaseTable{};


// Channel selection — port of the 6502 logic at &1421-&144f.
// CH_PRIORITY always grabs slot 0; CH_ANY scans 1..3 for an inactive
// slot, falling back to the quietest if all three are busy.
int pick_channel(int hint) {
    if (hint == Audio::CH_PRIORITY) return 0;
    for (int i = 1; i < kNumChannels; i++) {
        if (!channel_busy(g_channels[i])) return i;
    }
    int q = 1;
    uint8_t qv = g_channels[1].vol.value;
    for (int i = 2; i < kNumChannels; i++) {
        if (g_channels[i].vol.value < qv) {
            q = i;
            qv = g_channels[i].vol.value;
        }
    }
    return q;
}

// ============================================================================
// Envelopes table — verbatim port of &2db9-&2e88. Each play_sound
// param byte (params[0] for volume envelope, params[2] for frequency)
// is an offset into this array, and the engine walks (duration, delta)
// pairs from there. Bit 7 of a duration byte means "loop marker".
// ============================================================================
constexpr uint8_t kEnvelopesTable[208] = {
    0x88, 0x03, 0x10, 0x03, 0xf0, 0x80, 0x03, 0xc0,  // &2db9
    0x01, 0x04, 0x05, 0x06, 0x82, 0x0c, 0xfe, 0x03,
    0x03, 0x80, 0x01, 0xf9, 0x02, 0x01, 0x02, 0xff,  // &2dc9
    0x08, 0xf0, 0x08, 0xf8, 0x01, 0xfb, 0x87, 0x03,
    0xa1, 0x03, 0x81, 0x80, 0x83, 0x02, 0xa3, 0x02,  // &2dd9
    0x81, 0x80, 0x3e, 0x00, 0x01, 0x06, 0x0a, 0x0c,
    0x0a, 0x00, 0x78, 0xfe, 0x0f, 0x10, 0x0f, 0xf4,  // &2de9
    0xf8, 0x04, 0x02, 0x05, 0xfe, 0x80, 0x08, 0xf0,
    0x0a, 0xf8, 0x0c, 0xfc, 0x92, 0x03, 0x02, 0x03,  // &2df9
    0x01, 0x03, 0x00, 0x03, 0xff, 0x03, 0xfe, 0x80,
    0x03, 0x03, 0x03, 0x01, 0x03, 0x00, 0x0c, 0xff,  // &2e09
    0x04, 0x20, 0x05, 0x10, 0x05, 0x08, 0x04, 0xe0,
    0x05, 0xf0, 0x05, 0xf8, 0xe1, 0x01, 0xf8, 0x08,  // &2e19
    0x01, 0xe1, 0x01, 0x1a, 0x0d, 0xfe, 0x80, 0x01,
    0x18, 0x64, 0x00, 0x88, 0x02, 0x00, 0x01, 0x40,  // &2e29
    0x02, 0x00, 0x01, 0xbc, 0x90, 0x03, 0x00, 0x01,
    0x0c, 0x03, 0x00, 0x01, 0xf4, 0x80, 0x10, 0x00,  // &2e39
    0x01, 0x2f, 0x10, 0x00, 0x01, 0xf9, 0x10, 0x00,
    0x01, 0xf1, 0x83, 0x10, 0xf0, 0x87, 0x04, 0x20,  // &2e49
    0x02, 0xfd, 0x02, 0xc0, 0x80, 0x0b, 0x14, 0x83,
    0x03, 0xf0, 0x03, 0x10, 0x80, 0x03, 0xbc, 0x07,  // &2e59
    0x06, 0x82, 0x02, 0xfe, 0x04, 0x02, 0x80, 0x11,
    0xff, 0x0b, 0x14, 0x01, 0x02, 0x02, 0x83, 0x0a,  // &2e69
    0x03, 0x04, 0x09, 0x88, 0x01, 0x0b, 0x01, 0xe0,
    0x01, 0x15, 0xac, 0x01, 0x14, 0x01, 0xec, 0x80,  // &2e79
    0x10, 0xff, 0x14, 0xf8, 0x28, 0x02, 0x01, 0x00,
};

// ============================================================================
// Per-frame envelope advance. Returns true if the sub-envelope is
// still alive after this tick, false when it has run out of stages.
// Mirrors update_sound_envelope at &1399-&13e3 line for line.
// ============================================================================
static bool update_envelope(EnvState& e) {
    // &139a: ended already?
    if (e.duration == 0) return false;

    // &13a2: only start a new stage when the current one's frame
    // budget has run out.
    if (e.stage_duration == 0) {
        if (e.loops_remaining != 0) {
            // mid-loop; don't decrement the top-level duration.
        } else {
            // &13ac DEC volume_duration ; BEQ leave.
            e.duration--;
            if (e.duration == 0) return false;
        }

        // &13b1 INY ; LDA envelopes_table,Y. Y is one past stage_offset
        // (which currently points at the previous delta byte) for the
        // first stage; or at the marker, on the next encounter.
        uint8_t y = e.stage_offset + 1;
        uint8_t b = kEnvelopesTable[y];

        // &13b5 BPL not_loop. Bit 7 set = loop marker.
        if (b & 0x80) {
            // &13b7-&13c3 fresh-loop vs continuation.
            if (e.loops_remaining == 0) {
                // Fresh marker: low 7 bits = repeat count; save the
                // byte after as the loop body's start.
                e.loops_remaining = b & 0x7f;
                y++;
                e.loop_offset = y;
            } else {
                e.loops_remaining--;
            }
            // &13c6: jump to the body's first stage and read its
            // duration byte.
            y = e.loop_offset;
            b = kEnvelopesTable[y];
        }

        // &13cc-&13d1: stage_duration = duration byte; advance Y past
        // it; stage_offset now points at the delta byte.
        e.stage_duration = b;
        y++;
        e.stage_offset = y;
    }

    // &13d4-&13df: apply delta, decrement stage_duration. ADC's
    // carry-in is 0 on this path (CLC at &1399, preserved through the
    // intermediate flag-only ops).
    e.value = static_cast<uint8_t>(e.value + kEnvelopesTable[e.stage_offset]);
    e.stage_duration--;
    return true;
}

}  // namespace

namespace Audio {

bool open() {
    if (g_open) return true;
    for (auto& ch : g_channels) ch = {};
#ifdef EXILE_NO_AUDIO
    audio_log("disabled at compile time (EXILE_NO_AUDIO)\n");
    g_open = false;
    return false;
#elif defined(_WIN32)
    // Build a correctly-aligned PCM 16-bit mono format and open the
    // default waveOut device. nBlockAlign and nAvgBytesPerSec are
    // computed from the other fields (nBlockAlign was the bit
    // fenster_audio.h gets wrong).
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 1;
    wfx.nSamplesPerSec  = kSampleRate;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;     // 2
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;       // 88200
    wfx.cbSize          = 0;

    MMRESULT mr = waveOutOpen(&g_wo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        audio_log("waveOutOpen failed (mr=%u)\n", static_cast<unsigned>(mr));
        g_wo = nullptr;
        return false;
    }
    for (int i = 0; i < 2; i++) {
        g_hdr[i] = {};
        g_hdr[i].lpData         = reinterpret_cast<LPSTR>(g_buf[i]);
        g_hdr[i].dwBufferLength = sizeof(g_buf[i]);
        waveOutPrepareHeader(g_wo, &g_hdr[i], sizeof(WAVEHDR));
        // Mark as DONE so the first tick picks them up; we don't queue
        // any silence up front — first frame's samples land directly.
        g_hdr[i].dwFlags |= WHDR_DONE;
    }
    g_open = true;
    audio_log("open ok: %d Hz, 16-bit mono, %d-sample buffer (waveOut)\n",
              kSampleRate, kSamplesPerTick);
    return true;
#else
    std::memset(&g_fa, 0, sizeof(g_fa));
    const int rc = fenster_audio_open(&g_fa);
    if (rc != 0) {
        audio_log("fenster_audio_open failed (rc=%d)\n", rc);
        return false;
    }
    g_open = true;
    audio_log("open ok: %d Hz, mono float, %d-sample buffer\n",
              kSampleRate, FENSTER_AUDIO_BUFSZ);
    return true;
#endif
}

void close() {
    if (!g_open) return;
#if defined(EXILE_NO_AUDIO)
    // nothing to release
#elif defined(_WIN32)
    if (g_wo) {
        waveOutReset(g_wo);
        for (int i = 0; i < 2; i++) {
            waveOutUnprepareHeader(g_wo, &g_hdr[i], sizeof(WAVEHDR));
        }
        waveOutClose(g_wo);
        g_wo = nullptr;
    }
#else
    fenster_audio_close(&g_fa);
#endif
    g_open = false;
}

void play(int channel_hint, const uint8_t params[4]) {
    if (!g_open) return;
    const int ch_idx = pick_channel(channel_hint);
    Channel& ch = g_channels[ch_idx];

    // Same channel-arbitration rule as &1426-&144f: don't trample an
    // already-playing channel unless the new initial volume is at
    // least as loud. Stops per-frame callers (engine fire, power pod)
    // from re-arming forever.
    const uint8_t vol_init = params[1] & 0xf0;     // 0x00..0xf0 in 0x10 steps
    if (channel_busy(ch) && vol_init < ch.vol.value) {
        return;
    }

    // &1471-&1492: load both envelope param pairs into channel state.
    // 6502 stores top-nibble-of-byte-1 verbatim as the initial value
    // (so the resolution is 0x10 steps, 0x00..0xf0), and the bottom
    // nibble is the envelope's total stage count.
    ch.vol.value           = params[1] & 0xf0;
    ch.vol.duration        = params[1] & 0x0f;
    ch.vol.stage_offset    = params[0];
    ch.vol.stage_duration  = 0;       // forces start_stage on first tick
    ch.vol.loops_remaining = 0;
    ch.vol.loop_offset     = 0;

    ch.freq.value           = params[3] & 0xf0;
    ch.freq.duration        = params[3] & 0x0f;
    ch.freq.stage_offset    = params[2];
    ch.freq.stage_duration  = 0;
    ch.freq.loops_remaining = 0;
    ch.freq.loop_offset     = 0;

    // play() (no source) → no distance attenuation.
    ch.volume_reduction = 0;

    ch.phase_inc = kPhaseIncTable.v[ch.freq.value];
}

void play_at(int channel_hint, const uint8_t params[4],
             uint8_t src_x, uint8_t src_y) {
    if (!g_open) return;

    // 6502 &1415-&141a get_object_distance_from_screen_centre + the
    // CMP #&10 / BCS leave gate. Chebyshev distance in tiles; bigger
    // than 16 → don't play at all. The screen-centre / listener
    // position tracks the player (camera follows).
    const int dx = static_cast<int8_t>(src_x - g_listener_x);
    const int dy = static_cast<int8_t>(src_y - g_listener_y);
    const int adx = dx < 0 ? -dx : dx;
    const int ady = dy < 0 ? -dy : dy;
    const int distance = adx > ady ? adx : ady;
    if (distance >= 0x10) return;

    play(channel_hint, params);

    // 6502 &141c-&1465: distance × 16 stored as a per-channel volume
    // reduction. play() above already picked the channel and zeroed
    // its reduction; overwrite with the distance-derived value.
    const int ch_idx = pick_channel(channel_hint);
    g_channels[ch_idx].volume_reduction =
        static_cast<uint8_t>(distance * 0x10);
}

void set_listener(uint8_t x, uint8_t y) {
    g_listener_x = x;
    g_listener_y = y;
}

void tick() {
    if (!g_open) return;

    // Advance per-frame state. For each channel, step both the volume
    // and frequency envelopes (port of update_sound_channel_loop at
    // &1320-&1397). The frequency envelope's new value is mapped to a
    // phase_inc so the mixer's pitch tracks the envelope sweep.
    //
    // After the volume envelope ends, fade the volume to silence.
    // The 6502 at &1328-&132f does SBC #&01 with C=0 → -2 per frame.
    // Doubled here to -4 per frame because the BBC audio's perceived
    // duration is shorter than a strict 1:1 port suggests — the
    // SN76489's 4-bit volume cuts off at the nibble boundary, which
    // happens twice as fast at -4.
    for (auto& ch : g_channels) {
        if (!channel_audible(ch) && !channel_busy(ch)) continue;
        if (!update_envelope(ch.vol)) {
            ch.vol.value = (ch.vol.value > 4)
                ? static_cast<uint8_t>(ch.vol.value - 4)
                : 0;
        }
        if (update_envelope(ch.freq)) {
            ch.phase_inc = kPhaseIncTable.v[ch.freq.value];
        }
    }

    // Render kSamplesPerTick float samples and push. Each channel
    // contributes (square × volume/0xf0) × headroom; four channels at
    // full volume sum to ±1.0.
    float buf[kSamplesPerTick];
    for (int i = 0; i < kSamplesPerTick; i++) {
        float mix = 0.0f;
        if (g_debug_tone) {
            // Constant 440 Hz square wave at half scale. Bypasses the
            // channel/envelope logic entirely so this only proves the
            // device, write loop, and game's tick cadence are working.
            mix += (g_debug_phase & 0x80000000u) ? +0.5f : -0.5f;
            g_debug_phase += kDebugToneInc;
        }
        for (auto& ch : g_channels) {
            if (!channel_audible(ch)) continue;
            const float square = (ch.phase & 0x80000000u) ? +1.0f : -1.0f;
            // 6502 &1376-&137e: chip_volume = vol.value - reduction,
            // floored at zero. Renderer uses the reduced value's top
            // nibble (SN76489's 4-bit attenuation), matching how the
            // chip actually attenuated distant sounds.
            const int reduced = int(ch.vol.value) - int(ch.volume_reduction);
            const uint8_t out = (reduced > 0) ? static_cast<uint8_t>(reduced) : 0;
            const float amp = (out >> 4) / 15.0f;
            mix += square * amp * 0.25f;
            ch.phase += ch.phase_inc;
        }
        buf[i] = std::clamp(mix, -1.0f, 1.0f);
    }

    int wrote_to_slot = -1;
#if defined(EXILE_NO_AUDIO)
    (void)buf;
#elif defined(_WIN32)
    // Find a buffer the device has finished playing; convert our
    // float[-1, +1] samples into int16, queue it, mark not-done.
    for (int i = 0; i < 2; i++) {
        if (g_hdr[i].dwFlags & WHDR_DONE) {
            for (int j = 0; j < kSamplesPerTick; j++) {
                int v = static_cast<int>(buf[j] * 32767.0f);
                if (v >  32767) v =  32767;
                if (v < -32768) v = -32768;
                g_buf[i][j] = static_cast<int16_t>(v);
            }
            g_hdr[i].dwFlags &= ~WHDR_DONE;
            waveOutWrite(g_wo, &g_hdr[i], sizeof(WAVEHDR));
            wrote_to_slot = i;
            break;
        }
    }
#else
    fenster_audio_write(&g_fa, buf, kSamplesPerTick);
    wrote_to_slot = 0;
#endif

#ifndef EXILE_NO_AUDIO
    // First-call diagnostic: confirms tick() actually fires and that
    // we're pushing samples to a real buffer (slot != -1 means a
    // WHDR_DONE buffer was found and filled).
    static bool logged_first_tick = false;
    if (!logged_first_tick) {
        audio_log("first tick: pushed %d samples to slot %d (debug_tone=%d)\n",
                  kSamplesPerTick, wrote_to_slot, g_debug_tone ? 1 : 0);
        logged_first_tick = true;
    }
#endif
}

void set_debug_tone(bool on) {
    g_debug_tone = on;
    g_debug_phase = 0;
    audio_log("debug tone %s\n", on ? "ON" : "off");
}

}  // namespace Audio
