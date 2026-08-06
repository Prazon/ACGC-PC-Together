#pragma once

#include "acnet/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace acnet {

enum class ReceiveDisposition {
    New,
    Duplicate,
    Stale,
};

struct PendingDatagram {
    Channel channel = Channel::Control;
    std::uint32_t sequence = 0;
    std::vector<std::uint8_t> bytes;
    std::uint64_t last_sent_ms = 0;
    std::uint8_t retry_count = 0;
};

class ReliabilityPeer {
public:
    PacketHeader make_header(MessageType type, Channel channel, SessionId session);
    void track_sent(const PacketHeader& header, const std::vector<std::uint8_t>& packet, std::uint64_t now_ms);
    ReceiveDisposition receive(const PacketHeader& header);
    std::vector<PendingDatagram> retransmissions(std::uint64_t now_ms,
                                                 std::uint64_t retry_after_ms,
                                                 std::uint8_t max_retries);

    std::size_t pending_count() const { return pending_.size(); }
    std::uint64_t dropped_after_retries() const { return dropped_after_retries_; }

private:
    struct ReceiveWindow {
        bool initialized = false;
        std::uint32_t latest = 0;
        std::uint32_t previous_bits = 0;
    };

    static std::uint64_t pending_key(Channel channel, std::uint32_t sequence);
    void acknowledge(Channel channel, std::uint32_t sequence, std::uint32_t previous_bits);
    ReceiveDisposition observe(ReceiveWindow& window, std::uint32_t sequence);

    std::array<std::uint32_t, static_cast<std::size_t>(Channel::Count)> next_sequence_{};
    std::array<ReceiveWindow, static_cast<std::size_t>(Channel::Count)> received_{};
    std::unordered_map<std::uint64_t, PendingDatagram> pending_;
    std::uint64_t dropped_after_retries_ = 0;
};

} // namespace acnet

