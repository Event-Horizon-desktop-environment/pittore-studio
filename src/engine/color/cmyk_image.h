#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <vector>

#include "engine/core/image.h"

namespace pittore::color {

// Native, straight device inks in C, M, Y, K order. Alpha stays alongside
// the appearance pixels so transparent edges do not become ink-bearing.
struct CmykImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::array<std::uint16_t, 4>> pixels;

    // RGB appearance identity at the moment this device separation was made.
    // Alpha is separate, so alpha-only edits preserve the native inks.
    std::uint64_t appearanceFingerprint = 0;
    std::uint64_t profileFingerprint = 0;

    static std::uint64_t fingerprint(const std::uint8_t* data,
                                     std::size_t size) {
        std::uint64_t h = 14695981039346656037ull;
        for (std::size_t i = 0; i < size; ++i) {
            h ^= data[i];
            h *= 1099511628211ull;
        }
        return h ? h : 1;
    }

    bool valid() const {
        return width != 0 && height != 0 &&
               pixels.size() == static_cast<std::size_t>(width) * height;
    }

    static std::uint64_t fingerprint(const pittore::Image& image) {
        std::uint64_t h = 14695981039346656037ull;
        const pittore::RGBAf* p = image.data();
        for (std::size_t i = 0; i < image.pixel_count(); ++i) {
            for (int c = 0; c < 3; ++c) {
                const std::uint32_t word =
                    std::bit_cast<std::uint32_t>(p[i][c]);
                for (unsigned byte = 0; byte < 4; ++byte) {
                    h ^= (word >> (byte * 8)) & 0xffu;
                    h *= 1099511628211ull;
                }
            }
        }
        return h ? h : 1;
    }

    CmykImage rotate90(bool clockwise) const {
        CmykImage out;
        if (!valid()) return out;
        out.width = height;
        out.height = width;
        out.appearanceFingerprint = appearanceFingerprint;
        out.profileFingerprint = profileFingerprint;
        out.pixels.resize(pixels.size());
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint32_t dx = clockwise ? height - 1 - y : y;
                const std::uint32_t dy = clockwise ? x : width - 1 - x;
                out.pixels[static_cast<std::size_t>(dy) * out.width + dx] =
                    pixels[static_cast<std::size_t>(y) * width + x];
            }
        }
        return out;
    }

    CmykImage rotate180() const {
        CmykImage out;
        if (!valid()) return out;
        out.width = width;
        out.height = height;
        out.appearanceFingerprint = appearanceFingerprint;
        out.profileFingerprint = profileFingerprint;
        out.pixels.resize(pixels.size());
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x)
                out.pixels[static_cast<std::size_t>(height - 1 - y) * width +
                           (width - 1 - x)] =
                    pixels[static_cast<std::size_t>(y) * width + x];
        return out;
    }

    CmykImage flip(bool horizontal) const {
        CmykImage out;
        if (!valid()) return out;
        out.width = width;
        out.height = height;
        out.appearanceFingerprint = appearanceFingerprint;
        out.profileFingerprint = profileFingerprint;
        out.pixels.resize(pixels.size());
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint32_t dx = horizontal ? width - 1 - x : x;
                const std::uint32_t dy = horizontal ? y : height - 1 - y;
                out.pixels[static_cast<std::size_t>(dy) * width + dx] =
                    pixels[static_cast<std::size_t>(y) * width + x];
            }
        return out;
    }
};

}  // namespace pittore::color
