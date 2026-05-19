// jsbeeb bridge — SSE world-state push + keystroke poll for the BBC
// emulator at https://bbc.xania.org/. Toggle with J after booting Exile
// in jsbeeb, then paste this into the jsbeeb tab's DevTools console:
//   const es=new EventSource('http://localhost:5173/bridge/events');es.onmessage=e=>{const m=JSON.parse(e.data);if(m.type==='poke')m.writes.forEach(w=>processor.writemem(w.addr,w.value));};
//   const B=0x126b,N=0x27,U='http://localhost:5173/bridge/input',L=Array(N).fill(-1);(function loop(){const a=Array(N);let d=false;for(let i=0;i<N;i++){const b=processor.readmem(B+i)&0x80?1:0;a[i]=b;if(b!==L[i])d=true;}if(d){L.splice(0,N,...a);fetch(U,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({actions:a}),keepalive:true}).catch(()=>{});}requestAnimationFrame(loop);})();
// Pressing J pushes the initial snapshot; jsbeeb keystrokes then drive
// the C++ player until you press J again.

#include "bridge/jsbeeb_bridge.h"
#include "player/input.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace JsbeebBridge {
namespace {

std::atomic<bool> running_{false};
std::atomic<bool> wsa_ready_{false};
SOCKET listen_sock_ = INVALID_SOCKET;
std::thread acceptor_;
std::string doc_root_;
std::mutex sse_mutex_;
std::vector<SOCKET> sse_clients_;
std::once_flag init_flag_;

// Most recently POSTed BBC action_keys_pressed snapshot. The browser's
// bridge-client polls processor.readmem(0x126b..0x1291) each frame and
// POSTs to /bridge/input; merge_into() OR-s these into the port's
// InputState. 0x80 = key pressed, 0x00 = released.
constexpr int ACTION_COUNT = 0x27;
std::mutex input_mutex_;
uint8_t bridge_actions_[ACTION_COUNT] = {};

// Blocking write of `len` bytes. Returns false on any send() failure so
// the caller can drop the socket from the SSE client list.
bool send_all(SOCKET s, const char* buf, size_t len) {
    while (len > 0) {
        int n = ::send(s, buf, static_cast<int>(len), 0);
        if (n <= 0) return false;
        buf += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

// Receive up to "\r\n\r\n" (end of HTTP headers). Caps at 8 KB so a
// pathological client can't tie up the worker thread.
std::string recv_request(SOCKET s) {
    std::string buf;
    char tmp[1024];
    while (buf.size() < 8192) {
        int n = ::recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) return {};
        buf.append(tmp, static_cast<size_t>(n));
        if (buf.find("\r\n\r\n") != std::string::npos) break;
    }
    return buf;
}

bool parse_request(const std::string& req,
                   std::string& method, std::string& path) {
    auto eol = req.find("\r\n");
    if (eol == std::string::npos) return false;
    std::istringstream ss(req.substr(0, eol));
    std::string version;
    return static_cast<bool>(ss >> method >> path >> version);
}

const char* content_type_for(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot);
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".js")    return "application/javascript; charset=utf-8";
    if (ext == ".mjs")   return "application/javascript; charset=utf-8";
    if (ext == ".css")   return "text/css; charset=utf-8";
    if (ext == ".json")  return "application/json; charset=utf-8";
    if (ext == ".map")   return "application/json; charset=utf-8";
    if (ext == ".svg")   return "image/svg+xml";
    if (ext == ".png")   return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif")   return "image/gif";
    if (ext == ".ico")   return "image/x-icon";
    if (ext == ".wav")   return "audio/wav";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".woff")  return "font/woff";
    if (ext == ".ttf")   return "font/ttf";
    return "application/octet-stream";
}

void send_status(SOCKET s, int code, const char* reason) {
    std::ostringstream o;
    o << "HTTP/1.1 " << code << ' ' << reason
      << "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    auto str = o.str();
    send_all(s, str.data(), str.size());
}

void serve_static(SOCKET s, const std::string& path) {
    // Reject .. traversal so the doc root can't be escaped.
    if (path.find("..") != std::string::npos) {
        send_status(s, 403, "Forbidden");
        return;
    }

    std::string file_path = doc_root_;
    if (path == "/" || path.empty()) {
        file_path += "/index.html";
    } else {
        // Strip query string if any.
        auto q = path.find('?');
        file_path += (q == std::string::npos) ? path : path.substr(0, q);
    }

    std::ifstream f(file_path, std::ios::binary | std::ios::ate);
    if (!f) {
        send_status(s, 404, "Not Found");
        return;
    }
    auto size = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::string body(size, '\0');
    if (size > 0) f.read(body.data(), static_cast<std::streamsize>(size));

    std::ostringstream resp;
    resp << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: " << content_type_for(file_path) << "\r\n"
         << "Content-Length: " << size << "\r\n"
         << "Access-Control-Allow-Origin: *\r\n"
         << "Connection: close\r\n\r\n";
    std::string headers = resp.str();
    send_all(s, headers.data(), headers.size());
    if (size > 0) send_all(s, body.data(), body.size());
}

