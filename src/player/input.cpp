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
        case ' ':           state_.fire         = true; break;
        case InputKey::TAB:        state_.turn_around = true; break;
        case InputKey::CTRL_LEFT:  state_.lie_down    = true; break;
        case InputKey::CTRL_RIGHT: state_.boost       = true; break;
        case '\n': case '\r': case InputKey::ENTER:
            state_.pickup_drop = true; break;
        case ',': case '<': state_.pickup    = true; break;
        case 'm': case 'M': state_.drop      = true; break;
        // 'm' is now drop; the map-activation toggle moved to '\\' so the
        // pickup/drop/throw cluster (, m .) sits naturally under one hand.
        case '.': case '>': state_.throw_obj = true; break;
        case 's': case 'S': state_.store        = true; break;
        case 'g': case 'G': state_.retrieve     = true; break;  // 6502 &0c
        case 'r': case 'R': state_.remember_pos = true; break;  // 6502 &1b
        case 't': case 'T': state_.teleport     = true; break;  // 6502 &1a
        case 'i': case 'I': state_.aim_centre   = true; break;
        case 'k': case 'K': state_.aim_down     = true; break;
        case 'o': case 'O': state_.aim_up       = true; break;
        // map-activation toggle used to live on 'W'; moved to the debug-
        // panel "Map mode" checkbox in the HUD strip. W is now free.
        case 'p': case 'P': state_.toggle_pause          = true; break;
        case 'y': case 'Y': state_.whistle_one   = true; break;
        case 'u': case 'U': state_.whistle_two   = true; break;
        case 'q': case 'Q': state_.quit         = true; break;
        case ';': case ':': state_.save_game     = true; break;
        case '\'': case '"': state_.load_game    = true; break;
        case '1': state_.weapon_select = 0; break;
        case '2': state_.weapon_select = 1; break;
        case '3': state_.weapon_select = 2; break;
        case '4': state_.weapon_select = 3; break;
        case '5': state_.weapon_select = 4; break;
        default: break;
    }
}
