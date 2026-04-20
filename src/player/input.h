#pragma once
#include <cstdint>

// Game actions mapped from keyboard input
struct InputState {
    bool move_left    = false;
    bool move_right   = false;
    bool move_up      = false;   // Jetpack thrust up
    bool move_down    = false;   // Jetpack thrust down
    bool jetpack      = false;   // Z key: toggle jetpack
    bool fire         = false;   // X key: fire weapon
    bool pickup_drop  = false;   // Return key: pick up / drop object (legacy toggle)
    bool pickup       = false;   // , key: pick up touching object
    bool drop         = false;   // M key: drop held object straight down
    bool throw_obj    = false;   // . key: drop with a horizontal kick (throw)
    bool store        = false;   // S key: store held object in a pocket
    bool retrieve     = false;   // R key: retrieve top pocket as held object
    bool aim_up       = false;   // O key: raise weapon aim
    bool aim_down     = false;   // K key: lower weapon aim
    bool aim_centre   = false;   // I key: centre aim
    bool toggle_map_activation = false; // \ key: switch activation anchor
                                        // between player and camera centre
    bool toggle_pause = false;          // P key: freeze world updates
                                        // (rendering and input still run)
    bool whistle_one  = false;   // Y key: play whistle one (activates Chatter)
    bool whistle_two  = false;   // U key: play whistle two (Chatter produces power pod)
    bool quit         = false;   // Q key: quit
    uint8_t weapon_select = 0xff; // 0xff = no change, 0-5 = select weapon
};

// Process raw key input into game actions
class InputHandler {
public:
    void process_key(int key);
    void clear();
    const InputState& state() const { return state_; }

private:
    InputState state_;
};
