#pragma once

#include "acnet/interpolation.hpp"
#include "acnet/fragmentation.hpp"
#include "acnet/crypto.hpp"
#include "acnet/messages.hpp"
#include "acnet/movement.hpp"
#include "acnet/reliability.hpp"
#include "acnet/transport.hpp"
#include "acnet/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace acnet {

struct DecodedPacket;

enum class ClientConnectionState : std::uint8_t {
    Offline,
    Connecting,
    Connected,
    Reconnecting,
    Rejected,
    Failed,
};

struct ClientConfig {
    std::string server_host = "127.0.0.1";
    std::uint16_t server_port = 24680;
    TownId town = 1;
    AccountId account = 1;
    std::uint64_t build_id = 0;
    std::uint32_t input_rate = 30;
    std::uint32_t simulation_rate = 60;
    std::uint64_t timeout_ms = 5000;
    std::string invite_key;
};

struct RemotePresentation {
    EntityId entity = 0;
    AccountId account = 0;
    ZoneId zone = 0;
    Transform transform;
    PlayerAppearance appearance;
};

class ClientRuntime {
public:
    explicit ClientRuntime(ClientConfig config = {});

    bool start(std::uint64_t now_ms, std::string& error);
    void stop(std::uint64_t now_ms);
    bool poll(std::uint64_t now_ms, std::string& error);
    bool frame(std::uint64_t now_ms,
               std::int16_t stick_x,
               std::int16_t stick_y,
               std::uint16_t buttons,
               std::uint16_t action,
               const Transform& diagnostic_local,
               Transform& corrected_local,
               bool& has_correction,
               std::string& error);

    std::vector<RemotePresentation> remote_players() const;
    ClientConnectionState state() const { return state_; }
    SessionId session() const { return session_; }
    EntityId local_entity() const { return local_entity_; }
    Tick server_tick() const { return latest_server_tick_; }
    ResultCode rejection_reason() const { return rejection_reason_; }
    const std::string& last_error() const { return last_error_; }
    std::uint64_t packets_received() const { return packets_received_; }
    std::uint64_t packets_sent() const { return packets_sent_; }
    const ZoneBaseline* baseline() const { return has_baseline_ ? &baseline_ : nullptr; }
    Revision baseline_revision() const { return baseline_revision_; }
    std::int64_t estimated_town_time(std::uint64_t now_ms) const;
    std::uint32_t town_seed() const { return town_seed_; }
    std::uint16_t town_land_id() const { return town_land_id_; }
    std::uint8_t resident_slot() const { return resident_slot_; }
    bool town_initialized() const { return town_initialized_; }
    const std::array<std::uint8_t, 8>& town_name() const { return town_name_; }

    bool submit_town_bootstrap(TownBootstrap bootstrap, std::uint64_t now_ms, std::string& error);
    bool request(WorldOperation operation, std::uint64_t now_ms, std::string& error);
    bool request(EconomyRequest request, std::uint64_t now_ms, std::string& error);
    bool request(TradeRequest request, std::uint64_t now_ms, std::string& error);
    bool request(ConversationRequest request, std::uint64_t now_ms, std::string& error);
    bool request(ZoneTransferRequest request, std::uint64_t now_ms, std::string& error);
    bool ready(ZoneReadyRequest request, std::uint64_t now_ms, std::string& error);
    bool request(FurnitureOperation operation, std::uint64_t now_ms, std::string& error);
    bool request(EncounterRequest request, std::uint64_t now_ms, std::string& error);

    std::optional<WorldResult> take_world_result();
    std::optional<EconomyResult> take_economy_result();
    std::optional<TradeResult> take_trade_result();
    std::optional<ConversationResult> take_conversation_result();
    std::optional<TransferOffer> take_transfer_offer();
    std::optional<FurnitureResult> take_furniture_result();
    std::optional<EncounterResult> take_encounter_result();
    std::optional<TownBootstrapResult> take_town_bootstrap_result();

private:
    struct RemoteTrack {
        EntityId entity = 0;
        AccountId account = 0;
        ZoneId zone = 0;
        Tick last_tick = 0;
        TransformHistory history{32};
        PlayerAppearance appearance;
    };

    bool send_hello(std::uint64_t now_ms, std::string& error);
    bool send_payload(MessageType type,
                      Channel channel,
                      const std::vector<std::uint8_t>& payload,
                      std::uint64_t now_ms,
                      std::string& error);
    bool send_ack(Channel channel, std::uint64_t now_ms, std::string& error);
    bool handle_datagram(const Datagram& datagram, std::uint64_t now_ms, std::string& error);
    bool handle_server_hello(const DecodedPacket& packet, std::uint64_t now_ms, std::string& error);
    bool handle_snapshot(const DecodedPacket& packet, std::uint64_t now_ms);
    bool dispatch(DecodedPacket packet, std::uint64_t now_ms, std::string& error);
    Tick estimated_server_tick(std::uint64_t now_ms) const;

    ClientConfig config_;
    UdpSocket socket_;
    ReliabilityPeer reliability_;
    ClientPredictor predictor_;
    ClientConnectionState state_ = ClientConnectionState::Offline;
    ResultCode rejection_reason_ = ResultCode::Ok;
    SessionId session_ = 0;
    EntityId local_entity_ = 0;
    std::uint64_t client_nonce_ = 0;
    ClientHello pending_hello_;
    SessionKeys session_keys_;
    bool encryption_active_ = false;
    std::uint64_t last_hello_ms_ = 0;
    std::uint64_t last_send_ms_ = 0;
    std::uint64_t last_input_send_ms_ = 0;
    std::uint64_t last_control_send_ms_ = 0;
    std::uint64_t last_receive_ms_ = 0;
    std::uint64_t server_tick_received_ms_ = 0;
    Tick latest_server_tick_ = 0;
    std::array<std::uint8_t, kReconnectTokenBytes> reconnect_token_{};
    std::uint8_t reconnect_token_size_ = 0;
    std::unordered_map<AccountId, RemoteTrack> remotes_;
    FragmentReassembler fragments_;
    std::uint32_t next_transfer_id_ = 1;
    ZoneBaseline baseline_;
    std::uint64_t baseline_received_ms_ = 0;
    Revision baseline_revision_ = 0;
    bool has_baseline_ = false;
    std::uint32_t town_seed_ = 0;
    std::uint16_t town_land_id_ = 0;
    std::uint8_t resident_slot_ = 0xFF;
    bool town_initialized_ = false;
    std::array<std::uint8_t, 8> town_name_{};
    std::optional<WorldResult> world_result_;
    std::optional<EconomyResult> economy_result_;
    std::optional<TradeResult> trade_result_;
    std::optional<ConversationResult> conversation_result_;
    std::optional<TransferOffer> transfer_offer_;
    std::optional<FurnitureResult> furniture_result_;
    std::optional<EncounterResult> encounter_result_;
    std::optional<TownBootstrapResult> town_bootstrap_result_;
    Transform authoritative_local_;
    std::uint32_t authoritative_ack_ = 0;
    bool has_authoritative_local_ = false;
    std::string last_error_;
    std::uint64_t packets_received_ = 0;
    std::uint64_t packets_sent_ = 0;
};

std::uint64_t client_monotonic_milliseconds();

} // namespace acnet
