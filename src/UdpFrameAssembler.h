#pragma once
#include <vector>
#include <cstdint>
#include <cstring>

class UdpFrameAssembler {
public:
    static constexpr size_t maxBytes = 16 * 1024 * 1024;
    void reset() { bytes_.clear(); discarding_ = false; }
    // Preserve legacy 1472-byte fragments, with BMP/JPEG end markers as an
    // additional boundary when the final datagram is also exactly 1472 bytes.
    bool push(const unsigned char* data, size_t size, std::uint64_t peer,
              std::uint64_t now, std::vector<unsigned char>& frame) {
        if (peer != peer_ || now - lastPacket_ > 500) reset();
        peer_ = peer; lastPacket_ = now;
        if (size > 1472 || size > maxBytes - bytes_.size()) {
            bytes_.clear(); discarding_ = true;
        }
        if (discarding_) {
            if (size != 1472) reset();
            return false;
        }
        bytes_.insert(bytes_.end(), data, data + size);
        bool complete = size != 1472;
        if (bytes_.size() >= 6 && bytes_[0] == 'B' && bytes_[1] == 'M') {
            std::uint32_t length;
            std::memcpy(&length, bytes_.data() + 2, 4);
            if (length >= 54 && bytes_.size() == length) complete = true;
        }
        if (bytes_.size() >= 4 && bytes_[0] == 0xff && bytes_[1] == 0xd8 &&
            bytes_[bytes_.size()-2] == 0xff && bytes_.back() == 0xd9) complete = true;
        if (!complete || bytes_.empty()) return false;
        frame.swap(bytes_);
        bytes_.clear();
        return true;
    }
private:
    std::vector<unsigned char> bytes_;
    std::uint64_t peer_ = 0, lastPacket_ = 0;
    bool discarding_ = false;
};
