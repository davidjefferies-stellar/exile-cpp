#pragma once
#include <cstdint>

// Game actions mapped from keyboard input
struct InputState {
    bool move_left    = false;
    bool move_right   = false;
    bool move_up      = false;   // Jetpack thrust up
    bool move_down    = false;   // Jetpack thrust down
    bool jetpack      = false;   // Z key: toggle jetpack
    bool fire         = false;   // Space: fire weapon
    bool turn_around  = false;   // Tab: swap player facing direction
    bool lie_down     = false;   // Left Ctrl: lie down (&16 handle_lying_down)
    bool boost        = false;   // Right Ctrl: jetpack booster (&15 handle_using_booster)
    bool pickup_drop  = false;   // Return key: pick up / drop object (legacy toggle)
    bool pickup       = false;   // , key: pick up touching object
    bool drop         = false;   // M key: drop held object straight down
    bool throw_obj    = false;   // . key: drop with a horizontal kick (throw)
    bool store        = false;   // S key: store held object in a pocket
    bool retrieve     = false;   // G key: retrieve top pocket as held object
                                  // (6502 key at &0c — R is the 6502's
                                  // remember-position key, not retrieve)
    bool remember_pos = false;   // R key: remember current position for later
                                  // teleport (&2c3c handle_remembering_position)
    bool teleport     = false;   // T key: teleport back to last remembered
                                  // position (&0cc1 handle_teleporting)
    bool aim_up       = false;   // O key: raise weapon aim
    bool aim_down     = false;   // K key: lower weapon aim
    bool aim_centre   = false;   // I key: centre aim
    bool toggle_map_activation = false; // \ key: switch activation anchor
                                        // between player and camera centre
    bool toggle_pause = false;          // Esc key: freeze world updates
                                        // (rendering and input still run)
    bool whistle_one  = false;   // U key: play whistle one (activates Chatter)
    bool whistle_two  = false;   // Y key: play whistle two (Chatter produces power pod)
    bool quit         = false;   // Set when the window's close button was
                                  // clicked — no key binding. Game::process_input
                                  // sees this and breaks the run loop.
    bool save_game    = false;   // ';' key: write game-state save file
    bool load_game    = false;   // "'" key: read game-state save file
    bool save_map     = false;   // '\\' key: write current landscape grid to
                                  // exile.map (used by the in-game editor).
    bool tert_data_dec = false;   // '[' key: editor — decrement the data byte
                                  // of the tertiary at the highlighted cell.
    bool tert_data_inc = false;   // ']' key: editor — increment that data byte.
    // Frame-rewind scrubber. Numpad '+' / '-' step through the snapshot
    // ring buffer; pressing Esc while scrubbing resumes the sim from the
    // scrubbed frame (branching timeline). dump_all_frames (':' = Shift+';')
    // writes the entire ring buffer as a multi-frame trace.
    bool scrub_forward    = false;
    bool scrub_back       = false;
    bool dump_all_frames  = false;
    // 'J' (for jsbeeb): one-shot manual sync — POSTs the player's current
    // x/y position to the jsbeeb dev server's /bridge/poke endpoint.
    bool bridge_push      = false;
    uint8_t weapon_select = 0xff; // 0xff = no change, 0-5 = select weapon
};

// Process raw key input into game actions
class InputHandler {
public:
    void process_key(int key);
    void clear();
    const InputState& state() const { return state_; }
    // Used by save/load restore — overwrite the per-frame state directly
    // instead of replaying key presses. Cleared again at the next frame's
    // begin so loaded-into state lasts a single frame unless re-asserted.
    void set_state(const InputState& s) { state_ = s; }

private:
    InputState state_;
};
