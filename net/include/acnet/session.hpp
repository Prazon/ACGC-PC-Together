#pragma once

#include "acnet/types.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <unordered_map>

namespace acnet {

enum class SessionState : std::uint8_t {
    Active,
    Reconnecting,
    Closing,
};

struct Session {
    SessionId id = 0;
    AccountId account = 0;
    TownId town = 0;
    EntityId player_entity = 0;
    ZoneId zone = 1;
    SessionState state = SessionState::Active;
    std::uint64_t client_nonce = 0;
    std::uint64_t server_nonce = 0;
    std::array<std::uint8_t, kReconnectTokenBytes> reconnect_token{};
    Tick last_input_tick = 0;
    std::uint32_t last_input_sequence = 0;
    std::uint64_t expires_at_ms = 0;
};

struct SessionConfig {
    std::size_t capacity = 16;
    std::uint16_t minimum_protocol = kProtocolVersion;
    std::uint16_t maximum_protocol = kProtocolVersion;
    std::uint64_t required_build_id = 0;
    std::uint64_t supported_features = 0;
    std::uint64_t reconnect_window_ms = 30000;
};

class SessionTable {
public:
    explicit SessionTable(SessionConfig config, std::uint64_t random_seed = 0);

    ServerHello accept(const ClientHello& hello, EntityId player_entity, Tick tick, std::uint64_t now_ms);
    bool disconnect(SessionId id, std::uint64_t now_ms);
    bool close(SessionId id);
    std::size_t expire(std::uint64_t now_ms);

    Session* find(SessionId id);
    const Session* find(SessionId id) const;
    Session* by_account(AccountId account);
    std::size_t active_count() const;
    const SessionConfig& config() const { return config_; }

private:
    std::uint64_t random_u64();
    void random_bytes(std::uint8_t* output, std::size_t size);
    bool reconnect_matches(const Session& session, const ClientHello& hello, std::uint64_t now_ms) const;

    SessionConfig config_;
    std::mt19937_64 random_;
    bool secure_random_ = true;
    std::unordered_map<SessionId, Session> sessions_;
    std::unordered_map<AccountId, SessionId> accounts_;
};

} // namespace acnet