// Send the SSE preamble, then register the socket. The caller does
// NOT close it — poke() owns the socket from here on, closing it only
// when a send() fails (browser disconnected / page reloaded).
void serve_sse(SOCKET s) {
    static const char* headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Private-Network: true\r\n"
        "X-Accel-Buffering: no\r\n"
        "\r\n"
        "retry: 5000\n\n";
    if (!send_all(s, headers, std::strlen(headers))) {
        closesocket(s);
        return;
    }
    // Bound send() so a paused / hung browser tab can't freeze the
    // game tick when poke() loops over sse_clients_ under the mutex.
    // 50 ms is plenty for a tiny SSE event over loopback; if the
    // socket can't drain in that, treat it as dead and reap it.
    DWORD timeout_ms = 50;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout_ms),
               sizeof(timeout_ms));
    std::lock_guard<std::mutex> lock(sse_mutex_);
    sse_clients_.push_back(s);
}

// Pull Content-Length out of the request headers (case-insensitive).
// Returns 0 if absent / malformed. Cap to keep the read bounded.
size_t parse_content_length(const std::string& req) {
    static const char* keys[] = { "Content-Length:", "content-length:" };
    for (const char* k : keys) {
        auto p = req.find(k);
        if (p == std::string::npos) continue;
        p += std::strlen(k);
        while (p < req.size() && (req[p] == ' ' || req[p] == '\t')) ++p;
        long v = std::atol(req.c_str() + p);
        if (v < 0) v = 0;
        if (v > 65536) v = 65536;  // sanity cap
        return static_cast<size_t>(v);
    }
    return 0;
}

// Read full request including body. recv_request returns up to the
// double-CRLF; if there's a body, we extract whatever came in the same
// recv and read any remainder until Content-Length bytes are present.
std::string recv_full(SOCKET s, std::string& body_out) {
    std::string req = recv_request(s);
    body_out.clear();
    if (req.empty()) return req;
    auto hdr_end = req.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return req;
    size_t want = parse_content_length(req);
    body_out = req.substr(hdr_end + 4);
    while (body_out.size() < want) {
        char tmp[1024];
        int n = ::recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        body_out.append(tmp, static_cast<size_t>(n));
    }
    return req.substr(0, hdr_end);
}

// Parse {"actions":[0,1,...]} into bridge_actions_. Lenient: ignores
// whitespace, accepts any 0/non-0 integer, stops at ACTION_COUNT.
void parse_input_body(const std::string& body) {
    auto arr = body.find('[');
    auto end = body.find(']', arr == std::string::npos ? 0 : arr);
    if (arr == std::string::npos || end == std::string::npos) return;
    uint8_t parsed[ACTION_COUNT] = {};
    size_t idx = 0;
    size_t i = arr + 1;
    while (i < end && idx < ACTION_COUNT) {
        while (i < end && (body[i] == ' ' || body[i] == ',' ||
                            body[i] == '\t' || body[i] == '\n')) ++i;
        if (i >= end) break;
        long v = std::atol(body.c_str() + i);
        parsed[idx++] = (v != 0) ? 0x80 : 0;
        while (i < end && body[i] != ',') ++i;
    }
    std::lock_guard<std::mutex> lock(input_mutex_);
    std::memcpy(bridge_actions_, parsed, ACTION_COUNT);
}

void handle_client(SOCKET s) {
    std::string body;
    std::string req = recv_full(s, body);
    if (req.empty()) { closesocket(s); return; }
    std::string method, path;
    if (!parse_request(req, method, path)) { closesocket(s); return; }

    if (method == "OPTIONS") {
        // Access-Control-Allow-Private-Network is the Chrome 102+ PNA
        // requirement that lets an HTTPS page (e.g. bbc.xania.org) reach
        // http://localhost. Without it, EventSource is blocked silently.
        const char* preflight =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Access-Control-Allow-Private-Network: true\r\n\r\n";
        send_all(s, preflight, std::strlen(preflight));
        closesocket(s);
        return;
    }

    if (method == "POST" && path.starts_with("/bridge/input")) {
        parse_input_body(body);
        const char* ok =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 0\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n\r\n";
        send_all(s, ok, std::strlen(ok));
        closesocket(s);
        return;
    }

    if (method == "GET" && path.starts_with("/bridge/events")) {
        serve_sse(s);
        return;  // keep open
    }

    if (method == "GET") {
        serve_static(s, path);
    } else {
        send_status(s, 405, "Method Not Allowed");
    }
    closesocket(s);
}

