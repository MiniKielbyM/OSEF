#include "util/png_reader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <utility>

namespace osef::editor {

namespace png_detail {
namespace {

struct BitReader {
    const uint8_t* p;
    size_t size, pos = 0;
    uint32_t bitbuf = 0;
    int bitcount = 0;
    bool overflow = false;
    uint32_t bits(int n) {
        while (bitcount < n) {
            if (pos >= size) {
                overflow = true;
                return 0;
            }
            bitbuf |= uint32_t(p[pos++]) << bitcount;
            bitcount += 8;
        }
        const uint32_t v = bitbuf & ((1u << n) - 1);
        bitbuf >>= n;
        bitcount -= n;
        return v;
    }
    void align() {
        bitbuf = 0;
        bitcount = 0;
    }
};

// Canonical Huffman decoding table (RFC 1951 3.2.2).
struct Huffman {
    uint16_t counts[16] = {};
    uint16_t symbols[320] = {};
    bool build(const uint8_t* lengths, int n) {
        std::memset(counts, 0, sizeof counts);
        for (int i = 0; i < n; ++i) counts[lengths[i]]++;
        counts[0] = 0;
        uint16_t offs[16] = {};
        for (int i = 1; i < 16; ++i) offs[i] = static_cast<uint16_t>(offs[i - 1] + counts[i - 1]);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbols[offs[lengths[i]]++] = static_cast<uint16_t>(i);
        return true;
    }
    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= static_cast<int>(br.bits(1));
            const int count = counts[len];
            if (code - count < first) return symbols[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
            if (br.overflow) return -1;
        }
        return -1;
    }
};

const uint16_t kLenBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                             35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                              257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflate_block(BitReader& br, const Huffman& lit, const Huffman& dist, std::vector<uint8_t>& out) {
    for (;;) {
        const int sym = lit.decode(br);
        if (sym < 0) return false;
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            const int li = sym - 257;
            if (li >= 29) return false;
            const size_t len = kLenBase[li] + br.bits(kLenExtra[li]);
            const int ds = dist.decode(br);
            if (ds < 0 || ds >= 30) return false;
            const size_t d = kDistBase[ds] + br.bits(kDistExtra[ds]);
            if (br.overflow || d > out.size()) return false;
            const size_t from = out.size() - d;
            for (size_t i = 0; i < len; ++i) out.push_back(out[from + i]);
        }
    }
}

} // namespace

bool zlib_inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    if (size < 6 || (data[0] & 0x0F) != 8 || ((data[0] << 8) | data[1]) % 31 != 0 || (data[1] & 0x20)) return false;
    BitReader br{data + 2, size - 6};
    for (bool last = false; !last;) {
        last = br.bits(1) != 0;
        const uint32_t type = br.bits(2);
        if (type == 0) {
            br.align();
            if (br.pos + 4 > br.size) return false;
            const uint16_t len = static_cast<uint16_t>(br.p[br.pos] | (br.p[br.pos + 1] << 8));
            br.pos += 4;
            if (br.pos + len > br.size) return false;
            out.insert(out.end(), br.p + br.pos, br.p + br.pos + len);
            br.pos += len;
        } else if (type == 1) {
            uint8_t lengths[320];
            int i = 0;
            for (; i < 144; ++i) lengths[i] = 8;
            for (; i < 256; ++i) lengths[i] = 9;
            for (; i < 280; ++i) lengths[i] = 7;
            for (; i < 288; ++i) lengths[i] = 8;
            Huffman lit, dist;
            lit.build(lengths, 288);
            for (i = 0; i < 30; ++i) lengths[i] = 5;
            dist.build(lengths, 30);
            if (!inflate_block(br, lit, dist, out)) return false;
        } else if (type == 2) {
            const int hlit = static_cast<int>(br.bits(5)) + 257, hdist = static_cast<int>(br.bits(5)) + 1;
            const int hclen = static_cast<int>(br.bits(4)) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19] = {};
            for (int i = 0; i < hclen; ++i) cl[order[i]] = static_cast<uint8_t>(br.bits(3));
            Huffman clh;
            clh.build(cl, 19);
            uint8_t lengths[320] = {};
            for (int n = 0; n < hlit + hdist;) {
                const int sym = clh.decode(br);
                if (sym < 0) return false;
                if (sym < 16) {
                    lengths[n++] = static_cast<uint8_t>(sym);
                } else {
                    int repeat = 0;
                    uint8_t value = 0;
                    if (sym == 16) {
                        if (n == 0) return false;
                        value = lengths[n - 1];
                        repeat = 3 + static_cast<int>(br.bits(2));
                    } else if (sym == 17) {
                        repeat = 3 + static_cast<int>(br.bits(3));
                    } else {
                        repeat = 11 + static_cast<int>(br.bits(7));
                    }
                    if (n + repeat > hlit + hdist) return false;
                    while (repeat--) lengths[n++] = value;
                }
            }
            Huffman lit, dist;
            lit.build(lengths, hlit);
            dist.build(lengths + hlit, hdist);
            if (!inflate_block(br, lit, dist, out)) return false;
        } else {
            return false;
        }
        if (br.overflow) return false;
    }
    return true;
}

} // namespace png_detail

namespace {
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
int paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
}
bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}
} // namespace

