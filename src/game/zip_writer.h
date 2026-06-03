#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Minimal STORE-method (no compression) zip writer. Enough to bundle the
// debug log + a few save snapshots for a GitHub issue attachment.
namespace ZipWriter {

struct Entry {
    std::string name;     // archive-internal path; '/' separators
    std::string data;     // raw file contents
};

// Builds a self-contained .zip blob from the given entries. Returns false
// only if a file write actually fails — the in-memory build always
// succeeds.
bool write(const std::string& out_path, const std::vector<Entry>& entries);

} // namespace ZipWriter
