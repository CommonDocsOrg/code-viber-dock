#include "Zip.h"

#include <algorithm>
#include <cstring>

// Round 29 phase 29B. The DEFLATE decoder follows the structure of Mark Adler's puff.c (the zlib
// distribution's reference inflater, written for clarity and to be safe on any input): canonical
// Huffman codes decoded a bit at a time, every read and every back reference bounds-checked. It is
// slower than zlib's table-driven inflate, which does not matter for a widget of a few hundred KB.
// Every malformed input ends in an error, never in a read or write outside the buffers: the input
// position is checked on every bit, the output only grows by push_back after the cap is checked, and
// a distance is checked against what was already written.

namespace Zip {

namespace {

uint16_t U16(const std::string& d, size_t at) {
    return static_cast<uint16_t>(static_cast<uint8_t>(d[at]) | (static_cast<uint8_t>(d[at + 1]) << 8));
}
uint32_t U32(const std::string& d, size_t at) {
    return static_cast<uint32_t>(U16(d, at)) | (static_cast<uint32_t>(U16(d, at + 2)) << 16);
}

// --- inflate ---------------------------------------------------------------------------------------

struct Failure {
    const char* what;
};

constexpr int kMaxBits = 15;     // the longest code
constexpr int kMaxLCodes = 286;  // literal / length codes
constexpr int kMaxDCodes = 30;   // distance codes
constexpr int kFixLCodes = 288;  // the fixed code's literal / length symbols

struct Huffman {
    short count[kMaxBits + 1];  // codes of each length
    short symbol[kFixLCodes];   // the symbols in canonical order
};

class Inflater {
public:
    Inflater(const uint8_t* in, size_t n, std::string& out, size_t maxOut) : in_(in), n_(n), out_(out), max_(maxOut) {}

    void Run() {
        int last = 0;
        do {
            last = Bits(1);
            const int type = Bits(2);
            if (type == 0) {
                Stored();
            } else if (type == 1) {
                Fixed();
            } else if (type == 2) {
                Dynamic();
            } else {
                throw Failure{"invalid block type"};
            }
        } while (!last);
    }

private:
    int Bits(int need) {
        uint32_t val = bitBuf_;
        while (bitCount_ < need) {
            if (pos_ >= n_) throw Failure{"the compressed data ends early"};
            val |= static_cast<uint32_t>(in_[pos_++]) << bitCount_;
            bitCount_ += 8;
        }
        bitBuf_ = val >> need;
        bitCount_ -= need;
        return static_cast<int>(val & ((1u << need) - 1));
    }

    void Put(uint8_t b) {
        if (out_.size() >= max_) throw Failure{"the data is larger than it says"};
        out_.push_back(static_cast<char>(b));
    }

    void Stored() {
        bitBuf_ = 0;  // to the next byte
        bitCount_ = 0;
        if (pos_ + 4 > n_) throw Failure{"the compressed data ends early"};
        const unsigned len = in_[pos_] | (in_[pos_ + 1] << 8);
        const unsigned nlen = in_[pos_ + 2] | (in_[pos_ + 3] << 8);
        pos_ += 4;
        if (len != (~nlen & 0xFFFFu)) throw Failure{"a stored block's length does not match its complement"};
        if (pos_ + len > n_) throw Failure{"the compressed data ends early"};
        if (out_.size() + len > max_) throw Failure{"the data is larger than it says"};
        out_.append(reinterpret_cast<const char*>(in_ + pos_), len);
        pos_ += len;
    }

    int Decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= Bits(1);
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw Failure{"an invalid Huffman code"};
    }

