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
    bool pickup_drop  = false;   // Return key: pick up / drop object
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
