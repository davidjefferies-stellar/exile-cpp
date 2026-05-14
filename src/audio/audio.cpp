#include "audio/audio.h"

// PCM playback. FENSTER_AUDIO_BUFSZ must equal kSamplesPerTick (882 =
// 44100/50) on both sides — a mismatch leaves a stale tail that clicks.
// Impl lives in fenster_audio_impl.c so the C-only LPSTR cast compiles.
#ifndef EXILE_NO_AUDIO
  #if defined(_WIN32)
    // Hand-rolled waveOut* glue: fenster_audio.h sets nBlockAlign=1,
    // which modern waveOutOpen rejects for 16-bit mono (needs 2).
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

// fenster_audio.h uses float PCM at 44100 Hz mono. Audio::tick fires
// once per game tick; samples/tick = kSampleRate / target_fps. Buffers
// are sized for the SLOWEST supported tick rate (25 Hz = 1764 samples)
// so any allowed rate fits; the live count is g_samples_per_tick.
constexpr int kSampleRate         = 44100;
constexpr int kMinLogicHz         = 25;
constexpr int kSamplesPerTickMax  = kSampleRate / kMinLogicHz; // 1764
constexpr int kNumChannels        = 4;
int g_samples_per_tick = kSamplesPerTickMax;

// Per-channel state. Mirrors 6502 sound_channels_* arrays at &11ac.
// Each channel runs two parallel envelopes (volume + frequency).
// stage_offset points at the DELTA byte (duration was already consumed).
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
    // SN76489 noise: g_channels[0] only. &11a4 starts with &e0
    // (latch noise reg) so ch0 IS the noise gen (CH_PRIORITY).
    uint16_t noise_lfsr;     // 15-bit LFSR; must never be zero.
    uint32_t noise_phase;    // step counter; high-bit overflow shifts the LFSR.
};
// &1426: channel busy while vol envelope runs; free once duration hits
// 0 even if volume is still fading quietly in the background.
static bool channel_busy(const Channel& ch) {
    return ch.vol.duration != 0;
}

// SN76489 only uses the top vol nibble; <0x10 is silent. This
// matches the 6502's perceived fade duration.
static bool channel_audible(const Channel& ch) {
    return (ch.vol.value & 0xf0) != 0;
}

Channel g_channels[kNumChannels];
bool g_debug_tone = false;
uint32_t g_debug_phase = 0;
// Master enable flag — settable via [audio] enabled in exile.ini. When
// false, play/play_at become no-ops and tick pushes silence (the device
// stays open so toggling at runtime won't re-init the audio hw). Default
// true so the legacy "no [audio] section" config keeps making noise.
bool g_enabled = true;
// Listener (player) position; play_at compares against this to compute
// the volume reduction. Updated each tick by Audio::set_listener.
uint8_t g_listener_x = 0;
uint8_t g_listener_y = 0;

// exile-audio.log — lazy open, silently drops if open fails. Disabled
// by default; flipped on via Audio::set_log_enabled before open().
// std::ofstream dodges MSVC C4996 on std::fopen.
std::ofstream g_log;
bool g_log_tried   = false;
bool g_log_enabled = false;

