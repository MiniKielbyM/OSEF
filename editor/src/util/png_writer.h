#pragma once
// Minimal, dependency-free PNG encoder (8-bit RGBA, no interlacing) used for
// --screenshot. Pixel data is deflated with a small LZ77 + fixed-Huffman
// compressor, which is plenty for flat UI screenshots.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace osef::editor {

// Writes `width` x `height` RGBA pixels (row-major, 4 bytes per pixel, top row
// first unless `flip_vertically`, which is what glReadPixels returns) to
// `path`. Returns false and sets *error (if non-null) on failure.
bool write_png(const std::filesystem::path& path, int width, int height,
               const uint8_t* rgba, bool flip_vertically, std::string* error);

// Same, but encodes into memory. Returns an empty vector on invalid input.
std::vector<uint8_t> encode_png(int width, int height, const uint8_t* rgba,
                                bool flip_vertically);

namespace png_detail {
uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0);
uint32_t adler32(const uint8_t* data, size_t size, uint32_t adler = 1);
// zlib stream (RFC 1950) wrapping one fixed-Huffman deflate block (RFC 1951).
std::vector<uint8_t> zlib_compress(const uint8_t* data, size_t size);
} // namespace png_detail

} // namespace osef::editor
