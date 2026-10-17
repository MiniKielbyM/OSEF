// PNG decoder, resize and icon assets (util/png_reader.h, util/app_icon.h).

#include "test_main.h"

#include "util/app_icon.h"
#include "util/png_reader.h"
#include "util/png_writer.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace osef::editor;
namespace fs = std::filesystem;

namespace {
Image gradient(int w, int h) {
    Image img;
    img.width = w;
    img.height = h;
    img.rgba.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.rgba.data() + (size_t(y) * w + x) * 4;
            p[0] = uint8_t(x * 7);
            p[1] = uint8_t(y * 11);
            p[2] = uint8_t((x ^ y) * 3);
            p[3] = uint8_t(255 - (x + y) % 256);
        }
    return img;
}
std::vector<uint8_t> read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
} // namespace

TEST(png_decode_round_trip) {
    const Image src = gradient(37, 23);
    const std::vector<uint8_t> png = encode_png(src.width, src.height, src.rgba.data(), false);
    Image back;
    std::string error;
    REQUIRE(decode_png(png.data(), png.size(), back, &error));
    CHECK_EQ(back.width, 37);
    CHECK_EQ(back.height, 23);
    CHECK(back.rgba == src.rgba);
}

TEST(png_decode_rejects_garbage) {
    Image img;
    std::string error;
    const uint8_t junk[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(!decode_png(junk, sizeof junk, img, &error));
    CHECK(!error.empty());
    const Image src = gradient(8, 8);
    std::vector<uint8_t> png = encode_png(8, 8, src.rgba.data(), false);
    png.resize(png.size() / 2);  // truncated
    CHECK(!decode_png(png.data(), png.size(), img, &error));
}

// Real-world encoders use dynamic Huffman blocks and other color types: on
// macOS, re-encode with sips (RGB, dynamic Huffman) and decode that.
TEST(png_decode_sips_output) {
#if defined(__APPLE__)
    const fs::path dir = test::temp_dir("png_sips");
    Image src;  // smooth, so JPEG barely changes it
    src.width = 64;
    src.height = 48;
    src.rgba.resize(64 * 48 * 4);
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 64; ++x) {
            uint8_t* p = src.rgba.data() + (size_t(y) * 64 + x) * 4;
            p[0] = uint8_t(x * 4);
            p[1] = uint8_t(y * 5);
            p[2] = 128;
            p[3] = 255;
        }
    REQUIRE(write_png(dir / "in.png", 64, 48, src.rgba.data(), false, nullptr));
    const std::string cmd = "sips -s format jpeg '" + (dir / "in.png").string() + "' --out '" + (dir / "a.jpg").string() +
                            "' >/dev/null 2>&1 && sips -s format png '" + (dir / "a.jpg").string() + "' --out '" +
                            (dir / "out.png").string() + "' >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0) return;  // sips unavailable: nothing to check
    Image img;
    std::string error;
    REQUIRE(read_png(dir / "out.png", img, &error));
    CHECK_EQ(img.width, 64);
    CHECK_EQ(img.height, 48);
    // JPEG is lossy: colors must be close, alpha opaque.
    int max_diff = 0;
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
        CHECK_EQ(int(img.rgba[i + 3]), 255);
        for (int c = 0; c < 3; ++c) {
            const int d = std::abs(int(img.rgba[i + c]) - int(src.rgba[i + c]));
            if (d > max_diff) max_diff = d;
        }
    }
    CHECK(max_diff < 24);
#endif
}

TEST(png_resize_preserves_flat_color) {
    Image red;
    red.width = red.height = 100;
    red.rgba.assign(100 * 100 * 4, 0);
    for (size_t i = 0; i < red.rgba.size(); i += 4) red.rgba[i] = 200, red.rgba[i + 3] = 255;
    for (int s : {16, 33, 100, 256}) {
        const Image r = resize_image(red, s, s);
        CHECK_EQ(r.width, s);
        CHECK_EQ(int(r.rgba[0]), 200);
        CHECK_EQ(int(r.rgba[3]), 255);
        CHECK_EQ(int(r.rgba[r.rgba.size() - 4]), 200);
    }
}

TEST(app_icon_assets) {
    const fs::path dir = test::temp_dir("app_icon");
    const Image master = gradient(512, 512);
    std::string error;
    REQUIRE(write_ico(dir / "icon.ico", master, &error));
    const std::vector<uint8_t> ico = read_all(dir / "icon.ico");
    REQUIRE(ico.size() > 6 + 7 * 16);
    CHECK_EQ(int(ico[2]), 1);  // type icon
    CHECK_EQ(int(ico[4]), 7);  // 7 images
    CHECK_EQ(int(ico[6]), 16);
    CHECK_EQ(int(ico[6 + 6 * 16]), 0);  // 256 px is stored as 0
    REQUIRE(write_iconset(dir / "x.iconset", master, &error));
    CHECK(fs::exists(dir / "x.iconset" / "icon_512x512@2x.png"));
    Image big;
    REQUIRE(read_png(dir / "x.iconset" / "icon_512x512@2x.png", big, &error));
    CHECK_EQ(big.width, 1024);
    REQUIRE(write_icon_include(dir / "icon.inc", master, &error));
    const std::vector<uint8_t> inc = read_all(dir / "icon.inc");
    CHECK(std::string(inc.begin(), inc.end()).find("kAppIconPng[] = {") != std::string::npos);
}

#include "app/icon_data.inc"

// The committed runtime icon (generated from assets/icon.png) must decode,
// otherwise the app silently falls back to its drawn icon.
TEST(app_icon_embedded_png_decodes) {
    Image img;
    std::string error;
    REQUIRE(decode_png(kAppIconPng, sizeof(kAppIconPng), img, &error));
    CHECK_EQ(img.width, 256);
    CHECK_EQ(img.height, 256);
}