void audio_log(const char* fmt, ...) {
    if (!g_log_enabled) return;
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
    int16_t  g_buf[2][kSamplesPerTickMax] = {};
  #else
    // fenster_audio.h declares this as a C struct. In C++ the bare
    // name works as a type, but `struct fenster_audio` is unambiguous.
    struct fenster_audio g_fa;
  #endif
#endif
bool g_open = false;

// Port of &1345-&135f envelope-byte -> SN76489 10-bit divider:
//   EOR #&ff; pick the smallest Y in {0..3} where (a >= limits[Y]);
//   SBC bases[Y]; ASL A / ROL freq_high Y times. The 6502 then splits
//   the result across the SN76489 latch + data bytes, but composed
//   the 10-bit N simplifies to (a_post_SBC << Y). Frequency follows
//   the 6502's documented formula: freq = 4_000_000 / (32 * N).
constexpr uint16_t divider_from_envelope_byte(unsigned byte_value) {
    constexpr uint8_t limits[4] = { 0x00, 0x40, 0x84, 0xb6 };
    constexpr uint8_t bases[4]  = { 0xe0, 0x10, 0x4a, 0x80 };
    uint8_t a = static_cast<uint8_t>((byte_value & 0xff) ^ 0xff);
    int y = 4;
    while (y > 0) {
        y--;
        if (a >= limits[y]) break;
    }
    a = static_cast<uint8_t>(a - bases[y]);
    uint16_t n = a;
    for (int s = 0; s < y; s++) n = static_cast<uint16_t>(n << 1);
    return n == 0 ? 1 : n;
}

constexpr uint64_t phase_inc_for_byte(unsigned byte_value) {
    const uint16_t n = divider_from_envelope_byte(byte_value);
    // phase_inc = freq * 2^32 / sample_rate, where freq = 125000 / N.
    // Combine into a single integer division to keep precision.
    constexpr uint64_t k = (125000ULL << 32) / kSampleRate;
    return k / n;
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

// &1345-&135f freq envelope -> SN76489 noise control register.
// Byte-for-byte port so bytes near 0xff hit slow/rumble mode.
// Bottom 3 bits drive chip: bit2=white/periodic, bits1-0=rate.
constexpr uint8_t noise_reg_for_byte(unsigned byte_value) {
    constexpr uint8_t limits[4] = { 0x00, 0x40, 0x84, 0xb6 };  // &119c
    constexpr uint8_t bases[4]  = { 0xe0, 0x10, 0x4a, 0x80 };  // &11a0
    uint8_t a = static_cast<uint8_t>((byte_value & 0xff) ^ 0xff);
    int y = 3;
    while (y > 0 && a < limits[y]) y--;
    a = static_cast<uint8_t>(a - bases[y]);
    for (int s = 0; s < y; s++) a = static_cast<uint8_t>(a << 1);
    return static_cast<uint8_t>((a & 0x0f) | 0xe0);
}
constexpr uint32_t noise_step_for_byte(unsigned byte_value) {
    // SN76489AN noise LFSR shift rate at the BBC's 4 MHz chip clock:
    //   NF=00: 4_000_000 / 32 / 16 = 7812.5 Hz
    //   NF=01: 4_000_000 / 32 / 32 = 3906.25 Hz
    //   NF=10: 4_000_000 / 32 / 64 = 1953.125 Hz
    //   NF=11: channel 2 tone freq — we approximate with the slowest
    //          fixed rate since we don't have channel-2's live divider
    //          here. Matches the 6502's `4_000_000 / (32 * N)` formula
    //          documented at &1345.
    constexpr uint64_t rate_hz[4] = { 7813, 3906, 1953, 1953 };
    const uint8_t reg = noise_reg_for_byte(byte_value);
    return static_cast<uint32_t>((rate_hz[reg & 0x03] << 32) / kSampleRate);
}
constexpr bool noise_white_for_byte(unsigned byte_value) {
    return (noise_reg_for_byte(byte_value) & 0x04) != 0;
}
struct NoiseTable {
    uint32_t step[256];
    bool     white[256];
    constexpr NoiseTable() : step{}, white{} {
        for (unsigned i = 0; i < 256; i++) {
            step[i]  = noise_step_for_byte(i);
            white[i] = noise_white_for_byte(i);
        }
    }
};
constexpr NoiseTable kNoiseTable = NoiseTable{};


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

// &2db9-&2e88 envelopes table. params[0]/[2] index into here; engine
// walks (duration, delta) pairs. Bit 7 of duration = loop marker.
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

// &1399-&13e3 update_sound_envelope. Returns true while sub-envelope
// alive, false when stages exhaust.
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

        // &13b1 INY; Y is one past stage_offset (previous delta byte).
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
    audio_log("open ok: %d Hz, 16-bit mono, up to %d-sample buffer (waveOut)\n",
              kSampleRate, kSamplesPerTickMax);
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
    if (!g_open || !g_enabled) return;
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

    // play() (no source) -> no distance attenuation.
    ch.volume_reduction = 0;

    ch.phase_inc = kPhaseIncTable.v[ch.freq.value];

    // Re-seed the LFSR for channel 0 (noise). 0x4000 = single 1 in
    // the top stage; in white mode it saturates within ~15 shifts via
    // the XOR feedback, in periodic mode it produces the chip's
    // characteristic 15-stage pulse train (one '1' cycling through).
    if (ch_idx == 0) {
        ch.noise_lfsr = 0x4000;
        ch.noise_phase = 0;
    }
}

void play_at(int channel_hint, const uint8_t params[4],
             uint8_t src_x, uint8_t src_y) {
    if (!g_open || !g_enabled) return;

    // 6502 &1415-&141a get_object_distance_from_screen_centre + the
    // CMP #&10 / BCS leave gate. Chebyshev distance in tiles; bigger
    // than 16 -> don't play at all. The screen-centre / listener
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

void set_logic_rate(int hz) {
    if (hz < kMinLogicHz) hz = kMinLogicHz;
    const int n = kSampleRate / hz;
    if (n < 1)                  g_samples_per_tick = 1;
    else if (n > kSamplesPerTickMax) g_samples_per_tick = kSamplesPerTickMax;
    else                        g_samples_per_tick = n;
    audio_log("logic rate set to %d Hz (%d samples/tick)\n",
              hz, g_samples_per_tick);
}

void tick() {
    if (!g_open) return;

    // &1320-&1397 update_sound_channel_loop. After vol envelope ends,
    // fade to silence: 6502 &1328-&132f is -2/frame, doubled to -4
    // because SN76489's 4-bit volume cuts off twice as fast.
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

    // Render g_samples_per_tick float samples and push. Buffer sized
    // for the slowest supported tick rate (kSamplesPerTickMax = 1764).
    // Each channel contributes (square × volume/0xf0) × headroom; four
    // channels at full volume sum to ±1.0.
    float buf[kSamplesPerTickMax];
    const int n = g_samples_per_tick;
    for (int i = 0; i < n; i++) {
        float mix = 0.0f;
        if (g_debug_tone) {
            // Constant 440 Hz square wave at half scale. Bypasses the
            // channel/envelope logic entirely so this only proves the
            // device, write loop, and game's tick cadence are working.
            mix += (g_debug_phase & 0x80000000u) ? +0.5f : -0.5f;
            g_debug_phase += kDebugToneInc;
        }
        for (int ci = 0; ci < kNumChannels; ci++) {
            Channel& ch = g_channels[ci];
            if (!channel_audible(ch)) continue;
            // Channel 0 = SN76489 noise. Step a 15-bit LFSR at a
            // freq.value-derived rate; output the LSB. Other channels
            // use the existing square-wave generator.
            float wave;
            if (ci == 0) {
                // freq.value selects one of the chip's four discrete
                // noise modes via the 6502 pipeline (see
                // noise_reg_for_byte). Step rate fires the LFSR once
                // per accumulator wrap.
                const uint32_t step = kNoiseTable.step[ch.freq.value];
                const bool white    = kNoiseTable.white[ch.freq.value];
                const uint32_t prev = ch.noise_phase;
                ch.noise_phase = prev + step;
                if (ch.noise_phase < prev) {
                    // White: bit 0 XOR bit 1 (BBC SN76489AN taps).
                    // Periodic: feed bit 0 back to bit 14 — a single
                    // 1 cycles through 15 stages, producing a buzzy
                    // pulse at step_rate / 15 Hz instead of noise.
                    uint16_t fb = white
                        ? ((ch.noise_lfsr ^ (ch.noise_lfsr >> 1)) & 1u)
                        : (ch.noise_lfsr & 1u);
                    ch.noise_lfsr = static_cast<uint16_t>(
                        (ch.noise_lfsr >> 1) | (fb << 14));
                    if (ch.noise_lfsr == 0) ch.noise_lfsr = 1;
                }
                wave = (ch.noise_lfsr & 1u) ? +1.0f : -1.0f;
            } else {
                wave = (ch.phase & 0x80000000u) ? +1.0f : -1.0f;
                ch.phase += ch.phase_inc;
            }
            // 6502 &1376-&137e: chip_volume = vol.value - reduction,
            // floored at zero. Renderer uses the reduced value's top
            // nibble (SN76489's 4-bit attenuation), matching how the
            // chip actually attenuated distant sounds.
            const int reduced = int(ch.vol.value) - int(ch.volume_reduction);
            const uint8_t out = (reduced > 0) ? static_cast<uint8_t>(reduced) : 0;
            const float amp = (out >> 4) / 15.0f;
            mix += wave * amp * 0.25f;
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
            for (int j = 0; j < n; j++) {
                int v = static_cast<int>(buf[j] * 32767.0f);
                if (v >  32767) v =  32767;
                if (v < -32768) v = -32768;
                g_buf[i][j] = static_cast<int16_t>(v);
            }
            // Per-write size: bytes = samples * sizeof(int16_t). The
            // device plays exactly dwBufferLength bytes; mismatch with
            // g_samples_per_tick would leave a stale tail clicking.
            g_hdr[i].dwBufferLength = static_cast<DWORD>(n * sizeof(int16_t));
            g_hdr[i].dwFlags &= ~WHDR_DONE;
            waveOutWrite(g_wo, &g_hdr[i], sizeof(WAVEHDR));
            wrote_to_slot = i;
            break;
        }
    }
#else
    fenster_audio_write(&g_fa, buf, n);
    wrote_to_slot = 0;
#endif

#ifndef EXILE_NO_AUDIO
    // First-call diagnostic: confirms tick() actually fires and that
    // we're pushing samples to a real buffer (slot != -1 means a
    // WHDR_DONE buffer was found and filled).
    static bool logged_first_tick = false;
    if (!logged_first_tick) {
        audio_log("first tick: pushed %d samples to slot %d (debug_tone=%d)\n",
                  n, wrote_to_slot, g_debug_tone ? 1 : 0);
        logged_first_tick = true;
    }
#endif
}

void set_debug_tone(bool on) {
    g_debug_tone = on;
    g_debug_phase = 0;
    audio_log("debug tone %s\n", on ? "ON" : "off");
}

void set_enabled(bool on) {
    g_enabled = on;
    if (!on) {
        // Silence anything currently playing so the toggle takes effect
        // immediately — without this, in-flight envelopes would keep
        // ringing for up to a second after the disable.
        for (auto& ch : g_channels) {
            ch.vol.value = 0;
            ch.vol.duration = 0;
            ch.vol.stage_duration = 0;
        }
    }
    audio_log("audio %s\n", on ? "ENABLED" : "disabled");
}

bool is_enabled() { return g_enabled; }

void set_log_enabled(bool on) { g_log_enabled = on; }

}  // namespace Audio
