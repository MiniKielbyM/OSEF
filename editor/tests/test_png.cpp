// util/png_writer: chunk layout, CRCs, and a full decode (inflate + unfilter)
// checked against the source pixels.

#include "test_main.h"

#include "util/png_writer.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

using osef::editor::encode_png;
using osef::editor::write_png;
namespace png_detail = osef::editor::png_detail;

namespace {

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Independent bitwise CRC-32 so the encoder's table version is cross-checked.
uint32_t slow_crc32(const uint8_t* data, size_t size) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        c ^= data[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

struct Chunk {
    std::string type;
    std::vector<uint8_t> data;
    bool crc_ok = false;
};

std::vector<Chunk> parse_chunks(const std::vector<uint8_t>& png, bool* ok) {
    std::vector<Chunk> chunks;
    *ok = false;
    size_t pos = 8;
    while (pos + 12 <= png.size()) {
        const uint32_t len = be32(&png[pos]);
        if (pos + 12 + len > png.size()) return chunks;
        Chunk c;
        c.type.assign(reinterpret_cast<const char*>(&png[pos + 4]), 4);
        c.data.assign(png.begin() + long(pos + 8), png.begin() + long(pos + 8 + len));
        c.crc_ok = be32(&png[pos + 8 + len]) == slow_crc32(&png[pos + 4], len + 4);
        chunks.push_back(std::move(c));
        pos += 12 + len;
    }
    *ok = pos == png.size();
    return chunks;
}

// Minimal inflate for stored and fixed-Huffman blocks (what the writer emits).
class Inflater {
public:
    explicit Inflater(const std::vector<uint8_t>& in) : in_(in) {}

    std::optional<std::vector<uint8_t>> run() {
        std::vector<uint8_t> out;
        bool final = false;
        while (!final) {
            final = bits(1);
            const uint32_t type = bits(2);
            if (type == 0) {
                if (bit_) {  // stored blocks start on a byte boundary
                    bit_ = 0;
                    ++pos_;
                }
                if (pos_ + 4 > in_.size()) return std::nullopt;
                const uint32_t len = in_[pos_] | (in_[pos_ + 1] << 8);
                pos_ += 4;
                if (pos_ + len > in_.size()) return std::nullopt;
                out.insert(out.end(), in_.begin() + long(pos_), in_.begin() + long(pos_ + len));
                pos_ += len;
            } else if (type == 1) {
                for (;;) {
                    const int sym = fixed_litlen();
                    if (sym < 0 || failed_) return std::nullopt;
                    if (sym < 256) {
                        out.push_back(uint8_t(sym));
                    } else if (sym == 256) {
                        break;
                    } else {
                        static const int lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10,  11,  13,
                                                      15, 17, 19, 23, 27, 31, 35, 43,  51,  59,
                                                      67, 83, 99, 115, 131, 163, 195, 227, 258};
                        static const int lextra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
                        static const int dbase[30] = {1,    2,    3,    4,    5,    7,    9,    13,
                                                      17,   25,   33,   49,   65,   97,   129,  193,
                                                      257,  385,  513,  769,  1025, 1537, 2049, 3073,
                                                      4097, 6145, 8193, 12289, 16385, 24577};
                        static const int dextra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
                        const int li = sym - 257;
                        if (li >= 29) return std::nullopt;
                        const int len = lbase[li] + int(bits(lextra[li]));
                        const int di = int(rev_bits(5));
                        if (di >= 30) return std::nullopt;
                        const size_t dist = size_t(dbase[di]) + bits(dextra[di]);
                        if (dist > out.size()) return std::nullopt;
                        for (int k = 0; k < len; ++k) out.push_back(out[out.size() - dist]);
                    }
                }
            } else {
                return std::nullopt;  // dynamic Huffman: the writer never emits it
            }
            if (failed_) return std::nullopt;
        }
        consumed_ = pos_ + (bit_ ? 1 : 0);
        return out;
    }
    size_t consumed() const { return consumed_; }

private:
    uint32_t bits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            if (pos_ >= in_.size()) {
                failed_ = true;
                return 0;
            }
            v |= uint32_t((in_[pos_] >> bit_) & 1) << i;
            if (++bit_ == 8) {
                bit_ = 0;
                ++pos_;
            }
        }
        return v;
    }
    uint32_t rev_bits(int n) {  // Huffman codes arrive MSB-first
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | bits(1);
        return v;
    }
    int fixed_litlen() {
        uint32_t code = rev_bits(7);
        if (code <= 0x17) return int(code) + 256;
        code = (code << 1) | bits(1);
        if (code >= 0x30 && code <= 0xBF) return int(code - 0x30);
        if (code >= 0xC0 && code <= 0xC7) return int(code - 0xC0) + 280;
        code = (code << 1) | bits(1);
        if (code >= 0x190 && code <= 0x1FF) return int(code - 0x190) + 144;
        return -1;
    }

    const std::vector<uint8_t>& in_;
    size_t pos_ = 0;
    int bit_ = 0;
    bool failed_ = false;
    size_t consumed_ = 0;
};

std::optional<std::vector<uint8_t>> zlib_decompress(const std::vector<uint8_t>& z) {
    if (z.size() < 6) return std::nullopt;
    if ((z[0] & 0x0F) != 8 || ((z[0] << 8) | z[1]) % 31 != 0) return std::nullopt;
    std::vector<uint8_t> body(z.begin() + 2, z.end());
    Inflater inf(body);
    auto out = inf.run();
    if (!out) return std::nullopt;
    if (inf.consumed() + 4 != body.size()) return std::nullopt;
    if (be32(&body[inf.consumed()]) != png_detail::adler32(out->data(), out->size())) return std::nullopt;
    return out;
}

int paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

std::optional<std::vector<uint8_t>> unfilter(const std::vector<uint8_t>& raw, int w, int h) {
    const size_t stride = size_t(w) * 4;
    if (raw.size() != (stride + 1) * size_t(h)) return std::nullopt;
    std::vector<uint8_t> px(stride * size_t(h));
    for (int y = 0; y < h; ++y) {
        const uint8_t type = raw[size_t(y) * (stride + 1)];
        const uint8_t* src = &raw[size_t(y) * (stride + 1) + 1];
        uint8_t* row = &px[size_t(y) * stride];
        const uint8_t* up = y > 0 ? row - stride : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= 4 ? row[x - 4] : 0, b = up ? up[x] : 0, c = (up && x >= 4) ? up[x - 4] : 0;
            int pred = 0;
            switch (type) {
            case 0: pred = 0; break;
            case 1: pred = a; break;
            case 2: pred = b; break;
            case 3: pred = (a + b) / 2; break;
            case 4: pred = paeth(a, b, c); break;
            default: return std::nullopt;
            }
            row[x] = uint8_t(src[x] + pred);
        }
    }
    return px;
}

std::vector<uint8_t> test_image(int w, int h) {
    std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[(size_t(y) * size_t(w) + size_t(x)) * 4];
            p[0] = uint8_t(x * 7 + y);           // gradient
            p[1] = uint8_t((x / 4 + y / 4) & 1 ? 0x38 : 0xD2);  // checker, like UI flats
            p[2] = uint8_t((x * 31 + y * 17) ^ (x * y));        // noise-ish
            p[3] = uint8_t(255 - y);
        }
    return px;
}

} // namespace

TEST(png_crc32_and_adler32_known_values) {
    const char* s = "123456789";
    const auto* p = reinterpret_cast<const uint8_t*>(s);
    CHECK_EQ(png_detail::crc32(p, 9), 0xCBF43926u);
    CHECK_EQ(png_detail::adler32(p, 9), 0x091E01DEu);
    CHECK_EQ(png_detail::crc32(p, 0), 0u);
    CHECK_EQ(png_detail::adler32(p, 0), 1u);
}

TEST(png_zlib_round_trip) {
    std::vector<uint8_t> data;
    for (int i = 0; i < 100000; ++i) data.push_back(uint8_t(i % 251 < 120 ? 0 : (i * 13) >> 3));
    const auto z = png_detail::zlib_compress(data.data(), data.size());
    CHECK(z.size() < data.size());
    const auto back = zlib_decompress(z);
    REQUIRE(back.has_value());
    CHECK(*back == data);

    const auto empty = zlib_decompress(png_detail::zlib_compress(nullptr, 0));
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST(png_signature_ihdr_crcs_and_pixels) {
    const int w = 37, h = 23;
    const auto px = test_image(w, h);
    const auto png = encode_png(w, h, px.data(), false);
    REQUIRE(png.size() > 8 + 25 + 12 + 12);

    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    CHECK(std::equal(sig, sig + 8, png.begin()));

    bool ok = false;
    const auto chunks = parse_chunks(png, &ok);
    CHECK(ok);
    REQUIRE_EQ(chunks.size(), 3);
    CHECK_EQ(chunks[0].type, "IHDR");
    CHECK_EQ(chunks[1].type, "IDAT");
    CHECK_EQ(chunks[2].type, "IEND");
    for (const Chunk& c : chunks) CHECK(c.crc_ok);
    CHECK(chunks[2].data.empty());

    const auto& ihdr = chunks[0].data;
    REQUIRE_EQ(ihdr.size(), 13);
    CHECK_EQ(be32(&ihdr[0]), uint32_t(w));
    CHECK_EQ(be32(&ihdr[4]), uint32_t(h));
    CHECK_EQ(int(ihdr[8]), 8);   // bit depth
    CHECK_EQ(int(ihdr[9]), 6);   // RGBA
    CHECK_EQ(int(ihdr[10]), 0);
    CHECK_EQ(int(ihdr[11]), 0);
    CHECK_EQ(int(ihdr[12]), 0);

    const auto raw = zlib_decompress(chunks[1].data);
    REQUIRE(raw.has_value());
    const auto decoded = unfilter(*raw, w, h);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == px);
}

TEST(png_flip_vertically) {
    const int w = 5, h = 4;
    const auto px = test_image(w, h);
    std::vector<uint8_t> flipped(px.size());
    const size_t stride = size_t(w) * 4;
    for (int y = 0; y < h; ++y)
        std::copy_n(&px[size_t(h - 1 - y) * stride], stride, &flipped[size_t(y) * stride]);
    CHECK(encode_png(w, h, flipped.data(), true) == encode_png(w, h, px.data(), false));
}

TEST(png_write_file_and_reject_bad_input) {
    const auto dir = osef::editor::test::temp_dir("png");
    const auto path = dir / "sub" / "out.png";
    const auto px = test_image(16, 9);
    std::string error;
    REQUIRE(write_png(path, 16, 9, px.data(), true, &error));
    CHECK(error.empty());

    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(bytes == encode_png(16, 9, px.data(), true));

    CHECK(!write_png(dir / "bad.png", 0, 9, px.data(), false, &error));
    CHECK(!error.empty());
    CHECK(!write_png(dir / "bad.png", 4, 4, nullptr, false, nullptr));
    CHECK(!std::filesystem::exists(dir / "bad.png"));
}
