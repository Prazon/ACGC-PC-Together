#pragma once

#include "acnet/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace acnet {

constexpr std::size_t kFragmentHeaderBytes = 12;
constexpr std::size_t kMaximumTransferBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumConcurrentTransfers = 8;

struct Fragment {
    std::uint32_t transfer_id = 0;
    std::uint16_t index = 0;
    std::uint16_t count = 0;
    std::uint32_t total_size = 0;
    std::vector<std::uint8_t> payload;
};

bool encode_fragment(const Fragment& fragment, std::vector<std::uint8_t>& output);
bool decode_fragment(const std::vector<std::uint8_t>& input, Fragment& fragment);
bool split_fragments(const std::vector<std::uint8_t>& payload,
                     std::uint32_t transfer_id,
                     std::vector<std::vector<std::uint8_t>>& output);

class FragmentReassembler {
public:
    std::optional<std::vector<std::uint8_t>> accept(const Fragment& fragment, std::uint64_t now_ms);
    void expire(std::uint64_t now_ms, std::uint64_t timeout_ms = 10000);
    void clear();
    std::size_t pending() const { return transfers_.size(); }

private:
    struct Transfer {
        std::uint16_t count = 0;
        std::uint32_t total_size = 0;
        std::uint64_t updated_ms = 0;
        std::size_t received_bytes = 0;
        std::size_t received_count = 0;
        std::vector<std::vector<std::uint8_t>> fragments;
        std::vector<bool> received;
    };

    std::unordered_map<std::uint32_t, Transfer> transfers_;
};

} // namespace acnet
