#include "acnet/session.hpp"
#include "acnet/crypto.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <stdexcept>

namespace acnet {
SessionTable::SessionTable(SessionConfig config, std::uint64_t random_seed) : config_(config) {
    if (config_.capacity == 0) config_.capacity = 1;
    if (config_.minimum_protocol > config_.maximum_protocol) {
        std::swap(config_.minimum_protocol, config_.maximum_protocol);
    }
    secure_random_ = random_seed == 0;
    if (!secure_random_) random_.seed(random_seed);
}

std::uint64_t SessionTable::random_u64() {
    std::uint64_t value = 0;
    while (value == 0) {
        if (secure_random_) {
            if (!secure_random(reinterpret_cast<std::uint8_t*>(&value), sizeof(value)))
                throw std::runtime_error("operating-system random source failed");
        } else {
            value = random_();
        }
    }
    return value;
}

void SessionTable::random_bytes(std::uint8_t* output, std::size_t size) {
    if (secure_random_) {
        if (!secure_random(output, size)) throw std::runtime_error("operating-system random source failed");
        return;
    }
    std::uint64_t block = 0;
    for (std::size_t i = 0; i < size; ++i) {
        if ((i % sizeof(block)) == 0) block = random_u64();
        output[i] = static_cast<std::uint8_t>(block >> ((i % sizeof(block)) * 8));
    }
}

bool SessionTable::reconnect_matches(const Session& session,
                                     const ClientHello& hello,
                                     std::uint64_t now_ms) const {
    return session.state == SessionState::Reconnecting && now_ms <= session.expires_at_ms &&
           hello.reconnect_token_size == kReconnectTokenBytes &&
           constant_time_equal(session.reconnect_token.data(), hello.reconnect_token.data(), kReconnectTokenBytes);
}

ServerHello SessionTable::accept(const ClientHello& hello,
                                 EntityId player_entity,
                                 Tick tick,
                                 std::uint64_t now_ms) {
    ServerHello reply;
    reply.server_tick = tick;
    if (hello.account == 0 || hello.town == 0 || player_entity == 0) {
        reply.result = ResultCode::Malformed;
        return reply;
    }
    const std::uint16_t lower = std::max(hello.minimum_version, config_.minimum_protocol);
    const std::uint16_t upper = std::min(hello.maximum_version, config_.maximum_protocol);
    if (lower > upper) {
        reply.result = ResultCode::UnsupportedVersion;
        return reply;
    }
    reply.negotiated_version = upper;
    if (config_.required_build_id != 0 && hello.build_id != config_.required_build_id) {
        reply.result = ResultCode::UnsupportedVersion;
        return reply;
    }

    Session* old = by_account(hello.account);
    if (old != nullptr) {
        if (!reconnect_matches(*old, hello, now_ms) || old->town != hello.town) {
            reply.result = ResultCode::Conflict;
            return reply;
        }
        old->state = SessionState::Active;
        old->expires_at_ms = 0;
        old->client_nonce = hello.client_nonce;
        old->server_nonce = random_u64();
        random_bytes(old->reconnect_token.data(), old->reconnect_token.size());
        reply.result = ResultCode::Ok;
        reply.session = old->id;
        reply.player_entity = old->player_entity;
        reply.server_nonce = old->server_nonce;
        reply.reconnect_token = old->reconnect_token;
        reply.reconnect_token_size = kReconnectTokenBytes;
        return reply;
    }

    if (active_count() >= config_.capacity) {
        reply.result = ResultCode::Capacity;
        return reply;
    }
    Session session;
    do {
        session.id = random_u64();
    } while (sessions_.find(session.id) != sessions_.end());
    session.account = hello.account;
    session.town = hello.town;
    session.player_entity = player_entity;
    session.client_nonce = hello.client_nonce;
    session.server_nonce = random_u64();
    random_bytes(session.reconnect_token.data(), session.reconnect_token.size());
    const SessionId id = session.id;
    sessions_.emplace(id, session);
    accounts_[hello.account] = id;

    const Session& created = sessions_.at(id);
    reply.result = ResultCode::Ok;
    reply.session = id;
    reply.player_entity = player_entity;
    reply.server_nonce = created.server_nonce;
    reply.reconnect_token = created.reconnect_token;
    reply.reconnect_token_size = kReconnectTokenBytes;
    return reply;
}

bool SessionTable::disconnect(SessionId id, std::uint64_t now_ms) {
    Session* session = find(id);
    if (session == nullptr || session->state != SessionState::Active) return false;
    session->state = SessionState::Reconnecting;
    session->expires_at_ms = now_ms + config_.reconnect_window_ms;
    return true;
}

bool SessionTable::close(SessionId id) {
    const auto found = sessions_.find(id);
    if (found == sessions_.end()) return false;
    accounts_.erase(found->second.account);
    sessions_.erase(found);
    return true;
}

std::size_t SessionTable::expire(std::uint64_t now_ms) {
    std::vector<SessionId> expired;
    for (const auto& item : sessions_) {
        if (item.second.state == SessionState::Reconnecting && now_ms > item.second.expires_at_ms) {
            expired.push_back(item.first);
        }
    }
    for (SessionId id : expired) close(id);
    return expired.size();
}

Session* SessionTable::find(SessionId id) {
    const auto found = sessions_.find(id);
    return found == sessions_.end() ? nullptr : &found->second;
}

const Session* SessionTable::find(SessionId id) const {
    const auto found = sessions_.find(id);
    return found == sessions_.end() ? nullptr : &found->second;
}

Session* SessionTable::by_account(AccountId account) {
    const auto found = accounts_.find(account);
    return found == accounts_.end() ? nullptr : find(found->second);
}

std::size_t SessionTable::active_count() const {
    std::size_t result = 0;
    for (const auto& item : sessions_) {
        if (item.second.state == SessionState::Active) ++result;
    }
    return result;
}

} // namespace acnet