    // Builds the canonical code from the lengths. Returns 0 for a complete code, > 0 when it is
    // incomplete (codes left over), < 0 when it is over-subscribed (impossible).
    static int Construct(Huffman& h, const short* length, int n) {
        for (int len = 0; len <= kMaxBits; ++len) h.count[len] = 0;
        for (int s = 0; s < n; ++s) h.count[length[s]]++;
        if (h.count[0] == n) return 0;  // no codes: complete, but decoding fails
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) return left;
        }
        short offs[kMaxBits + 1];
        offs[1] = 0;
        for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = static_cast<short>(offs[len] + h.count[len]);
        for (int s = 0; s < n; ++s) {
            if (length[s] != 0) h.symbol[offs[length[s]]++] = static_cast<short>(s);
        }
        return left;
    }

    void Codes(const Huffman& lencode, const Huffman& distcode) {
        static const short kLBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                         31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short kLExt[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short kDBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                         193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short kDExt[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = Decode(lencode);
            if (symbol < 256) {
                Put(static_cast<uint8_t>(symbol));
            } else if (symbol == 256) {
                return;  // the end of the block
            } else {
                symbol -= 257;
                if (symbol >= 29) throw Failure{"an invalid length code"};
                const int len = kLBase[symbol] + Bits(kLExt[symbol]);
                symbol = Decode(distcode);
                if (symbol >= 30) throw Failure{"an invalid distance code"};
                const size_t dist = static_cast<size_t>(kDBase[symbol] + Bits(kDExt[symbol]));
                if (dist > out_.size()) throw Failure{"a distance points before the start"};
                if (out_.size() + static_cast<size_t>(len) > max_) throw Failure{"the data is larger than it says"};
                for (int k = 0; k < len; ++k) out_.push_back(out_[out_.size() - dist]);  // may overlap itself
            }
        }
    }

    void Fixed() {
        struct FixedCodes {
            Huffman lencode{}, distcode{};
            FixedCodes() {
                short lengths[kFixLCodes];
                int s = 0;
                for (; s < 144; ++s) lengths[s] = 8;
                for (; s < 256; ++s) lengths[s] = 9;
                for (; s < 280; ++s) lengths[s] = 7;
                for (; s < kFixLCodes; ++s) lengths[s] = 8;
                Construct(lencode, lengths, kFixLCodes);
                for (s = 0; s < kMaxDCodes; ++s) lengths[s] = 5;
                Construct(distcode, lengths, kMaxDCodes);
            }
        };
        static const FixedCodes codes;  // built once (a function-local static: thread-safe)
        Codes(codes.lencode, codes.distcode);
    }

    void Dynamic() {
        static const short kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        short lengths[kMaxLCodes + kMaxDCodes];
        const int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
        if (nlen > kMaxLCodes || ndist > kMaxDCodes) throw Failure{"too many length or distance codes"};
        int index = 0;
        for (; index < ncode; ++index) lengths[kOrder[index]] = static_cast<short>(Bits(3));
        for (; index < 19; ++index) lengths[kOrder[index]] = 0;
        Huffman lencode{}, distcode{};
        if (Construct(lencode, lengths, 19) != 0) throw Failure{"an incomplete code-length code"};
        index = 0;
        while (index < nlen + ndist) {
            int symbol = Decode(lencode);
            if (symbol < 16) {
                lengths[index++] = static_cast<short>(symbol);
            } else {
                int len = 0;
                if (symbol == 16) {
                    if (index == 0) throw Failure{"a repeat with no length before it"};
                    len = lengths[index - 1];
                    symbol = 3 + Bits(2);
                } else if (symbol == 17) {
                    symbol = 3 + Bits(3);
                } else {
                    symbol = 11 + Bits(7);
                }
                if (index + symbol > nlen + ndist) throw Failure{"too many lengths"};
                while (symbol--) lengths[index++] = static_cast<short>(len);
            }
        }
        if (lengths[256] == 0) throw Failure{"no end-of-block code"};
        int err = Construct(lencode, lengths, nlen);
        if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) throw Failure{"an invalid literal / length code"};
        err = Construct(distcode, lengths + nlen, ndist);
        if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) throw Failure{"an invalid distance code"};
        Codes(lencode, distcode);
    }

    const uint8_t* in_;
    size_t n_;
    size_t pos_ = 0;
    uint32_t bitBuf_ = 0;
    int bitCount_ = 0;
    std::string& out_;
    size_t max_;
};

