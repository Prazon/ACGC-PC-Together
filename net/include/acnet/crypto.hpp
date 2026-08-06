#pragma once

#include "acnet/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace acnet {

using CryptoKey = std::array<std::uint8_t, 32>;

struct SessionKeys {
    CryptoKey client_to_server{};
    CryptoKey server_to_client{};
};

bool secure_random(std::uint8_t* output, std::size_t size);
std::array<std::uint8_t, 32> sha256(const std::uint8_t* data, std::size_t size);
CryptoKey hmac_sha256(const std::vector<std::uint8_t>& key,
                      const std::uint8_t* data,
                      std::size_t size);
bool constant_time_equal(const std::uint8_t* first, const std::uint8_t* second, std::size_t size);

std::array<std::uint8_t, 32> invite_proof(const ClientHello& hello, const std::string& invite_key);
std::array<std::uint8_t, 32> server_proof(const ServerHello& hello,
                                         std::uint64_t client_nonce,
                                         const std::string& invite_key);
SessionKeys derive_session_keys(const ClientHello& client,
                                const ServerHello& server,
                                const std::string& invite_key);

bool seal_payload(const PacketHeader& header,
                  const CryptoKey& key,
                  const std::vector<std::uint8_t>& plaintext,
                  std::vector<std::uint8_t>& sealed);
bool open_payload(const PacketHeader& header,
                  const CryptoKey& key,
                  const std::vector<std::uint8_t>& sealed,
                  std::vector<std::uint8_t>& plaintext);

} // namespace acnet
