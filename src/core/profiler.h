#pragma once
#include <chrono>
#include <cstdint>
#include <array>

// Zero-dependency frame profiler. Accumulates wall-clock time per named
// section across a window of frames; Game emits a summary to
// exile-debug.log once per ~second and resets. Sections are a fixed enum
// so the hot path is an array index, never a map lookup or allocation.
// Enabled via [debug] profile; when disabled the Scope guard skips the
// clock reads so it costs effectively nothing.
namespace Profile {

enum class Section : int {
    Frame = 0,      // whole tick() of work (excludes the frame-rate sleep)
    Input,
    Player,
    Objects,
    Events,
    Particles,
    Render,         // whole render() — the sub-sections below are inside it
    RenderBegin,    // renderer begin_frame (framebuffer clear)
    RenderTiles,    // water columns + the vp_w x vp_h tile loop (parent)
    RenderWater,    //   water-column backdrop loop
    RenderTileResolve,//  resolve_tile_with_tertiary + spawn_tertiary_object
    RenderTileInfo, //   TileRenderInfo build (per-cell landscape queries)
    RenderTileBlit, //   renderer render_tile (the pixel blit)
    RenderObjects,  // primary sprite blits
    RenderEnd,      // HUD + end_frame (present to window)
    COUNT,
};

inline const char* name(Section s) {
    switch (s) {
        case Section::Frame:         return "frame";
        case Section::Input:         return "input";
        case Section::Player:        return "player";
        case Section::Objects:       return "objects";
        case Section::Events:        return "events";
        case Section::Particles:     return "particles";
        case Section::Render:        return "render";
        case Section::RenderBegin:       return "  begin";
        case Section::RenderTiles:       return "  tiles";
        case Section::RenderWater:       return "    water";
        case Section::RenderTileResolve: return "    resolve";
        case Section::RenderTileInfo:    return "    info";
        case Section::RenderTileBlit:    return "    blit";
        case Section::RenderObjects:     return "  objects";
        case Section::RenderEnd:         return "  end";
        default:                         return "?";
    }
}

class Profiler {
public:
    void set_enabled(bool e) { enabled_ = e; }
    bool enabled() const { return enabled_; }

    void add(Section s, uint64_t ns) {
        int i = static_cast<int>(s);
        total_ns_[i] += ns;
        calls_[i]    += 1;
    }
    void mark_frame() { frames_++; }

    int      frames() const { return frames_; }
    uint64_t total_ns(Section s) const {
        return total_ns_[static_cast<int>(s)];
    }
    // Mean milliseconds per frame for a section over the current window.
    double avg_ms(Section s) const {
        if (frames_ <= 0) return 0.0;
        return total_ns_[static_cast<int>(s)] / 1.0e6 / frames_;
    }

    void reset() {
        total_ns_.fill(0);
        calls_.fill(0);
        frames_ = 0;
    }

private:
    bool enabled_ = false;
    int  frames_  = 0;
    std::array<uint64_t, static_cast<size_t>(Section::COUNT)> total_ns_{};
    std::array<uint64_t, static_cast<size_t>(Section::COUNT)> calls_{};
};

// RAII section timer. Reads the clock only while the profiler is enabled,
// so leaving the [debug] profile flag off has no measurable cost.
class Scope {
public:
    Scope(Profiler& p, Section s) : prof_(p), section_(s) {
        if (prof_.enabled()) start_ = std::chrono::steady_clock::now();
    }
    ~Scope() {
        if (!prof_.enabled()) return;
        auto end = std::chrono::steady_clock::now();
        prof_.add(section_, static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start_).count()));
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Profiler& prof_;
    Section   section_;
    std::chrono::steady_clock::time_point start_{};
};

} // namespace Profile
