#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Round 29 phase 29B: a small, bounded reader of .zip archives for installing a widget from a zip
// (Widgets::PrepareZipInstall), with its own DEFLATE decoder (RFC 1951). No dependency (no zlib, no
// Shell zip folder) and nothing but memory in and memory out: the caller reads the archive into a
// buffer (it is capped) and writes the files itself, under rules of its own.
//
// What is accepted, deliberately little: the "stored" and "deflate" methods, no encryption, no
// zip64, no multi-disk archives, at most Limits::maxEntries entries; every entry name passes
// NormalizeName; the sizes the central directory declares are capped per file and in total, and an
// entry whose declared size is more than maxRatio times its compressed size is refused before any
// byte of it is inflated (a "zip bomb"); while inflating, the output can never pass the declared
// size (the decoder stops at it), and the result must match the declared size and CRC-32 exactly.
// Entries that are links are refused: a Unix symlink (version made by Unix, S_IFLNK in the mode) or
// any entry with the Windows reparse-point attribute.
//
// Errors are short English text (ASCII) naming the entry.
namespace Zip {

struct Limits {
    size_t maxArchiveBytes = 20u << 20;  // the .zip file itself
    size_t maxEntries = 200;             // files and folders
    size_t maxFileBytes = 8u << 20;      // one file, uncompressed (a widget file is read up to 8 MB)
    size_t maxTotalBytes = 20u << 20;    // every file, uncompressed
    uint32_t maxRatio = 100;             // uncompressed / compressed, checked for files above 64 KB
};

struct Entry {
    std::string name;        // normalized: "lib/util.js"; a folder ends with '/'
    bool directory = false;
    uint16_t method = 0;     // 0 stored, 8 deflate
    uint32_t crc = 0;
    uint32_t compressed = 0;
    uint32_t size = 0;       // uncompressed
    size_t dataOffset = 0;   // the compressed bytes in the archive buffer
};

// The archive's entries from its central directory, every one checked (names, methods, flags,
// links, sizes, the ratio, that its data lies inside the buffer). False with `error` on the first
// problem: the archive is then refused as a whole.
bool ReadDirectory(const std::string& archive, const Limits& limits, std::vector<Entry>& entries, std::string& error);

// One file entry's bytes, inflated (or copied) and checked against its size and CRC-32.
bool Extract(const std::string& archive, const Entry& entry, std::string& out, std::string& error);

// Raw DEFLATE data in[0, n) into `out` (appended), at most `maxOut` bytes in all: more is an error,
// as is any malformed stream (a bad block type, an over-subscribed or incomplete code, a distance
// before the start, running out of input).
bool Inflate(const uint8_t* in, size_t n, std::string& out, size_t maxOut, std::string& error);

uint32_t Crc32(const void* data, size_t n);

// An entry's name as a package path, or why not: '\' read as '/'; refused when absolute ("/x",
// "\\server\x"), with a drive letter or any ':' (an alternate data stream: "a.js:evil"), with an
// empty, "." or ".." segment, a segment ending in a dot or a space (Windows drops those, so two
// names could land on one file), one of < > " | ? * or a control character, a reserved device name
// as a segment (CON, PRN, AUX, NUL, COM0-9, LPT0-9, CONIN$, CONOUT$, also with an extension:
// "con.txt"), non-ASCII bytes without the archive's UTF-8 flag (or invalid UTF-8 with it), more than
// 16 levels or 240 characters. A folder keeps its trailing '/'.
bool NormalizeName(const std::string& raw, bool utf8, std::string& out, std::string& error);

}  // namespace Zip
