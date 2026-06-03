// jsbeeb bridge — SSE world-state push to the BBC emulator at
// https://bbc.xania.org/. Press J in Exile to push a one-shot snapshot;
// after the initial poke jsbeeb runs free, so its keyboard and ours
// are independent. Paste this into the jsbeeb tab's DevTools console:
//   const es=new EventSource('http://localhost:5173/bridge/events');es.onmessage=e=>{const m=JSON.parse(e.data);if(m.type==='poke')m.writes.forEach(w=>processor.writemem(w.addr,w.value));};

#include "emu/jsbeeb_bridge.h"

// Release builds OMIT this entire implementation — Chrome / SmartScreen
// flag unsigned exes that listen on a network port (the embedded HTTP +
// SSE server). Debug builds define EXILE_JSBEEB_BRIDGE in exile.vcxproj
// to compile the real bridge in; the no-op stubs at the bottom take
// over otherwise. Pressing J still works, it just silently does nothing.

#ifdef EXILE_JSBEEB_BRIDGE

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

void handle_client(SOCKET s) {
    std::string req = recv_request(s);
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
            "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Access-Control-Allow-Private-Network: true\r\n\r\n";
        send_all(s, preflight, std::strlen(preflight));
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

}  // namespace JsbeebBridge

#else  // !EXILE_JSBEEB_BRIDGE — no-op stubs for release builds.

namespace JsbeebBridge {

void poke(const std::vector<Write>& /*writes*/) {}
bool start(int /*port*/, const char* /*doc_root*/) { return false; }
void stop() {}

}  // namespace JsbeebBridge

#endif  // EXILE_JSBEEB_BRIDGE
