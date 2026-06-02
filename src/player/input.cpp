#include "player/input.h"
#include "rendering/renderer.h"

void InputHandler::clear() {
    state_ = {};
}

void InputHandler::process_key(int key) {
    switch (key) {
        // Jetpack movement: QWPL letter keys. Arrow keys drive the
        // map-view pan instead (matching the original 6502's cursor-
        // key slew). Q + W sit adjacent on the left of the keyboard,
        // P + L stack on the right.
        case 'q': case 'Q': state_.move_left  = true; break;
        case 'w': case 'W': state_.move_right = true; break;
        case 'p': case 'P': state_.move_up    = true; break;
        case 'l': case 'L': state_.move_down  = true; break;
        case InputKey::LEFT:  state_.pan_left  = true; break;
        case InputKey::RIGHT: state_.pan_right = true; break;
        case InputKey::UP:    state_.pan_up    = true; break;
        case InputKey::DOWN:  state_.pan_down  = true; break;
        case ' ':           state_.fire         = true; break;
        case InputKey::TAB:        state_.turn_around = true; break;
        case InputKey::CTRL_LEFT:   state_.lie_down  = true; break;
        // 6502 &26 SHIFT — modifier key for f-key energy transfer
        // (handle_changing_weapon_or_transferring_energy &2ce3).
        case InputKey::SHIFT_LEFT:  state_.shift_held = true; break;
        case InputKey::CTRL_RIGHT:
        case '[': case '{':         state_.boost     = true; break;
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
        case InputKey::ESCAPE: state_.toggle_pause = true; break;
        case InputKey::CLOSE_REQUESTED: state_.quit = true; break;
        // BBC ROM key table: Y -> &2c99 handle_playing_whistle_two
        // (chatter produces power pod). U -> &2cac handle_playing_
        // whistle_one (activates chatter). Annotator comments in the
        // disassembly's key table swap the names but the routines at
        // those addresses are as above (see &2c99 / &2cac at line 7700).
        case 'y': case 'Y': state_.whistle_two   = true; break;
        case 'u': case 'U': state_.whistle_one   = true; break;
        case 'j': case 'J': state_.bridge_push   = true; break;  // jsbeeb sync
        // ';' / '\'' (save / load game) live on the Esc pause menu now.
        case ':': state_.dump_all_frames  = true; break;  // Shift+;
        case InputKey::KEYPAD_STAR:  state_.scrub_forward = true; break;
        case InputKey::KEYPAD_MINUS: state_.scrub_back    = true; break;
        case '\\': case '|': state_.save_map     = true; break;
        // Editor data-byte bump. `[` was the natural pair for `]` but
        // is now the jetpack-boost alias, so the editor side keeps
        // only the -/= fallbacks.
        case '-': case '_':   state_.tert_data_dec = true; break;
        case ']': case '}': case '=': case '+':
                              state_.tert_data_inc = true; break;
        case '1': state_.weapon_select = 0; break;
        case '2': state_.weapon_select = 1; break;
        case '3': state_.weapon_select = 2; break;
        case '4': state_.weapon_select = 3; break;
        case '5': state_.weapon_select = 4; break;
        default: break;
    }
}
