#include "player/input.h"
#include "rendering/renderer.h"

void InputHandler::clear() {
    state_ = {};
}

void InputHandler::process_key(int key) {
    switch (key) {
        case InputKey::LEFT:  state_.move_left    = true; break;
        case InputKey::RIGHT: state_.move_right   = true; break;
        case InputKey::UP:    state_.move_up      = true; break;
        case InputKey::DOWN:  state_.move_down    = true; break;
        case 'z': case 'Z': state_.jetpack      = true; break;
        case 'x': case 'X': state_.fire         = true; break;
        case '\n': case '\r': case InputKey::ENTER:
            state_.pickup_drop = true; break;
        case 'y': case 'Y': state_.whistle_one   = true; break;
        case 'u': case 'U': state_.whistle_two   = true; break;
        case 'q': case 'Q': state_.quit         = true; break;
        case '1': state_.weapon_select = 0; break;
        case '2': state_.weapon_select = 1; break;
        case '3': state_.weapon_select = 2; break;
        case '4': state_.weapon_select = 3; break;
        case '5': state_.weapon_select = 4; break;
        default: break;
    }
}
