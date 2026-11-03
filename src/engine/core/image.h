#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "engine/core/pixel.h"

namespace pittore {

// Host RGBAf image, row-major. CPU side of the tile cache;
// GPU backends mirror it in device memory.
class Image {
  public:
    Image(std::uint32_t width, std::uint32_t height)
        : w_(width), h_(height), serial_(nextSerial()),
          data_(static_cast<std::size_t>(width) * height) {
        if (width == 0 || height == 0)
            throw std::invalid_argument("Image dimensions cannot be zero");
    }

    // Unique per allocation and never reused: a caller can key a cache on it
    // for the image's whole lifetime. The data address cannot serve that role
    // — a freed image's heap block can be handed to the next allocation of the
    // same size, an ABA swap that stamps and byte counts cannot detect.
    std::uint64_t serial() const { return serial_; }

    std::uint32_t width() const { return w_; }
    std::uint32_t height() const { return h_; }
    std::size_t pixel_count() const { return data_.size(); }

    RGBAf& at(std::uint32_t x, std::uint32_t y) {
        return data_[static_cast<std::size_t>(y) * w_ + x];
    }
    const RGBAf& at(std::uint32_t x, std::uint32_t y) const {
        return data_[static_cast<std::size_t>(y) * w_ + x];
    }

    RGBAf* data() { return data_.data(); }
    const RGBAf* data() const { return data_.data(); }

    void fill(RGBAf v) { std::fill(data_.begin(), data_.end(), v); }

    // Deep copy. Undo shares Images via shared_ptr, so clone before
    // mutating (copy-on-write) or old snapshots see the edit.
    Image clone() const {
        Image c(w_, h_);
        c.data_ = data_;
        return c;
    }

    // Copies are distinct allocations, so they take their own serial.
    Image(const Image& o)
        : w_(o.w_), h_(o.h_), serial_(nextSerial()), data_(o.data_) {}
    Image& operator=(const Image& o) {
        if (this != &o) {
            w_ = o.w_;
            h_ = o.h_;
            serial_ = nextSerial();  // assigned bytes re-enter as an identity
            data_ = o.data_;
        }
        return *this;
    }
    // A move hands the whole identity (bytes and serial) to the target.
    Image(Image&&) = default;
    Image& operator=(Image&&) = default;

  private:
    static std::uint64_t nextSerial() {
        static std::atomic<std::uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint32_t w_;
    std::uint32_t h_;
    std::uint64_t serial_;
    std::vector<RGBAf> data_;
};

}  // namespace pittore