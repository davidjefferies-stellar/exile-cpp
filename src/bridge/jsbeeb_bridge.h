#pragma once
#include <cstdint>
#include <vector>

struct InputState;

// Embedded HTTP + Server-Sent-Events server. Serves jsbeeb's static
// `dist/` (produced by a one-time `npm run build`) and a /bridge/events
// SSE channel that our port pokes into when 'J' is pressed. No npm at
// runtime; the browser loads jsbeeb directly from our exe.
namespace JsbeebBridge {

struct Write {
    uint16_t addr;
    uint8_t  value;
};

// Lazy: the first poke() call boots the server on the default port and
// document root. Returns immediately. Failures (port in use, missing
// dist/) are silent — the J binding is a debug convenience, not a
// load-bearing feature.
void poke(const std::vector<Write>& writes);

// OR-merge the BBC's most-recently-received action_keys_pressed state
// (posted by the in-browser bridge-client to /bridge/input) into the
// supplied InputState. No-op if no input has ever arrived. Lets jsbeeb
// drive our port's player while mirror mode is on.
void merge_into(InputState& s);

// Optional explicit lifecycle for tests or non-default config. Safe to
// call repeatedly; subsequent calls are no-ops.
bool start(int port = 5173, const char* doc_root = "../jsbeeb/dist");
void stop();

}  // namespace JsbeebBridge
