#include "../camera_codec.h"
#include <cassert>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>

static std::vector<uint8_t> unpack(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < data.size();) {
        unsigned t = data[i++];
        assert(t != 128);
        if (t < 128) {
            assert(i + t + 1 <= data.size());
            out.insert(out.end(), data.begin() + i, data.begin() + i + t + 1);
            i += t + 1;
        } else {
            assert(i < data.size());
            out.insert(out.end(), 257 - t, data[i++]);
        }
    }
    return out;
}

static std::string hex(const std::vector<uint8_t>& v) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    for (auto b : v) { s += digits[b >> 4]; s += digits[b & 15]; }
    return s;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    std::mt19937 random(1701);
    assert(nc2::base64({0, 1, 2, 253, 254, 255}) == "AAEC/f7/");
    assert(nc2::base64({0}) == "AA==");
    assert(nc2::base64({0, 1}) == "AAE=");
    assert(nc2::adler32(reinterpret_cast<const uint8_t*>("Wikipedia"), 9) == 0x11E60398);
    for (size_t n : {0, 1, 2, 3, 127, 128, 129, 2601, 5184, 10404}) {
        for (unsigned pattern = 0; pattern < 5; ++pattern) {
            std::vector<uint8_t> v(n);
            for (size_t i = 0; i < n; ++i) {
                v[i] = pattern == 0 ? 42 : pattern == 1 ? i % 256 : pattern == 2 ? random() % 256 :
                       pattern == 3 ? (i / 127) % 256 : random() % 4;
            }
            auto packed = nc2::pack(v);
            assert(packed.size() <= n + (n + 127) / 128);
            assert(unpack(packed) == v);
        }
    }
    std::vector<uint8_t> out;
    // Exact byte order, white expansion, primary colours and complete crop.
    for (auto v : {0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F}) {
        std::vector<uint8_t> src(5 * 3 * 2);
        for (size_t i = 0; i < src.size(); i += 2) { src[i] = v >> 8; src[i+1] = v; }
        assert(nc2::downsample(src.data(), src.size(), 5, 3, 2, out));
        uint8_t expected = v == 0 ? 0 : v == 0xFFFF ? 255 : v == 0xF800 ? 224 : v == 0x07E0 ? 28 : 3;
        assert(out == std::vector<uint8_t>(4, expected));
    }
    std::vector<uint8_t> edge(5 * 5 * 2, 0);
    for (size_t i = 0; i < 5; ++i) {
        edge[(4 * 5 + i) * 2] = edge[(4 * 5 + i) * 2 + 1] = 255;
        edge[(i * 5 + 4) * 2] = edge[(i * 5 + 4) * 2 + 1] = 255;
    }
    assert(nc2::downsample(edge.data(), edge.size(), 5, 5, 2, out));
    assert(out[0] == 0 && out[1] > 0 && out[2] > 0 && out[3] > out[1]);
    assert(!nc2::downsample(edge.data(), 3, 5, 5, 2, out));
    assert(!nc2::downsample(edge.data(), edge.size(), 5, 5, 6, out));
    assert(!nc2::downsample(nullptr, 0, 0, 0, 0, out));

    std::ofstream fixture(argv[1]);
    uint32_t seq = 0;
    for (unsigned side : {1, 2, 51, 72, 102}) {
        std::vector<uint8_t> previous;
        uint32_t base = 0;
        for (unsigned pattern = 0; pattern < 8; ++pattern) {
            std::vector<uint8_t> pixels(side * side);
            for (size_t i = 0; i < pixels.size(); ++i) {
                if (pattern == 0) pixels[i] = 0;
                else if (pattern == 1) pixels[i] = i % 256;
                else if (pattern == 2) pixels[i] = random() % 256;
                else if (pattern == 3 || pattern == 4) pixels[i] = previous[i];
                else if (pattern == 5) pixels[i] = i == pixels.size()/2 ? previous[i] ^ 255 : previous[i];
                else if (pattern == 6) pixels[i] = 255;
                else pixels[i] = random() % 4;
            }
            auto enc = nc2::encode(pixels, previous, true);
            assert(enc.payload.size() <= 4 * ((pixels.size() + 2) / 3));
            fixture << ++seq << '\t' << (enc.mode == 'D' ? base : 0) << '\t' << side << '\t'
                    << enc.mode << '\t' << enc.checksum << '\t' << enc.payload << '\t' << hex(pixels) << '\n';
            if (side == 72) std::cout << "72x72 pattern " << pattern << ": " << enc.mode << " " << enc.payload.size() << " payload bytes\n";
            previous = pixels; base = seq;
        }
    }
    std::cout << "C++ codec tests passed; wrote 40 cross-language frames\n";
}
