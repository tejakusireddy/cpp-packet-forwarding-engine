#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace forwarding {

// Single-owner pool. A dispatcher can acquire buffers, send them through workers,
// then recycle returned completions on the same thread. No internal locks.
class PacketPool {
public:
    PacketPool(std::size_t count, std::size_t frame_capacity)
        : count_(count), frame_capacity_(frame_capacity) {
        if (count == 0 || frame_capacity < 14) throw std::invalid_argument("Invalid packet pool size");
        free_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            free_.emplace_back(frame_capacity);
        }
    }
    PacketPool(const PacketPool&) = delete;
    PacketPool& operator=(const PacketPool&) = delete;
    PacketPool(PacketPool&&) = delete;
    PacketPool& operator=(PacketPool&&) = delete;

    [[nodiscard]] std::size_t available() const noexcept { return free_.size(); }
    [[nodiscard]] std::size_t frame_capacity() const noexcept { return frame_capacity_; }
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> acquire(std::size_t size) {
        if (size > frame_capacity_ || free_.empty()) return std::nullopt;
        auto result = std::move(free_.back());
        free_.pop_back();
        result.resize(size); // Capacity is already reserved; newly exposed bytes are zeroed.
        return result;
    }
    // Accepts any compatible vector, including returned worker buffers. Failure
    // leaves caller ownership unchanged. Successful recycling clears logical size.
    [[nodiscard]] bool recycle(std::vector<std::uint8_t>&& frame) noexcept {
        if (frame.capacity() < frame_capacity_ || free_.size() == count_) return false;
        frame.clear();
        free_.push_back(std::move(frame));
        return true;
    }

private:
    std::size_t count_;
    std::size_t frame_capacity_;
    std::vector<std::vector<std::uint8_t>> free_;
};

} // namespace forwarding
