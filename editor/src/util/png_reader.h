#pragma once
// Minimal, dependency-free PNG decoder (zlib inflate with stored, fixed and
// dynamic Huffman blocks) for the app icon. Supports non-interlaced 8-bit
// grayscale, gray+alpha, RGB, RGBA and palette images (with tRNS), i.e. what
// image editors and `sips` write. Always returns RGBA.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace osef::editor {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // width * height * 4, top row first
};

bool decode_png(const uint8_t* data, size_t size, Image& out, std::string* error);
bool read_png(const std::filesystem::path& path, Image& out, std::string* error);

// Area-averaging resize (good for downscaling icons; bilinear when upscaling).
Image resize_image(const Image& src, int width, int height);

namespace png_detail {
bool zlib_inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out);
} // namespace png_detail

} // namespace osef::editor