bool decode_png(const uint8_t* data, size_t size, Image& out, std::string* error) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (size < 8 || std::memcmp(data, sig, 8) != 0) return fail(error, "not a PNG file");
    uint32_t width = 0, height = 0;
    uint8_t depth = 0, color = 0, interlace = 0;
    std::vector<uint8_t> idat, palette, trns;
    for (size_t pos = 8; pos + 12 <= size;) {
        const uint32_t len = be32(data + pos);
        if (len > size - pos - 12) return fail(error, "truncated PNG chunk");
        const uint8_t* type = data + pos + 4;
        const uint8_t* body = data + pos + 8;
        if (!std::memcmp(type, "IHDR", 4) && len >= 13) {
            width = be32(body);
            height = be32(body + 4);
            depth = body[8];
            color = body[9];
            interlace = body[12];
        } else if (!std::memcmp(type, "PLTE", 4)) {
            palette.assign(body, body + len);
        } else if (!std::memcmp(type, "tRNS", 4)) {
            trns.assign(body, body + len);
        } else if (!std::memcmp(type, "IDAT", 4)) {
            idat.insert(idat.end(), body, body + len);
        } else if (!std::memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + len;
    }
    if (!width || !height || width > 8192 || height > 8192) return fail(error, "unsupported PNG size");
    if (depth != 8) return fail(error, "only 8-bit PNGs are supported (re-export the image as 8-bit)");
    if (interlace) return fail(error, "interlaced PNGs are not supported (re-export without interlacing)");
    int channels = 0;
    switch (color) {
    case 0: channels = 1; break;
    case 2: channels = 3; break;
    case 3: channels = 1; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    default: return fail(error, "unknown PNG color type");
    }
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (size_t(width) * channels + 1));
    if (!png_detail::zlib_inflate(idat.data(), idat.size(), raw)) return fail(error, "corrupt PNG image data");
    const size_t stride = size_t(width) * channels;
    if (raw.size() < size_t(height) * (stride + 1)) return fail(error, "PNG image data is too short");

    std::vector<uint8_t> prev(stride, 0), cur(stride);
    out.width = static_cast<int>(width);
    out.height = static_cast<int>(height);
    out.rgba.assign(size_t(width) * height * 4, 0);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* line = raw.data() + y * (stride + 1);
        const uint8_t filter = line[0];
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= size_t(channels) ? cur[x - channels] : 0, b = prev[x];
            const int c = x >= size_t(channels) ? prev[x - channels] : 0;
            int v = line[1 + x];
            switch (filter) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: v += paeth(a, b, c); break;
            default: return fail(error, "bad PNG filter");
            }
            cur[x] = static_cast<uint8_t>(v);
        }
        uint8_t* dst = out.rgba.data() + size_t(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x, dst += 4) {
            const uint8_t* s = cur.data() + size_t(x) * channels;
            switch (color) {
            case 0: dst[0] = dst[1] = dst[2] = s[0]; dst[3] = 255; break;
            case 4: dst[0] = dst[1] = dst[2] = s[0]; dst[3] = s[1]; break;
            case 2: dst[0] = s[0]; dst[1] = s[1]; dst[2] = s[2]; dst[3] = 255; break;
            case 6: std::memcpy(dst, s, 4); break;
            case 3: {
                const size_t i = s[0];
                if (i * 3 + 2 >= palette.size()) return fail(error, "PNG palette index out of range");
                dst[0] = palette[i * 3];
                dst[1] = palette[i * 3 + 1];
                dst[2] = palette[i * 3 + 2];
                dst[3] = i < trns.size() ? trns[i] : 255;
                break;
            }
            }
        }
        std::swap(prev, cur);
    }
    return true;
}

bool read_png(const std::filesystem::path& path, Image& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail(error, "cannot open the image file");
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return decode_png(data.data(), data.size(), out, error);
}

Image resize_image(const Image& src, int width, int height) {
    Image dst;
    dst.width = width;
    dst.height = height;
    dst.rgba.assign(size_t(width) * height * 4, 0);
    const double sx = double(src.width) / width, sy = double(src.height) / height;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Average the source area covered by this pixel (premultiplied alpha).
            const double x0 = x * sx, x1 = (x + 1) * sx, y0 = y * sy, y1 = (y + 1) * sy;
            double acc[4] = {}, total = 0;
            for (int yy = int(y0); yy < int(std::ceil(y1)) && yy < src.height; ++yy) {
                const double wy = std::min<double>(yy + 1, y1) - std::max<double>(yy, y0);
                for (int xx = int(x0); xx < int(std::ceil(x1)) && xx < src.width; ++xx) {
                    const double w = (std::min<double>(xx + 1, x1) - std::max<double>(xx, x0)) * wy;
                    if (w <= 0) continue;
                    const uint8_t* p = src.rgba.data() + (size_t(yy) * src.width + xx) * 4;
                    const double a = p[3] / 255.0;
                    acc[0] += p[0] * a * w;
                    acc[1] += p[1] * a * w;
                    acc[2] += p[2] * a * w;
                    acc[3] += a * w;
                    total += w;
                }
            }
            uint8_t* d = dst.rgba.data() + (size_t(y) * width + x) * 4;
            if (total <= 0 || acc[3] <= 0) continue;
            for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(std::lround(std::min(255.0, acc[c] / acc[3])));
            d[3] = static_cast<uint8_t>(std::lround(std::min(255.0, acc[3] / total * 255.0)));
        }
    }
    return dst;
}

} // namespace osef::editor
