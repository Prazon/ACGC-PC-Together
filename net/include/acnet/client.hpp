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
#include <deque>
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

/* One tile change that arrived as a delta rather than in a baseline, carrying
 * the cause and the acting account so a viewer can animate it. */
struct TileChange {
    TileAddress address;
    TileState state;
    AccountId actor = 0;
    TileChangeCause cause = TileChangeCause::Server;
};

struct RemotePresentation {
    EntityId entity = 0;
    AccountId account = 0;
    ZoneId zone = 0;
    Transform transform;
    PlayerAppearance appearance;
    PlayerPresentation presentation;
    CustomPattern pattern;
    DoorTransitionPhase transition_phase = DoorTransitionPhase::None;
    std::uint32_t transition_door = 0;
    Tick transition_expires_tick = 0;
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
               const PlayerAnimation& animation,
               const PlayerAppearanceBits& appearance_bits,
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
    /* Counts whole baselines received, and nothing else. baseline_revision()
     * moves for every delta of every kind -- a viewer that keyed its bulk tile
     * projection on it rewrote the entire interest chunk each time a nearby
     * player changed animation. */
    std::uint32_t baseline_serial() const { return baseline_serial_; }
    /* Tile changes that arrived as deltas since the last drain, oldest first.
     * Separate from the baseline mirror because a viewer wants to react to the
     * change -- animate a drop -- not just observe the new state. A baseline
     * supersedes the queue and clears it. */
    std::size_t drain_tile_changes(TileChange* output, std::size_t capacity);
    std::size_t pending_tile_changes() const { return tile_changes_.size(); }
    /* True when the queue evicted an entry before it could be drained. The
     * reader must fall back to a full projection: the lost change is otherwise
     * invisible until the next baseline. Cleared by the drain. */
    bool tile_changes_overflowed() const { return tile_changes_overflowed_; }
    std::int64_t estimated_town_time(std::uint64_t now_ms) const;
    std::uint32_t town_seed() const { return town_seed_; }
    std::uint16_t town_land_id() const { return town_land_id_; }
    std::uint8_t resident_slot() const { return resident_slot_; }
    bool town_initialized() const { return town_initialized_; }
    const std::array<std::uint8_t, 8>& town_name() const { return town_name_; }
    std::uint8_t house_light_mask() const { return house_light_mask_; }
    /* Town-wide, refreshed by baselines and by Town deltas between them.
     * Population 0 means the server has not reported one. */
    std::uint8_t town_population() const { return town_population_; }
    std::uint8_t town_capacity() const { return town_capacity_; }
    /* Who owns the four original houses. Empty until the first baseline, which
     * is why has_residents() is separate from an all-vacant roster. */
    const ResidentRoster& residents() const { return residents_; }
    bool has_residents() const { return has_residents_; }
    /* The four resident gyroids, slot-indexed; seeded by the baseline and kept
     * live by Gyroid deltas. The serial counts changes -- baseline or delta --
     * so a viewer reprojects only when something actually moved, exactly like
     * baseline_serial(). Zero until the first baseline. */
    const ZoneBaseline::GyroidEntry* gyroid(std::size_t slot) const {
        return slot < gyroids_.size() && gyroids_[slot].occupied ? &gyroids_[slot] : nullptr;
    }
    std::uint32_t gyroid_serial() const { return gyroid_serial_; }
    /* The authoritative mailbox mirror: the baseline seeds it and Mail deltas
     * keep it live, so a claim always quotes a revision the server issued. */
    const MailboxState& mailbox() const { return baseline_.mailbox; }
    const std::vector<MailRecord>& mail() const { return baseline_.mail; }

    bool submit_town_bootstrap(TownBootstrap bootstrap, std::uint64_t now_ms, std::string& error);
    bool update_appearance(AppearanceUpdate update, std::uint64_t now_ms, std::string& error);
    bool request(WorldOperation operation, std::uint64_t now_ms, std::string& error);
    bool request(EconomyRequest request, std::uint64_t now_ms, std::string& error);
    bool request(TradeRequest request, std::uint64_t now_ms, std::string& error);
    bool request(ConversationRequest request, std::uint64_t now_ms, std::string& error);
    bool request(ZoneTransferRequest request, std::uint64_t now_ms, std::string& error);
    bool ready(ZoneReadyRequest request, std::uint64_t now_ms, std::string& error);
    /* Applies one already-decrypted message, reassembling fragments first. This
     * is what handle_datagram calls once a datagram has been authenticated, and
     * it is public so a test can drive message handling without standing up a
     * server and a handshake. It performs no authentication of its own -- never
     * hand it bytes that have not been through the transport. */
    bool dispatch(DecodedPacket packet, std::uint64_t now_ms, std::string& error);
    bool request(FurnitureOperation operation, std::uint64_t now_ms, std::string& error);
    bool request(HouseUpdate update, std::uint64_t now_ms, std::string& error);
    bool request(EncounterRequest request, std::uint64_t now_ms, std::string& error);
    bool request(GyroidOperation operation, std::uint64_t now_ms, std::string& error);