bool ValidUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        int extra = 0;
        uint32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + static_cast<size_t>(extra) >= s.size()) return false;  // the sequence is cut off
        for (int k = 1; k <= extra; ++k) {
            const uint8_t d = static_cast<uint8_t>(s[i + static_cast<size_t>(k)]);
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        // Overlong forms, surrogates and values past U+10FFFF are not UTF-8.
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += static_cast<size_t>(extra) + 1;
    }
    return true;
}

bool ReservedName(std::string segment) {
    const size_t dot = segment.find('.');
    if (dot != std::string::npos) segment.resize(dot);
    for (char& c : segment) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    if (segment == "CON" || segment == "PRN" || segment == "AUX" || segment == "NUL" || segment == "CONIN$" || segment == "CONOUT$") return true;
    if (segment.size() == 4 && (segment.compare(0, 3, "COM") == 0 || segment.compare(0, 3, "LPT") == 0) && segment[3] >= '0' &&
        segment[3] <= '9') {
        return true;
    }
    // COM¹ / COM² / COM³ (superscript digits, UTF-8 C2 B9 / C2 B2 / C2 B3) are reserved too.
    if (segment.size() == 5 && (segment.compare(0, 3, "COM") == 0 || segment.compare(0, 3, "LPT") == 0) &&
        static_cast<uint8_t>(segment[3]) == 0xC2 &&
        (static_cast<uint8_t>(segment[4]) == 0xB9 || static_cast<uint8_t>(segment[4]) == 0xB2 || static_cast<uint8_t>(segment[4]) == 0xB3)) {
        return true;
    }
    return false;
}

}  // namespace

uint32_t Crc32(const void* data, size_t n) {
    struct Table {
        uint32_t t[256];
        Table() {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
        }
    };
    static const Table table;  // built once (thread-safe)
    uint32_t crc = 0xFFFFFFFFu;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) crc = table.t[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

bool Inflate(const uint8_t* in, size_t n, std::string& out, size_t maxOut, std::string& error) {
    const size_t start = out.size();
    try {
        Inflater(in, n, out, start + maxOut).Run();
        return true;
    } catch (const Failure& f) {
        error = std::string("deflate: ") + f.what;
    } catch (...) {
        error = "deflate: out of memory";
    }
    out.resize(start);
    return false;
}

bool NormalizeName(const std::string& raw, bool utf8, std::string& out, std::string& error) {
    out.clear();
    if (raw.empty()) {
        error = "an entry has no name";
        return false;
    }
    std::string s = raw;
    std::replace(s.begin(), s.end(), '\\', '/');
    const std::string shown = "\"" + s.substr(0, 120) + "\"";
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) {
            error = shown + " has a control character in its name";
            return false;
        }
        if (c >= 0x80 && !utf8) {
            error = shown + " has a non-ASCII name without the archive's UTF-8 flag";
            return false;
        }
        if (c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') {
            error = shown + (c == ':' ? " has a ':' in its name (a drive letter or an alternate data stream)"
                                      : " has a character Windows does not allow in file names");
            return false;
        }
    }
    if (utf8 && !ValidUtf8(s)) {
        error = shown + " is not valid UTF-8";
        return false;
    }
    if (s.front() == '/') {
        error = shown + " is an absolute path";
        return false;
    }
    if (s.size() > 240) {
        error = shown + " is longer than 240 characters";
        return false;
    }
    const bool directory = s.back() == '/';
    if (directory) s.pop_back();
    size_t start = 0;
    int depth = 0;
    while (start <= s.size()) {
        size_t slash = s.find('/', start);
        if (slash == std::string::npos) slash = s.size();
        const std::string segment = s.substr(start, slash - start);
        if (segment.empty()) {
            error = shown + " has an empty folder name";
            return false;
        }
        if (segment == "." || segment == "..") {
            error = shown + " has a \"" + segment + "\" segment";
            return false;
        }
        if (segment.back() == '.' || segment.back() == ' ') {
            error = shown + " has a name ending in a dot or a space";
            return false;
        }
        if (ReservedName(segment)) {
            error = shown + " uses a reserved device name (" + segment + ")";
            return false;
        }
        if (++depth > 16) {
            error = shown + " is nested more than 16 folders deep";
            return false;
        }
        start = slash + 1;
    }
    out = s + (directory ? "/" : "");
    return true;
}

