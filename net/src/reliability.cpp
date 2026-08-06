#include "acnet/reliability.hpp"

#include "acnet/protocol.hpp"

#include <algorithm>
#include <cstdint>

namespace acnet {
namespace {

bool sequence_more_recent(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::int32_t>(a - b) > 0;
}

std::size_t channel_index(Channel channel) {
    return static_cast<std::size_t>(channel);
}

} // namespace

std::uint64_t ReliabilityPeer::pending_key(Channel channel, std::uint32_t sequence) {
    return (static_cast<std::uint64_t>(channel_index(channel)) << 32) | sequence;
}

PacketHeader ReliabilityPeer::make_header(MessageType type, Channel channel, SessionId session) {
    PacketHeader header;
    header.message_type = type;
    header.channel = channel;
    header.session = session;
    header.sequence = ++next_sequence_[channel_index(channel)];
    if (delivery_for(channel) != Delivery::UnreliableSequenced) header.flags |= PacketReliable;
    const ReceiveWindow& window = received_[channel_index(channel)];
    if (window.initialized) {
        header.acknowledged_sequence = window.latest;
        header.acknowledged_bits = window.previous_bits;
    }
    return header;
}

void ReliabilityPeer::track_sent(const PacketHeader& header,
                                 const std::vector<std::uint8_t>& packet,
                                 std::uint64_t now_ms) {
    if ((header.flags & PacketReliable) == 0) return;
    PendingDatagram pending;
    pending.channel = header.channel;
    pending.sequence = header.sequence;
    pending.bytes = packet;
    pending.last_sent_ms = now_ms;
    pending_[pending_key(header.channel, header.sequence)] = std::move(pending);
}

void ReliabilityPeer::acknowledge(Channel channel,
                                  std::uint32_t sequence,
                                  std::uint32_t previous_bits) {
    if (sequence == 0) return;
    pending_.erase(pending_key(channel, sequence));
    for (std::uint32_t bit = 0; bit < 32; ++bit) {
        if ((previous_bits & (1U << bit)) != 0) {
            pending_.erase(pending_key(channel, sequence - (bit + 1)));
        }
    }
}

ReceiveDisposition ReliabilityPeer::observe(ReceiveWindow& window, std::uint32_t sequence) {
    if (!window.initialized) {
        window.initialized = true;
        window.latest = sequence;
        window.previous_bits = 0;
        return ReceiveDisposition::New;
    }
    if (sequence == window.latest) return ReceiveDisposition::Duplicate;
    if (sequence_more_recent(sequence, window.latest)) {
        const std::uint32_t distance = sequence - window.latest;
        if (distance > 32) {
            window.previous_bits = 0;
        } else if (distance == 32) {
            window.previous_bits = 1U << 31;
        } else {
            window.previous_bits = (window.previous_bits << distance) | (1U << (distance - 1));
        }
        window.latest = sequence;
        return ReceiveDisposition::New;
    }
    const std::uint32_t distance = window.latest - sequence;
    if (distance == 0 || distance > 32) return ReceiveDisposition::Stale;
    const std::uint32_t mask = 1U << (distance - 1);
    if ((window.previous_bits & mask) != 0) return ReceiveDisposition::Duplicate;
    window.previous_bits |= mask;
    return ReceiveDisposition::New;
}

ReceiveDisposition ReliabilityPeer::receive(const PacketHeader& header) {
    if (header.channel == Channel::Count) return ReceiveDisposition::Stale;
    acknowledge(header.channel, header.acknowledged_sequence, header.acknowledged_bits);
    return observe(received_[channel_index(header.channel)], header.sequence);
}

std::vector<PendingDatagram> ReliabilityPeer::retransmissions(std::uint64_t now_ms,
                                                              std::uint64_t retry_after_ms,
                                                              std::uint8_t max_retries) {
    std::vector<PendingDatagram> due;
    std::vector<std::uint64_t> expired;
    for (auto& item : pending_) {
        PendingDatagram& pending = item.second;
        if (now_ms < pending.last_sent_ms || now_ms - pending.last_sent_ms < retry_after_ms) continue;
        if (pending.retry_count >= max_retries) {
            expired.push_back(item.first);
            ++dropped_after_retries_;
            continue;
        }
        ++pending.retry_count;
        pending.last_sent_ms = now_ms;
        due.push_back(pending);
    }
    for (std::uint64_t key : expired) pending_.erase(key);
    std::sort(due.begin(), due.end(), [](const PendingDatagram& a, const PendingDatagram& b) {
        if (a.channel != b.channel) return a.channel < b.channel;
        return a.sequence < b.sequence;
    });
    return due;
}

} // namespace acnet

