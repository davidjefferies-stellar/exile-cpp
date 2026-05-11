#pragma once
#include "rendering/renderer.h"
#include <vector>

// Headless renderer for tests and --headless / --replay runs. Every
// IRenderer entry point is a no-op (or returns the documented "nothing
// happened" sentinel). get_key() pops from key_queue_ in FIFO order so
// scripted fixtures can inject input frames without going through
// fenster's per-frame poll.
class NullRenderer : public IRenderer {
public:
    bool init() override { return true; }
    void shutdown() override {}
    void begin_frame() override {}
    void end_frame() override {}
    void set_viewport(uint8_t, uint8_t, uint8_t = 0, uint8_t = 0) override {}
    void render_tile(uint8_t, uint8_t, const TileRenderInfo&) override {}
    void render_object(Fixed8_8, Fixed8_8, const SpriteRenderInfo&) override {}
    void render_hud(const PlayerState&) override {}

    // Reasonable viewport so visibility-based code (spawn gates, render
    // culling) doesn't see a degenerate 0×0 window. Matches PixelRenderer's
    // default INITIAL_W / INITIAL_H at 3:1 zoom roughly.
    int viewport_width_tiles()  const override { return 32; }
    int viewport_height_tiles() const override { return 16; }

    int get_key() override {
        if (key_cursor_ >= key_queue_.size()) return InputKey::NONE;
        return key_queue_[key_cursor_++];
    }

    // Test helper: queue a sequence of keys to be consumed by subsequent
    // get_key() calls. Game::process_input drains keys until NONE each
    // frame, so all queued keys for a given frame should be pushed
    // before the corresponding tick() call.
    void queue_key(int key) { key_queue_.push_back(key); }
    void clear_keys() { key_queue_.clear(); key_cursor_ = 0; }

private:
    std::vector<int> key_queue_;
    size_t           key_cursor_ = 0;
};
