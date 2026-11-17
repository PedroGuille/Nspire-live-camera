#ifndef NSPIRE_CAMERA_CODEC_H
#define NSPIRE_CAMERA_CODEC_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace nc2 {
constexpr unsigned MaxSide = 102;
constexpr size_t MaxPixels = MaxSide * MaxSide;
constexpr size_t ChunkChars = 192; // divisible by 4, one outstanding packet

inline uint32_t adler32(const uint8_t* data, size_t size) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

// PackBits: 0..127 => 1..128 literals; 129..255 => 128..2 repeats.
// 128 is reserved and never emitted. Worst case: n + ceil(n/128).
inline std::vector<uint8_t> pack(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> out;
    out.reserve(input.size() + (input.size() + 127) / 128);
    size_t i = 0;
    while (i < input.size()) {
        size_t run = 1;
        while (run < 128 && i + run < input.size() && input[i + run] == input[i]) ++run;
        if (run >= 3) {
            out.push_back(static_cast<uint8_t>(257 - run));
            out.push_back(input[i]);
            i += run;
        } else {
            const size_t start = i;
            do {
                ++i;
                run = 1;
                while (run < 3 && i + run < input.size() && input[i + run] == input[i]) ++run;
            } while (i < input.size() && i - start < 128 && run < 3);
            out.push_back(static_cast<uint8_t>(i - start - 1));
            out.insert(out.end(), input.begin() + start, input.begin() + i);
        }
    }
    return out;
}

inline std::string base64(const std::vector<uint8_t>& input) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(4 * ((input.size() + 2) / 3));
    for (size_t i = 0; i < input.size(); i += 3) {
        uint32_t v = static_cast<uint32_t>(input[i]) << 16;
        if (i + 1 < input.size()) v |= static_cast<uint32_t>(input[i + 1]) << 8;
        if (i + 2 < input.size()) v |= input[i + 2];
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += i + 1 < input.size() ? alphabet[(v >> 6) & 63] : '=';
        out += i + 2 < input.size() ? alphabet[v & 63] : '=';
    }
    return out;
}

// Local palette + bit-packed indices handles low-colour detail that RLE cannot
// compress (e.g. text/checkerboards), without a per-frame Huffman tree.
inline std::vector<uint8_t> palettePack(const std::vector<uint8_t>& pixels) {
    int indices[256];
    std::fill(indices, indices + 256, -1);
    std::vector<uint8_t> palette;
    for (auto pixel : pixels) {
        if (indices[pixel] < 0) {
            indices[pixel] = static_cast<int>(palette.size());
            palette.push_back(pixel);
        }
    }
    if (palette.empty()) return {};
    unsigned bits = 1;
    while ((1u << bits) < palette.size()) ++bits;
    std::vector<uint8_t> out;
    out.reserve(1 + palette.size() + (pixels.size() * bits + 7) / 8);
    out.push_back(static_cast<uint8_t>(palette.size() - 1));
    out.insert(out.end(), palette.begin(), palette.end());
    unsigned buffer = 0, count = 0;
    for (auto pixel : pixels) {
        buffer = (buffer << bits) | static_cast<unsigned>(indices[pixel]);
        count += bits;
        if (count >= 8) {
            count -= 8;
            out.push_back(static_cast<uint8_t>(buffer >> count));
            buffer &= (1u << count) - 1;
        }
    }
    if (count) out.push_back(static_cast<uint8_t>(buffer << (8 - count)));
    return out;
}

struct Encoded {
    char mode;
    uint32_t checksum;
    std::string payload;
};

inline Encoded encode(const std::vector<uint8_t>& pixels,
                      const std::vector<uint8_t>& previous, bool allowDelta) {
    std::vector<uint8_t> best = pixels;
    char mode = 'K';
    auto packed = pack(pixels);
    if (packed.size() < best.size()) {
        best.swap(packed);
        mode = 'R';
    }
    packed = palettePack(pixels);
    if (!packed.empty() && packed.size() < best.size()) {
        best.swap(packed);
        mode = 'P';
    }
    if (allowDelta && previous.size() == pixels.size()) {
        std::vector<uint8_t> delta(pixels.size());
        for (size_t i = 0; i < pixels.size(); ++i) delta[i] = pixels[i] ^ previous[i];
        packed = pack(delta);
        if (packed.size() < best.size()) {
            best.swap(packed);
            mode = 'D';
        }
    }
    // Check decoded pixels, protecting the delta reference as well as the wire data.
    return {mode, adler32(pixels.data(), pixels.size()), base64(best)};
}

