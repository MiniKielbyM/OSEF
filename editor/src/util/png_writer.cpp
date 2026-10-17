#include "util/png_writer.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <utility>

namespace osef::editor {

namespace png_detail {

namespace {

std::array<uint32_t, 256> make_crc_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    return table;
}

// Writes bits LSB-first, as deflate requires.
class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}

    void put_bits(uint32_t value, int count) {
        buffer_ |= static_cast<uint64_t>(value) << bit_count_;
        bit_count_ += count;
        while (bit_count_ >= 8) {
            out_.push_back(static_cast<uint8_t>(buffer_ & 0xFF));
            buffer_ >>= 8;
            bit_count_ -= 8;
        }
    }
    // Huffman codes are defined MSB-first, so they're bit-reversed into the stream.
    void put_code(uint32_t code, int length) {
        uint32_t reversed = 0;
        for (int i = 0; i < length; ++i) reversed |= ((code >> i) & 1u) << (length - 1 - i);
        put_bits(reversed, length);
    }
    void flush() {
        if (bit_count_ > 0) out_.push_back(static_cast<uint8_t>(buffer_ & 0xFF));
        buffer_ = 0;
        bit_count_ = 0;
    }

private:
    std::vector<uint8_t>& out_;
    uint64_t buffer_ = 0;
    int bit_count_ = 0;
};

constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                    33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                    1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Fixed Huffman literal/length alphabet (RFC 1951 section 3.2.6).
void put_literal_length_symbol(BitWriter& bw, int symbol) {
    if (symbol <= 143) bw.put_code(0x30 + symbol, 8);
    else if (symbol <= 255) bw.put_code(0x190 + (symbol - 144), 9);
    else if (symbol <= 279) bw.put_code(symbol - 256, 7);
    else bw.put_code(0xC0 + (symbol - 280), 8);
}

void put_match(BitWriter& bw, int length, int distance) {
    int li = 28;
    while (kLengthBase[li] > length) --li;
    put_literal_length_symbol(bw, 257 + li);
    if (kLengthExtra[li]) bw.put_bits(static_cast<uint32_t>(length - kLengthBase[li]), kLengthExtra[li]);
    int di = 29;
    while (kDistBase[di] > distance) --di;
    bw.put_code(static_cast<uint32_t>(di), 5);
    if (kDistExtra[di]) bw.put_bits(static_cast<uint32_t>(distance - kDistBase[di]), kDistExtra[di]);
}

constexpr int kWindow = 32768;
constexpr int kMinMatch = 3;
constexpr int kMaxMatch = 258;
constexpr int kHashBits = 15;
constexpr int kMaxChain = 48;

