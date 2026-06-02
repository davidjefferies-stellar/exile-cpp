#pragma once
#include <string>
#include <vector>

// Builds a zip by staging each entry to a temp directory and shelling out
// to PowerShell's Compress-Archive. Avoids hand-rolling PKZIP. Windows-
// only; the rest of the project is Windows-only too.
namespace ZipWriter {

struct Entry {
    std::string name;     // archive-internal path; '/' separators
    std::string data;     // raw file contents
};

// Writes `entries` to `out_path` as a .zip. Returns false on staging /
// PowerShell / IO failure.
bool write(const std::string& out_path, const std::vector<Entry>& entries);

} // namespace ZipWriter
