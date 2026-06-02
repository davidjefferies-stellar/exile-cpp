#include "game/zip_writer.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace ZipWriter {

namespace {

// "abc/def.txt" -> ["abc", "def.txt"]. Forward slashes only (entries
// are produced internally so we don't need to handle '\\').
std::vector<std::string> split_path(const std::string& name) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == '/') {
            if (i > start) parts.emplace_back(name.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

bool write_file(const std::filesystem::path& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (!data.empty()) {
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(out);
}

// Pick a sibling temp directory next to out_path. Using the same volume
// guarantees rename-style ops work and avoids littering %TEMP%.
std::filesystem::path make_staging_dir(const std::filesystem::path& out_path) {
    std::random_device rd;
    std::uniform_int_distribution<uint64_t> dist;
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::ostringstream name;
        name << ".zipstage-" << std::hex << dist(rd);
        std::filesystem::path candidate =
            out_path.parent_path() / name.str();
        std::error_code ec;
        if (std::filesystem::create_directory(candidate, ec) && !ec) {
            return candidate;
        }
    }
    return {};
}

// Quote a path for PowerShell's single-quoted string context.
std::string quote_for_ps(const std::filesystem::path& p) {
    std::string s = p.string();
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "''";
        else           out += c;
    }
    out += "'";
    return out;
}

bool run_compress_archive(const std::filesystem::path& staging,
                          const std::filesystem::path& out_path) {
    // -Path '<stage>\*' picks up every staged entry. -Force overwrites
    // any leftover zip at the destination. -CompressionLevel Fastest
    // keeps issue bundles cheap to build; we're not optimising on-disk
    // size here.
    std::string cmd = "powershell -NoProfile -NonInteractive -Command "
                      "\"Compress-Archive -Path ";
    cmd += quote_for_ps(staging / "*");
    cmd += " -DestinationPath ";
    cmd += quote_for_ps(out_path);
    cmd += " -CompressionLevel Fastest -Force\"";

#ifdef _WIN32
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::string mutable_cmd = cmd;  // CreateProcessA may modify the buffer.
    BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(),
                             nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr,
                             &si, &pi);
    if (!ok) return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
#else
    return std::system(cmd.c_str()) == 0;
#endif
}

} // namespace

bool write(const std::string& out_path_str,
           const std::vector<Entry>& entries) {
    std::filesystem::path out_path(out_path_str);
    std::error_code ec;

    std::filesystem::path staging = make_staging_dir(out_path);
    if (staging.empty()) return false;

    bool stage_ok = true;
    for (const Entry& e : entries) {
        std::filesystem::path entry_path = staging;
        for (const std::string& part : split_path(e.name)) {
            entry_path /= part;
        }
        std::filesystem::create_directories(entry_path.parent_path(), ec);
        if (!write_file(entry_path, e.data)) { stage_ok = false; break; }
    }

    // Compress-Archive refuses to overwrite without -Force, but it also
    // refuses if the destination is a directory or read-only — remove
    // first to keep both paths simple.
    if (stage_ok) {
        std::filesystem::remove(out_path, ec);
    }

    bool ok = stage_ok && run_compress_archive(staging, out_path);

    std::filesystem::remove_all(staging, ec);
    return ok;
}

} // namespace ZipWriter