    std::optional<WorldResult> take_world_result();
    std::optional<EconomyResult> take_economy_result();
    std::optional<TradeResult> take_trade_result();
    std::optional<ConversationResult> take_conversation_result();
    std::optional<TransferOffer> take_transfer_offer();
    std::optional<FurnitureResult> take_furniture_result();
    std::optional<HouseUpdateResult> take_house_update_result();
    std::optional<TownTuneResult> take_town_tune_result();
    std::optional<NoticePostResult> take_notice_result();
    std::optional<VillagerResult> take_villager_result();
    bool request_villager(const VillagerRequest& request, std::uint64_t now_ms, std::string& error);
    bool request_notice_post(const NoticePost& post, std::uint64_t now_ms, std::string& error);
    bool request_town_tune(std::uint64_t notes, std::uint64_t now_ms, std::string& error);
    std::optional<EncounterResult> take_encounter_result();
    std::optional<GyroidResult> take_gyroid_result();
    std::optional<TownBootstrapResult> take_town_bootstrap_result();

private:
    struct RemoteTrack {
        EntityId entity = 0;
        AccountId account = 0;
        ZoneId zone = 0;
        Tick last_tick = 0;
        TransformHistory history{32};
        PlayerAppearance appearance;
        /* Seeded by the baseline, kept live by Player deltas. Snapshots never
         * carry it, so it must survive every snapshot that arrives between. */
        PlayerPresentation presentation;
        CustomPattern pattern;
        DoorTransitionPhase transition_phase = DoorTransitionPhase::None;
        std::uint32_t transition_door = 0;
        Tick transition_expires_tick = 0;
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
    std::uint64_t current_time_ms_ = 0;
    Tick latest_server_tick_ = 0;
    std::array<std::uint8_t, kReconnectTokenBytes> reconnect_token_{};
    std::uint8_t reconnect_token_size_ = 0;
    std::unordered_map<AccountId, RemoteTrack> remotes_;
    FragmentReassembler fragments_;
    std::uint32_t next_transfer_id_ = 1;
    ZoneBaseline baseline_;
    std::uint64_t baseline_received_ms_ = 0;
    Revision baseline_revision_ = 0;
    std::uint32_t baseline_serial_ = 0;
    /* Bounded: a viewer that stops draining must degrade to a full projection
     * rather than grow this without limit. */
    static constexpr std::size_t kMaxQueuedTileChanges = 256;
    /* The mirror keeps out-of-window tiles so a request near the chunk edge can
     * still quote a revision, but it is not a whole-zone table and must not
     * grow into one between baselines. */
    static constexpr std::size_t kMaxMirroredTiles = 1024;
    std::deque<TileChange> tile_changes_;
    bool tile_changes_overflowed_ = false;
    bool has_baseline_ = false;
    std::uint32_t town_seed_ = 0;
    std::uint16_t town_land_id_ = 0;
    std::uint8_t resident_slot_ = 0xFF;
    bool town_initialized_ = false;
    std::array<std::uint8_t, 8> town_name_{};
    std::uint8_t house_light_mask_ = 0;
    std::uint8_t town_population_ = 0;
    std::uint8_t town_capacity_ = 1;
    ResidentRoster residents_;
    bool has_residents_ = false;
    std::array<ZoneBaseline::GyroidEntry, kOriginalResidentSlots> gyroids_{};
    std::uint32_t gyroid_serial_ = 0;
    std::optional<WorldResult> world_result_;
    std::optional<EconomyResult> economy_result_;
    std::optional<TradeResult> trade_result_;
    std::optional<ConversationResult> conversation_result_;
    std::optional<TransferOffer> transfer_offer_;
    std::optional<FurnitureResult> furniture_result_;
    std::optional<HouseUpdateResult> house_update_result_;
    std::optional<TownTuneResult> town_tune_result_;
    std::optional<NoticePostResult> notice_result_;
    std::optional<VillagerResult> villager_result_;
    std::optional<EncounterResult> encounter_result_;
    std::optional<GyroidResult> gyroid_result_;
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
