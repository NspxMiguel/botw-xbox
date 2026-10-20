// SPDX-License-Identifier: MIT
#include "../art.h"
#include <cstdlib>
#include <iostream>

#define CHECK(...)                                                                                           \
    do {                                                                                                     \
        if (!(__VA_ARGS__)) {                                                                                \
            std::cerr << "Failed at line " << __LINE__ << ": " #__VA_ARGS__ "\n";                            \
            std::exit(1);                                                                                    \
        }                                                                                                    \
    } while (false)
using namespace uwp;
std::vector<uint8_t> header(int width, int height, int bits = 24, int type = 2, int origin = 32) {
    std::vector<uint8_t> bytes(18);
    bytes[2] = uint8_t(type);
    bytes[12] = uint8_t(width);
    bytes[13] = uint8_t(width >> 8);
    bytes[14] = uint8_t(height);
    bytes[15] = uint8_t(height >> 8);
    bytes[16] = uint8_t(bits);
    bytes[17] = uint8_t(origin);
    return bytes;
}
int main() {
    // Four distinct corners, all four TGA storage origins, with and without alpha.
    for (int bits : {24, 32})
        for (int origin : {0, 16, 32, 48}) {
            auto bytes = header(2, 2, bits, 2, origin | (bits == 32 ? 8 : 0));
            bytes[0] = 3;
            bytes.insert(bytes.end(), {1, 2, 3}); // Skip the image ID.
            for (int i = 0; i < 4; ++i) {
                bytes.insert(bytes.end(), {uint8_t(i + 1), uint8_t(i + 11), uint8_t(i + 21)});
                if (bits == 32)
                    bytes.push_back(uint8_t(i + 101));
            }
            auto image = decode_tga(bytes);
            CHECK(image && image->width == 2 && image->height == 2);
            for (int i = 0; i < 4; ++i) {
                int x = i % 2, y = i / 2;
                if (origin & 16)
                    x = 1 - x;
                if (!(origin & 32))
                    y = 1 - y;
                size_t at = size_t(y * 2 + x) * 4;
                CHECK(image->rgba[at] == i + 21 && image->rgba[at + 1] == i + 11 &&
                      image->rgba[at + 2] == i + 1);
                CHECK(image->rgba[at + 3] == (bits == 32 ? i + 101 : 255));
            }
            // Every truncated prefix must fail without reading out of bounds.
            for (size_t size = 0; size < bytes.size(); ++size)
                CHECK(!decode_tga(std::span(bytes).first(size)));
        }
    for (int bits : {24, 32}) {
        auto bytes = header(3, 2, bits, 10, 32 | (bits == 32 ? 8 : 0));
        bytes.push_back(0x83); // Run crosses a row boundary.
        bytes.insert(bytes.end(), {3, 2, 1});
        if (bits == 32)
            bytes.push_back(100);
        bytes.push_back(1); // Two raw pixels.
        for (int i = 0; i < 2; ++i) {
            bytes.insert(bytes.end(), {6, 5, 4});
            if (bits == 32)
                bytes.push_back(200);
        }
        auto image = decode_tga(bytes);
        CHECK(image && image->rgba[0] == 1 && image->rgba[16] == 4);
        CHECK(image->rgba[3] == (bits == 32 ? 100 : 255));
        CHECK(image->rgba[19] == (bits == 32 ? 200 : 255));
        for (size_t size = 0; size < bytes.size(); ++size)
            CHECK(!decode_tga(std::span(bytes).first(size)));
    }
    auto bytes = header(1, 1, 32);
    bytes.insert(bytes.end(), {3, 2, 1, 0});
    CHECK(decode_tga(bytes)->rgba[3] == 255); // Unused attribute byte.
    auto invalid = bytes;
    invalid[1] = 1;
    CHECK(!decode_tga(invalid));
    invalid = bytes;
    invalid[2] = 1;
    CHECK(!decode_tga(invalid));
    invalid = bytes;
    invalid[16] = 16;
    CHECK(!decode_tga(invalid));
    invalid = bytes;
    invalid[17] = 64;
    CHECK(!decode_tga(invalid));
    invalid = bytes;
    invalid[17] = 9;
    CHECK(!decode_tga(invalid));
    invalid = bytes;
    invalid[12] = 0;
    CHECK(!decode_tga(invalid));
    invalid = header(65535, 65535);
    CHECK(!decode_tga(invalid));
    invalid = header(1, 1, 24, 10);
    invalid.insert(invalid.end(), {0x81, 1, 2, 3});
    CHECK(!decode_tga(invalid));
    invalid = header(1, 1, 24, 10);
    invalid.insert(invalid.end(), {1, 1, 2, 3, 1, 2, 3});
    CHECK(!decode_tga(invalid));
    // Maximum-size RLE packets, a raw packet followed by a repeated packet, and trailing TGA footer.
    auto packets = header(256, 1, 24, 10);
    packets.push_back(127);
    for (int i = 0; i < 128; ++i)
        packets.insert(packets.end(), {0, 0, uint8_t(i)});
    packets.insert(packets.end(), {255, 9, 8, 7});
    packets.resize(packets.size() + 26, 0);
    auto decoded_packets = decode_tga(packets);
    CHECK(decoded_packets && decoded_packets->rgba[127 * 4] == 127 && decoded_packets->rgba[255 * 4] == 7);
    // Full-size synthetic TV art, not game data.
    auto tv = header(1280, 720);
    tv.resize(18 + 1280 * 720 * 3, 123);
    CHECK(decode_tga(tv)->rgba.size() == 1280 * 720 * 4);
    auto icon = header(128, 128);
    icon.resize(18 + 128 * 128 * 3, 123);
    CHECK(decode_tga(icon));
    // Neutral pixels cannot win against the dominant saturated hue.
    ArtImage art{100, 1, {}};
    for (int i = 0; i < 90; ++i)
        art.rgba.insert(art.rgba.end(), {200, 200, 200, 255});
    for (int i = 0; i < 8; ++i)
        art.rgba.insert(art.rgba.end(), {0, 100, 0, 255});
    for (int i = 0; i < 2; ++i)
        art.rgba.insert(art.rgba.end(), {100, 0, 0, 255});
    auto color = art_accent(art);
    CHECK(color.g > color.r && color.g > color.b && black_contrast(color) >= 4.5);
    for (int r = 0; r <= 255; r += 17)
        for (int g = 0; g <= 255; g += 17)
            for (int b = 0; b <= 255; b += 17) {
                ArtImage sample{1, 1, {uint8_t(r), uint8_t(g), uint8_t(b), 255}};
                CHECK(black_contrast(art_accent(sample)) >= 4.5);
            }
    CHECK(black_contrast(art_accent({})) == 21);
    CHECK(black_contrast(art_accent({1, 1, {255, 0, 0, 0}})) == 21);
    CHECK(disc_angle(3, false, false) == 0 && disc_angle(3, true, true) == 0);
    CHECK(disc_angle(0, true, false) == 0 && disc_angle(12, true, false) == 0);
    CHECK(disc_angle(3, true, false) < 90 && disc_angle(6, true, false) == 180);
    std::cout << "TGA, accent and motion tests passed\n";
}
