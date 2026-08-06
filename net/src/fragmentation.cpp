#include "acnet/fragmentation.hpp"

#include "acnet/protocol.hpp"

#include <algorithm>
#include <limits>

namespace acnet {

bool encode_fragment(const Fragment& fragment, std::vector<std::uint8_t>& output) {
    if (fragment.transfer_id == 0 || fragment.count == 0 || fragment.index >= fragment.count ||
        fragment.total_size == 0 || fragment.total_size > kMaximumTransferBytes || fragment.payload.empty() ||
        fragment.payload.size() > kMaxPlaintextPayloadBytes - kFragmentHeaderBytes) return false;
    ByteWriter writer(kMaxPlaintextPayloadBytes);
    if (!writer.u32(fragment.transfer_id) || !writer.u16(fragment.index) || !writer.u16(fragment.count) ||
        !writer.u32(fragment.total_size) ||
        !writer.bytes(fragment.payload.data(), fragment.payload.size())) return false;
    output = writer.data();
    return true;
}

bool decode_fragment(const std::vector<std::uint8_t>& input, Fragment& fragment) {
    if (input.size() <= kFragmentHeaderBytes || input.size() > kMaxPayloadBytes) return false;
    ByteReader reader(input);
    if (!reader.u32(fragment.transfer_id) || !reader.u16(fragment.index) || !reader.u16(fragment.count) ||
        !reader.u32(fragment.total_size) || fragment.transfer_id == 0 || fragment.count == 0 ||
        fragment.index >= fragment.count || fragment.total_size == 0 || fragment.total_size > kMaximumTransferBytes ||
        reader.remaining() == 0) return false;
    fragment.payload.resize(reader.remaining());
    return reader.bytes(fragment.payload.data(), fragment.payload.size()) && reader.finished();
}

bool split_fragments(const std::vector<std::uint8_t>& payload,
                     std::uint32_t transfer_id,
                     std::vector<std::vector<std::uint8_t>>& output) {
    output.clear();
    if (transfer_id == 0 || payload.size() <= kMaxPlaintextPayloadBytes || payload.size() > kMaximumTransferBytes) return false;
    constexpr std::size_t capacity = kMaxPlaintextPayloadBytes - kFragmentHeaderBytes;
    const std::size_t count = (payload.size() + capacity - 1) / capacity;
    if (count > std::numeric_limits<std::uint16_t>::max()) return false;
    output.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t offset = index * capacity;
        const std::size_t size = std::min(capacity, payload.size() - offset);
        Fragment fragment;
        fragment.transfer_id = transfer_id;
        fragment.index = static_cast<std::uint16_t>(index);
        fragment.count = static_cast<std::uint16_t>(count);
        fragment.total_size = static_cast<std::uint32_t>(payload.size());
        fragment.payload.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                payload.begin() + static_cast<std::ptrdiff_t>(offset + size));
        std::vector<std::uint8_t> encoded;
        if (!encode_fragment(fragment, encoded)) {
            output.clear();
            return false;
        }
        output.push_back(std::move(encoded));
    }
    return true;
}

std::optional<std::vector<std::uint8_t>> FragmentReassembler::accept(const Fragment& fragment,
                                                                     std::uint64_t now_ms) {
    if (fragment.transfer_id == 0 || fragment.count == 0 || fragment.index >= fragment.count ||
        fragment.total_size == 0 || fragment.total_size > kMaximumTransferBytes || fragment.payload.empty() ||
        fragment.payload.size() > kMaxPlaintextPayloadBytes - kFragmentHeaderBytes) return std::nullopt;
    auto found = transfers_.find(fragment.transfer_id);
    if (found == transfers_.end()) {
        if (transfers_.size() >= kMaximumConcurrentTransfers) return std::nullopt;
        Transfer transfer;
        transfer.count = fragment.count;
        transfer.total_size = fragment.total_size;
        transfer.fragments.resize(fragment.count);
        transfer.received.resize(fragment.count, false);
        found = transfers_.emplace(fragment.transfer_id, std::move(transfer)).first;
    }
    Transfer& transfer = found->second;
    if (transfer.count != fragment.count || transfer.total_size != fragment.total_size) {
        transfers_.erase(found);
        return std::nullopt;
    }
    transfer.updated_ms = now_ms;
    if (!transfer.received[fragment.index]) {
        if (transfer.received_bytes > transfer.total_size - std::min<std::size_t>(fragment.payload.size(), transfer.total_size)) {
            transfers_.erase(found);
            return std::nullopt;
        }
        transfer.fragments[fragment.index] = fragment.payload;
        transfer.received[fragment.index] = true;
        transfer.received_bytes += fragment.payload.size();
        ++transfer.received_count;
    }
    if (transfer.received_count != transfer.count) return std::nullopt;
    if (transfer.received_bytes != transfer.total_size) {
        transfers_.erase(found);
        return std::nullopt;
    }
    std::vector<std::uint8_t> result;
    result.reserve(transfer.total_size);
    for (const auto& part : transfer.fragments) result.insert(result.end(), part.begin(), part.end());
    transfers_.erase(found);
    return result;
}

void FragmentReassembler::expire(std::uint64_t now_ms, std::uint64_t timeout_ms) {
    for (auto it = transfers_.begin(); it != transfers_.end();) {
        if (now_ms >= it->second.updated_ms && now_ms - it->second.updated_ms > timeout_ms) {
            it = transfers_.erase(it);
        } else {
            ++it;
        }
    }
}

void FragmentReassembler::clear() {
    transfers_.clear();
}

} // namespace acnet