inline uint32_t hash3(const uint8_t* p) {
    uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                 (static_cast<uint32_t>(p[2]) << 16);
    return (v * 2654435761u) >> (32 - kHashBits);
}

} // namespace

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc) {
    static const std::array<uint32_t, 256> table = make_crc_table();
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t adler32(const uint8_t* data, size_t size, uint32_t adler) {
    uint32_t a = adler & 0xFFFF, b = adler >> 16;
    while (size > 0) {
        size_t n = size < 5552 ? size : 5552;  // largest n that can't overflow before the modulo
        size -= n;
        while (n--) {
            a += *data++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

std::vector<uint8_t> zlib_compress(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    out.reserve(size / 4 + 64);
    out.push_back(0x78);  // CM=8 (deflate), CINFO=7 (32K window)
    out.push_back(0x01);  // FLEVEL=0, FCHECK so that 0x7801 % 31 == 0

    BitWriter bw(out);
    bw.put_bits(1, 1);  // BFINAL
    bw.put_bits(1, 2);  // BTYPE=01 fixed Huffman

    std::vector<int32_t> head(size_t{1} << kHashBits, -1);
    std::vector<int32_t> prev(kWindow, -1);
    auto insert = [&](size_t pos) {
        uint32_t h = hash3(data + pos);
        prev[pos & (kWindow - 1)] = head[h];
        head[h] = static_cast<int32_t>(pos);
    };

    size_t i = 0;
    while (i < size) {
        int best_len = 0, best_dist = 0;
        if (i + kMinMatch <= size) {
            const size_t max_len = std::min<size_t>(kMaxMatch, size - i);
            int32_t cand = head[hash3(data + i)];
            for (int chain = 0; cand >= 0 && chain < kMaxChain; ++chain) {
                const size_t dist = i - static_cast<size_t>(cand);
                if (dist > static_cast<size_t>(kWindow)) break;
                const uint8_t* a = data + cand;
                const uint8_t* b = data + i;
                // Quick reject: a longer match must also agree at index best_len
                // (best_len < max_len here, since a full-length match ends the search).
                if (a[best_len] == b[best_len]) {
                    size_t len = 0;
                    while (len < max_len && a[len] == b[len]) ++len;
                    if (static_cast<int>(len) > best_len) {
                        best_len = static_cast<int>(len);
                        best_dist = static_cast<int>(dist);
                        if (len == max_len) break;
                    }
                }
                cand = prev[static_cast<size_t>(cand) & (kWindow - 1)];
            }
            insert(i);
        }
        if (best_len >= kMinMatch) {
            put_match(bw, best_len, best_dist);
            const size_t end = i + static_cast<size_t>(best_len);
            for (size_t p = i + 1; p < end && p + kMinMatch <= size; ++p) insert(p);
            i = end;
        } else {
            put_literal_length_symbol(bw, data[i]);
            ++i;
        }
    }
    put_literal_length_symbol(bw, 256);  // end of block
    bw.flush();

    const uint32_t adler = adler32(data, size);
    out.push_back(static_cast<uint8_t>(adler >> 24));
    out.push_back(static_cast<uint8_t>(adler >> 16));
    out.push_back(static_cast<uint8_t>(adler >> 8));
    out.push_back(static_cast<uint8_t>(adler));
    return out;
}

} // namespace png_detail

namespace {

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put_chunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& payload) {
    put_u32(out, static_cast<uint32_t>(payload.size()));
    const size_t type_pos = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    put_u32(out, png_detail::crc32(out.data() + type_pos, payload.size() + 4));
}

inline int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

// Filters one row with the type (None/Sub/Up/Average/Paeth) that minimizes the
// sum of absolute signed residuals, the usual libpng heuristic.
void filter_row(const uint8_t* row, const uint8_t* above, size_t stride, std::vector<uint8_t>& out,
                std::vector<uint8_t>& scratch) {
    constexpr size_t bpp = 4;
    scratch.resize(stride);
    long best_score = -1;
    uint8_t best_type = 0;
    std::vector<uint8_t> best;
    for (uint8_t type = 0; type < 5; ++type) {
        long score = 0;
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= bpp ? row[x - bpp] : 0;
            const int b = above ? above[x] : 0;
            const int c = (above && x >= bpp) ? above[x - bpp] : 0;
            int predicted = 0;
            switch (type) {
            case 1: predicted = a; break;
            case 2: predicted = b; break;
            case 3: predicted = (a + b) / 2; break;
            case 4: predicted = paeth(a, b, c); break;
            default: break;
            }
            const uint8_t v = static_cast<uint8_t>(row[x] - predicted);
            scratch[x] = v;
            score += v < 128 ? v : 256 - v;
        }
        if (best_score < 0 || score < best_score) {
            best_score = score;
            best_type = type;
            best.swap(scratch);
            scratch.resize(stride);
        }
    }
    out.push_back(best_type);
    out.insert(out.end(), best.begin(), best.end());
}

} // namespace

std::vector<uint8_t> encode_png(int width, int height, const uint8_t* rgba, bool flip_vertically) {
    if (width <= 0 || height <= 0 || !rgba) return {};
    if (static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > (uint64_t{1} << 28)) return {};

    const size_t stride = static_cast<size_t>(width) * 4;
    auto row_at = [&](int y) {
        const int src = flip_vertically ? height - 1 - y : y;
        return rgba + static_cast<size_t>(src) * stride;
    };

    std::vector<uint8_t> filtered;
    filtered.reserve((stride + 1) * static_cast<size_t>(height));
    std::vector<uint8_t> scratch;
    for (int y = 0; y < height; ++y)
        filter_row(row_at(y), y > 0 ? row_at(y - 1) : nullptr, stride, filtered, scratch);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

    std::vector<uint8_t> ihdr;
    put_u32(ihdr, static_cast<uint32_t>(width));
    put_u32(ihdr, static_cast<uint32_t>(height));
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // color type: RGBA
    ihdr.push_back(0);  // compression: deflate
    ihdr.push_back(0);  // filter method 0
    ihdr.push_back(0);  // no interlace
    put_chunk(png, "IHDR", ihdr);
    put_chunk(png, "IDAT", png_detail::zlib_compress(filtered.data(), filtered.size()));
    put_chunk(png, "IEND", {});
    return png;
}

bool write_png(const std::filesystem::path& path, int width, int height, const uint8_t* rgba,
               bool flip_vertically, std::string* error) {
    auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    const std::vector<uint8_t> png = encode_png(width, height, rgba, flip_vertically);
    if (png.empty())
        return fail("invalid image (" + std::to_string(width) + "x" + std::to_string(height) + ")");

    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return fail("cannot create " + path.parent_path().string() + ": " + ec.message());
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return fail("cannot open " + path.string() + " for writing");
    file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    file.close();
    if (!file) return fail("error writing " + path.string());
    return true;
}

} // namespace osef::editor