bool ReadDirectory(const std::string& d, const Limits& limits, std::vector<Entry>& entries, std::string& error) {
    entries.clear();
    if (d.size() > limits.maxArchiveBytes) {
        error = "the archive is larger than " + std::to_string(limits.maxArchiveBytes >> 20) + " MB";
        return false;
    }
    if (d.size() < 22) {
        error = "not a zip archive (too short)";
        return false;
    }
    // The end of central directory record: the last one in the final 64 KB + 22 bytes.
    size_t eocd = std::string::npos;
    const size_t lowest = d.size() > 22 + 65535 ? d.size() - 22 - 65535 : 0;
    for (size_t at = d.size() - 22 + 1; at-- > lowest;) {
        if (U32(d, at) == 0x06054b50u && at + 22 + U16(d, at + 20) == d.size()) {
            eocd = at;
            break;
        }
    }
    if (eocd == std::string::npos) {
        error = "not a zip archive (no end of central directory)";
        return false;
    }
    const uint16_t disk = U16(d, eocd + 4), cdDisk = U16(d, eocd + 6);
    const uint16_t onDisk = U16(d, eocd + 8), total = U16(d, eocd + 10);
    const uint32_t cdSize = U32(d, eocd + 12), cdOffset = U32(d, eocd + 16);
    if (disk != 0 || cdDisk != 0 || onDisk != total) {
        error = "split (multi-disk) archives are not supported";
        return false;
    }
    if (total == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu) {
        error = "zip64 archives are not supported";
        return false;
    }
    if (total > limits.maxEntries) {
        error = "the archive has " + std::to_string(total) + " entries (at most " + std::to_string(limits.maxEntries) + ")";
        return false;
    }
    if (static_cast<size_t>(cdOffset) + cdSize > eocd) {
        error = "the central directory is outside the archive";
        return false;
    }
    size_t at = cdOffset;
    uint64_t sum = 0;
    std::vector<std::string> lower;  // the names, ASCII lower case (duplicates)
    for (uint16_t k = 0; k < total; ++k) {
        if (at + 46 > static_cast<size_t>(cdOffset) + cdSize || U32(d, at) != 0x02014b50u) {
            error = "the central directory is damaged";
            return false;
        }
        const uint16_t madeBy = U16(d, at + 4), flags = U16(d, at + 8), method = U16(d, at + 10);
        const uint32_t crc = U32(d, at + 16), csize = U32(d, at + 20), usize = U32(d, at + 24);
        const uint16_t nameLen = U16(d, at + 28), extraLen = U16(d, at + 30), commentLen = U16(d, at + 32);
        const uint32_t external = U32(d, at + 38), local = U32(d, at + 42);
        const size_t next = at + 46 + nameLen + extraLen + commentLen;
        if (next > static_cast<size_t>(cdOffset) + cdSize) {
            error = "the central directory is damaged";
            return false;
        }
        Entry e;
        if (!NormalizeName(d.substr(at + 46, nameLen), (flags & 0x0800) != 0, e.name, error)) return false;
        const std::string& n = e.name;
        if (flags & 0x0041) {  // bit 0 encrypted, bit 6 strong encryption
            error = "\"" + n + "\" is encrypted";
            return false;
        }
        if (method != 0 && method != 8) {
            error = "\"" + n + "\" uses compression method " + std::to_string(method) + " (only stored and deflate are supported)";
            return false;
        }
        if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || local == 0xFFFFFFFFu) {
            error = "\"" + n + "\" needs zip64";
            return false;
        }
        // Links: a Unix entry's mode (the high half of the external attributes) or the Windows
        // attributes (the low half; host 0 FAT, 10 NTFS, 11 VFAT, 14 VFAT) of a reparse point.
        const uint8_t host = static_cast<uint8_t>(madeBy >> 8);
        if (host == 3 || host == 19) {
            const uint32_t type = (external >> 16) & 0xF000u;
            if (type == 0xA000u) {
                error = "\"" + n + "\" is a symbolic link";
                return false;
            }
            if (type != 0 && type != 0x8000u && type != 0x4000u) {
                error = "\"" + n + "\" is not a plain file or folder";
                return false;
            }
        }
        if ((host == 0 || host == 10 || host == 11 || host == 14) && (external & 0x400u)) {
            error = "\"" + n + "\" is a link (a reparse point)";
            return false;
        }
        e.directory = n.back() == '/';
        e.method = method;
        e.crc = crc;
        e.compressed = csize;
        e.size = usize;
        if (!e.directory) {
            if (usize > limits.maxFileBytes) {
                error = "\"" + n + "\" is larger than " + std::to_string(limits.maxFileBytes >> 20) + " MB";
                return false;
            }
            sum += usize;
            if (sum > limits.maxTotalBytes) {
                error = "the files add up to more than " + std::to_string(limits.maxTotalBytes >> 20) + " MB";
                return false;
            }
            if (usize > (64u << 10) && static_cast<uint64_t>(usize) > static_cast<uint64_t>(csize) * limits.maxRatio) {
                error = "\"" + n + "\" is compressed more than " + std::to_string(limits.maxRatio) + " to 1";
                return false;
            }
            if (method == 0 && csize != usize) {
                error = "\"" + n + "\" is stored with a size that does not match";
                return false;
            }
        } else if (usize != 0) {
            error = "\"" + n + "\" is a folder with data";
            return false;
        }
        // Its local header and data, before the central directory.
        if (static_cast<size_t>(local) + 30 > cdOffset || U32(d, local) != 0x04034b50u) {
            error = "\"" + n + "\" has a damaged local header";
            return false;
        }
        e.dataOffset = static_cast<size_t>(local) + 30 + U16(d, local + 26) + U16(d, local + 28);
        if (e.dataOffset + csize > cdOffset) {
            error = "\"" + n + "\" has data outside the archive";
            return false;
        }
        std::string key = n;
        for (char& c : key) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (std::find(lower.begin(), lower.end(), key) != lower.end()) {
            error = "\"" + n + "\" is in the archive twice";
            return false;
        }
        lower.push_back(std::move(key));
        entries.push_back(std::move(e));
        at = next;
    }
    return true;
}

bool Extract(const std::string& d, const Entry& e, std::string& out, std::string& error) {
    out.clear();
    if (e.directory) return true;
    if (e.dataOffset + e.compressed > d.size()) {
        error = "\"" + e.name + "\" has data outside the archive";
        return false;
    }
    const auto* p = reinterpret_cast<const uint8_t*>(d.data()) + e.dataOffset;
    if (e.method == 0) {
        out.assign(reinterpret_cast<const char*>(p), e.compressed);
    } else {
        out.reserve(e.size);
        if (!Inflate(p, e.compressed, out, e.size, error)) {
            error = "\"" + e.name + "\": " + error;
            return false;
        }
    }
    if (out.size() != e.size) {
        error = "\"" + e.name + "\" is shorter than it says";
        out.clear();
        return false;
    }
    if (Crc32(out.data(), out.size()) != e.crc) {
        error = "\"" + e.name + "\" is damaged (its CRC-32 does not match)";
        out.clear();
        return false;
    }
    return true;
}

}  // namespace Zip