inline uint8_t rgb332(unsigned r, unsigned g, unsigned b) {
    return static_cast<uint8_t>((((r * 7 + 127) / 255) << 5) |
                               (((g * 7 + 127) / 255) << 2) |
                               ((b * 3 + 127) / 255));
}

// ESP32 camera RGB565 is high byte first. Exact box boundaries cover the entire
// centre crop, including right/bottom remainders at non-divisible resolutions.
inline uint8_t quantize(unsigned r, unsigned g, unsigned b, unsigned colours) {
    if (colours == 16) return static_cast<uint8_t>((((77*r + 150*g + 29*b + 128) >> 8)*15 + 127)/255);
    if (colours == 64) {
        r = ((r*3+127)/255)*85; g = ((g*3+127)/255)*85; b = ((b*3+127)/255)*85;
    }
    return rgb332(r,g,b);
}
inline uint8_t filterPixel(unsigned r, unsigned g, unsigned b, unsigned colours,
                           uint8_t old, unsigned noise) {
    if (noise) {
        int pr,pg,pb,hr,hg,hb;
        if (colours == 16) {
            pr=pg=pb=old*17; hr=hg=hb=9;
            const int luma=(77*r+150*g+29*b+128)>>8;
            if (std::abs(luma-pr)<=int(noise)+hr) return old;
        } else {
            pr=((old>>5)*255+3)/7; pg=(((old>>2)&7)*255+3)/7; pb=(old&3)*85;
            if (colours==64) { pr=(((old>>5)*3+3)/7)*85; pg=(( ((old>>2)&7)*3+3)/7)*85; }
            hr=hg=colours==64?43:18; hb=43;
            if (std::abs(int(r)-pr)<=int(noise)+hr && std::abs(int(g)-pg)<=int(noise)+hg &&
                std::abs(int(b)-pb)<=int(noise)+hb) return old;
        }
    }
    return quantize(r,g,b,colours);
}

inline bool downsample(const uint8_t* src, size_t length, unsigned width,
                       unsigned height, unsigned side, std::vector<uint8_t>& out,
                       unsigned colours = 256, const std::vector<uint8_t>* reference = nullptr, unsigned noise = 0) {
    const unsigned crop = std::min(width, height);
    if (!src || !width || !height || !side || side > MaxSide || side > crop ||
        width > 4096 || height > 4096 || length < size_t(width) * height * 2) return false;
    out.resize(size_t(side) * side);
    const unsigned x0 = (width - crop) / 2, y0 = (height - crop) / 2;
    for (unsigned y = 0; y < side; ++y) {
        const unsigned top = y0 + y * crop / side, bottom = y0 + (y + 1) * crop / side;
        for (unsigned x = 0; x < side; ++x) {
            const unsigned left = x0 + x * crop / side, right = x0 + (x + 1) * crop / side;
            uint32_t r = 0, g = 0, b = 0;
            for (unsigned sy = top; sy < bottom; ++sy) {
                for (unsigned sx = left; sx < right; ++sx) {
                    const size_t p = (size_t(sy) * width + sx) * 2;
                    const uint16_t v = (uint16_t(src[p]) << 8) | src[p + 1];
                    const unsigned r5 = v >> 11, g6 = (v >> 5) & 63, b5 = v & 31;
                    r += (r5 << 3) | (r5 >> 2);
                    g += (g6 << 2) | (g6 >> 4);
                    b += (b5 << 3) | (b5 >> 2);
                }
            }
            const unsigned n = (bottom - top) * (right - left);
            const unsigned rr=(r+n/2)/n, gg=(g+n/2)/n, bb=(b+n/2)/n;
            const size_t pos=y*side+x;
            out[pos] = reference && reference->size()==out.size() ?
                filterPixel(rr,gg,bb,colours,(*reference)[pos],noise) : quantize(rr,gg,bb,colours);
        }
    }
    return true;
}
} // namespace nc2
#endif
