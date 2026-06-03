#pragma once
#include <cstdint>
#include <vector>

// Embedded HTTP + Server-Sent-Events server. Serves jsbeeb's static
// `dist/` (produced by a one-time `npm run build`) and a /bridge/events
// SSE channel that our port pokes into when 'J' is pressed. No npm at
// runtime; the browser loads jsbeeb directly from our exe.
//
// Release builds compile this out — Chrome and Defender flag unsigned
// exes that listen on a network port, and the bridge is a debug-only
// convenience. Define EXILE_JSBEEB_BRIDGE (set by Debug|x64 in
// exile.vcxproj) to enable. The Release stubs in jsbeeb_bridge.cpp's
// `#else` branch make J a no-op and skip the network stack entirely.
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

// Optional explicit lifecycle for tests or non-default config. Safe to
// call repeatedly; subsequent calls are no-ops.
bool start(int port = 5173, const char* doc_root = "../jsbeeb/dist");
void stop();

}  // namespace JsbeebBridge
