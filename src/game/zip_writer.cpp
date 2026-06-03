#include "game/zip_writer.h"
#include <cstdint>
#include <cstdio>
#include <fstream>

// PKZIP format reference: PKWARE APPNOTE.TXT v6.3.x sections 4.3.6-4.4.
// We emit STORE (method=0) only — no compression, no encryption, no zip64.
// Compatible with Windows Explorer, 7-Zip, unzip, Python's zipfile.

namespace {

constexpr uint16_t kVersionNeeded   = 20;     // 2.0 (STORE deflate-era)
constexpr uint16_t kMethodStore     = 0;
constexpr uint16_t kDosTime         = 0;      // we leave timestamps zeroed;
constexpr uint16_t kDosDate         = 0x21;   // date=0 is rejected by some
                                              // unzippers, so use 1980-01-01

uint32_t crc32(const std::string& data) {
    static uint32_t table[256] = {0};
    static bool table_ready = false;
    if (!table_ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        table_ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) {
        c = table[(c ^ b) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

void put_u16(std::string& out, uint16_t v) {
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
}
void put_u32(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>((v >> 16) & 0xff));
    out.push_back(static_cast<char>((v >> 24) & 0xff));
}

} // namespace

namespace ZipWriter {

bool write(const std::string& out_path, const std::vector<Entry>& entries) {
    struct Computed { uint32_t crc; uint32_t local_header_offset; };
    std::vector<Computed> meta(entries.size());

    std::string buf;
    buf.reserve(64 * 1024);

    // Local file headers + data.
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        meta[i].crc = crc32(e.data);
        meta[i].local_header_offset = static_cast<uint32_t>(buf.size());

        put_u32(buf, 0x04034b50u);                           // local sig
        put_u16(buf, kVersionNeeded);
        put_u16(buf, 0);                                     // gp flag
        put_u16(buf, kMethodStore);
        put_u16(buf, kDosTime);
        put_u16(buf, kDosDate);
        put_u32(buf, meta[i].crc);
        put_u32(buf, static_cast<uint32_t>(e.data.size()));  // compressed
        put_u32(buf, static_cast<uint32_t>(e.data.size()));  // uncompressed
        put_u16(buf, static_cast<uint16_t>(e.name.size()));
        put_u16(buf, 0);                                     // extra len
        buf.append(e.name);
        buf.append(e.data);
    }

    // Central directory.
    uint32_t cd_offset = static_cast<uint32_t>(buf.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        put_u32(buf, 0x02014b50u);                           // central sig
        put_u16(buf, kVersionNeeded);                        // ver made by
        put_u16(buf, kVersionNeeded);                        // ver needed
        put_u16(buf, 0);                                     // gp flag
        put_u16(buf, kMethodStore);
        put_u16(buf, kDosTime);
        put_u16(buf, kDosDate);
        put_u32(buf, meta[i].crc);
        put_u32(buf, static_cast<uint32_t>(e.data.size()));
        put_u32(buf, static_cast<uint32_t>(e.data.size()));
        put_u16(buf, static_cast<uint16_t>(e.name.size()));
        put_u16(buf, 0);                                     // extra len
        put_u16(buf, 0);                                     // comment len
        put_u16(buf, 0);                                     // disk no.
        put_u16(buf, 0);                                     // internal attr
        put_u32(buf, 0);                                     // external attr
        put_u32(buf, meta[i].local_header_offset);
        buf.append(e.name);
    }
    uint32_t cd_size = static_cast<uint32_t>(buf.size() - cd_offset);

    // End-of-central-directory record.
    put_u32(buf, 0x06054b50u);
    put_u16(buf, 0);                                         // disk no.
    put_u16(buf, 0);                                         // disk w/ CD
    put_u16(buf, static_cast<uint16_t>(entries.size()));     // entries this disk
    put_u16(buf, static_cast<uint16_t>(entries.size()));     // entries total
    put_u32(buf, cd_size);
    put_u32(buf, cd_offset);
    put_u16(buf, 0);                                         // comment len

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    return static_cast<bool>(out);
}

} // namespace ZipWriter