void acceptor_loop() {
    while (running_.load()) {
        sockaddr_in addr{};
        int addrlen = sizeof(addr);
        SOCKET c = ::accept(listen_sock_,
                            reinterpret_cast<sockaddr*>(&addr), &addrlen);
        if (c == INVALID_SOCKET) {
            if (!running_.load()) return;
            continue;
        }
        std::thread(handle_client, c).detach();
    }
}

}  // namespace

bool start(int port, const char* doc_root) {
    if (running_.load()) return true;
    doc_root_ = doc_root;

    if (!wsa_ready_.load()) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        wsa_ready_.store(true);
    }

    listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCKET) return false;

    int opt = 1;
    setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // localhost only
    addr.sin_port = htons(static_cast<u_short>(port));
    if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) != 0) {
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
        return false;
    }
    if (::listen(listen_sock_, 8) != 0) {
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
        return false;
    }

    running_.store(true);
    acceptor_ = std::thread(acceptor_loop);
    return true;
}

void stop() {
    running_.store(false);
    if (listen_sock_ != INVALID_SOCKET) {
        closesocket(listen_sock_);  // unblocks accept()
        listen_sock_ = INVALID_SOCKET;
    }
    if (acceptor_.joinable()) acceptor_.join();
    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        for (SOCKET s : sse_clients_) closesocket(s);
        sse_clients_.clear();
    }
    if (wsa_ready_.load()) {
        WSACleanup();
        wsa_ready_.store(false);
    }
}

void poke(const std::vector<Write>& writes) {
    if (writes.empty()) return;
    std::call_once(init_flag_, [] { start(); });
    // If the server never came up (port in use, bind failed) bail
    // before we touch the SSE list. Pressing J is meant to be a
    // no-op in that case, not crash the game.
    if (!running_.load()) return;

    std::ostringstream body;
    body << R"({"type":"poke","writes":[)";
    for (size_t i = 0; i < writes.size(); ++i) {
        if (i) body << ',';
        body << R"({"addr":)" << writes[i].addr
             << R"(,"value":)" << static_cast<unsigned>(writes[i].value)
             << '}';
    }
    body << "]}";

    std::string msg = "data: ";
    msg += body.str();
    msg += "\n\n";

    std::lock_guard<std::mutex> lock(sse_mutex_);
    for (auto it = sse_clients_.begin(); it != sse_clients_.end();) {
        if (!send_all(*it, msg.data(), msg.size())) {
            closesocket(*it);
            it = sse_clients_.erase(it);
        } else {
            ++it;
        }
    }
}

void merge_into(InputState& s) {
    // Snapshot under the mutex so concurrent POST writes can't tear
    // mid-read. Mapping mirrors the action_keys_pressed indices in
    // the disassembly's action table (line ~3556).
    uint8_t a[ACTION_COUNT];
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        std::memcpy(a, bridge_actions_, ACTION_COUNT);
    }
    if (a[0x0c]) s.retrieve     = true;   // G
    if (a[0x0d]) s.fire         = true;   // SPACE
    if (a[0x0e]) s.aim_centre   = true;   // I
    if (a[0x0f] || a[0x21]) s.move_left  = true;   // LEFT / Q
    if (a[0x10] || a[0x22]) s.move_right = true;   // RIGHT / W
    if (a[0x11] || a[0x23]) s.move_up    = true;   // UP / P
    if (a[0x12] || a[0x25]) s.move_down  = true;   // DOWN / L
    if (a[0x13]) s.aim_down     = true;   // K
    if (a[0x14]) s.aim_up       = true;   // O
    if (a[0x15]) s.boost        = true;   // @
    if (a[0x16]) s.lie_down     = true;   // CTRL
    if (a[0x17]) s.turn_around  = true;   // TAB
    if (a[0x18]) s.whistle_one  = true;   // Y
    if (a[0x19]) s.whistle_two  = true;   // U
    if (a[0x1a]) s.teleport     = true;   // T
    if (a[0x1b]) s.remember_pos = true;   // R
    if (a[0x1c]) s.throw_obj    = true;   // >
    if (a[0x1d]) s.drop         = true;   // M
    if (a[0x1e]) s.pickup       = true;   // <
    if (a[0x1f]) s.store        = true;   // S
    // F0..F5 (BBC actions &01-&06) -> weapon_select 0..5.
    for (uint8_t i = 0; i < 6; ++i) {
        if (a[1 + i]) s.weapon_select = i;
    }
}

}  // namespace JsbeebBridge
