#include "acnet/client.hpp"

#include "acnet/protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <random>

namespace acnet {
namespace {

std::uint64_t random_nonzero_u64() {
    std::random_device source;
    std::uint64_t value = 0;
    while (value == 0) {
        value = (static_cast<std::uint64_t>(source()) << 32) ^ source();
    }
    return value;
}

bool endpoint_matches(const Datagram& datagram, const ClientConfig& config) {
    return datagram.host == config.server_host && datagram.port == config.server_port;
}

} // namespace

std::uint64_t client_monotonic_milliseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
}

ClientRuntime::ClientRuntime(ClientConfig config) : config_(std::move(config)), predictor_() {
    if (config_.input_rate == 0) config_.input_rate = 30;
    if (config_.simulation_rate == 0) config_.simulation_rate = 60;
    if (config_.timeout_ms < 1000) config_.timeout_ms = 1000;
}

std::int64_t ClientRuntime::estimated_town_time(std::uint64_t now_ms) const {
    if (!has_baseline_) return 0;
    if (now_ms <= baseline_received_ms_) return baseline_.town_unix_seconds;
    const std::uint64_t elapsed_seconds = (now_ms - baseline_received_ms_) / 1000;
    if (elapsed_seconds > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() -
                                                     baseline_.town_unix_seconds))
        return std::numeric_limits<std::int64_t>::max();
    return baseline_.town_unix_seconds + static_cast<std::int64_t>(elapsed_seconds);
}

bool ClientRuntime::start(std::uint64_t now_ms, std::string& error) {
    error.clear();
    if (state_ != ClientConnectionState::Offline || config_.server_port == 0 || config_.town == 0 ||
        config_.account == 0) {
        error = "invalid client configuration or state";
        return false;
    }
    if (!socket_.open(0, error)) {
        state_ = ClientConnectionState::Failed;
        last_error_ = error;
        return false;
    }
    reliability_ = ReliabilityPeer{};
    client_nonce_ = random_nonzero_u64();
    state_ = ClientConnectionState::Connecting;
    return send_hello(now_ms, error);
}

void ClientRuntime::stop(std::uint64_t now_ms) {
    if (state_ == ClientConnectionState::Connected && session_ != 0) {
        std::string ignored;
        /* UDP has no close handshake. Send a short best-effort burst so an
         * orderly exit normally releases presence immediately; timeout still
         * remains the authoritative fallback. */
        for (int attempt = 0; attempt < 3; ++attempt) {
            send_payload(MessageType::Disconnect, Channel::Control, {}, now_ms, ignored);
        }
    }
    socket_.close();
    remotes_.clear();
    house_light_mask_ = 0;
    town_population_ = 0;
    town_capacity_ = 1;
    session_ = 0;
    local_entity_ = 0;
    has_authoritative_local_ = false;
    has_baseline_ = false;
    baseline_revision_ = 0;
    fragments_.clear();
    encryption_active_ = false;
    state_ = ClientConnectionState::Offline;
}

bool ClientRuntime::send_hello(std::uint64_t now_ms, std::string& error) {
    ClientHello hello;
    hello.build_id = config_.build_id;
    hello.town = config_.town;
    hello.account = config_.account;
    hello.client_nonce = client_nonce_;
    hello.reconnect_token = reconnect_token_;
    hello.reconnect_token_size = reconnect_token_size_;
    if (!config_.invite_key.empty()) hello.invite_proof = invite_proof(hello, config_.invite_key);
    pending_hello_ = hello;
    std::vector<std::uint8_t> payload;
    if (!encode(hello, payload)) {
        error = "failed to encode client hello";
        return false;
    }
    PacketHeader header = reliability_.make_header(MessageType::ClientHello, Channel::Control, 0);
    std::vector<std::uint8_t> packet;
    if (!encode_packet(header, payload, packet, error) ||
        !socket_.send(config_.server_host, config_.server_port, packet, error)) {
        last_error_ = error;
        return false;
    }
    reliability_.track_sent(header, packet, now_ms);
    last_hello_ms_ = now_ms;
    last_send_ms_ = now_ms;
    last_control_send_ms_ = now_ms;
    ++packets_sent_;
    return true;
}

bool ClientRuntime::send_payload(MessageType type,
                                 Channel channel,
                                 const std::vector<std::uint8_t>& payload,
                                 std::uint64_t now_ms,
                                 std::string& error) {
    if (session_ == 0) {
        error = "client session is not established";
        return false;
    }
    if (payload.size() > kMaxPlaintextPayloadBytes) {
        std::vector<std::vector<std::uint8_t>> fragments;
        if (!split_fragments(payload, next_transfer_id_++, fragments)) {
            error = "failed to fragment client payload";
            return false;
        }
        if (next_transfer_id_ == 0) next_transfer_id_ = 1;
        for (const auto& fragment : fragments) {
            PacketHeader header = reliability_.make_header(type, Channel::Bulk, session_);
            header.flags = static_cast<std::uint8_t>(header.flags | PacketFragment);
            std::vector<std::uint8_t> wire_payload = fragment;
            if (encryption_active_) {
                header.flags = static_cast<std::uint8_t>(header.flags | PacketEncrypted);
                if (!seal_payload(header, session_keys_.client_to_server, fragment, wire_payload)) {
                    error = "failed to encrypt client fragment";
                    return false;
                }
            }
            std::vector<std::uint8_t> packet;
            if (!encode_packet(header, wire_payload, packet, error) ||
                !socket_.send(config_.server_host, config_.server_port, packet, error)) return false;
            reliability_.track_sent(header, packet, now_ms);
            ++packets_sent_;
        }
    } else {
        PacketHeader header = reliability_.make_header(type, channel, session_);
        std::vector<std::uint8_t> wire_payload = payload;
        if (encryption_active_) {
            header.flags = static_cast<std::uint8_t>(header.flags | PacketEncrypted);
            if (!seal_payload(header, session_keys_.client_to_server, payload, wire_payload)) {
                error = "failed to encrypt client payload";
                return false;
            }
        }
        std::vector<std::uint8_t> packet;
        if (!encode_packet(header, wire_payload, packet, error) ||
            !socket_.send(config_.server_host, config_.server_port, packet, error)) {
            last_error_ = error;
            return false;
        }
        reliability_.track_sent(header, packet, now_ms);
        ++packets_sent_;
    }
    last_send_ms_ = now_ms;
    if (channel == Channel::Control) last_control_send_ms_ = now_ms;
    if (type == MessageType::InputCommand) last_input_send_ms_ = now_ms;
    return true;
}

bool ClientRuntime::send_ack(Channel channel, std::uint64_t now_ms, std::string& error) {
    PacketHeader header = reliability_.make_header(MessageType::Pong, channel, session_);
    header.flags = PacketAckOnly;
    std::vector<std::uint8_t> payload;
    if (encryption_active_) {
        header.flags = static_cast<std::uint8_t>(header.flags | PacketEncrypted);
        if (!seal_payload(header, session_keys_.client_to_server, {}, payload)) return false;
    }
    std::vector<std::uint8_t> packet;
    if (!encode_packet(header, payload, packet, error) ||
        !socket_.send(config_.server_host, config_.server_port, packet, error)) return false;
    ++packets_sent_;
    (void)now_ms;
    return true;
}

bool ClientRuntime::handle_server_hello(const DecodedPacket& packet,
                                        std::uint64_t now_ms,
                                        std::string& error) {
    ServerHello hello;
    if (!decode(packet.payload, hello)) {
        error = "malformed server hello";
        return false;
    }
    rejection_reason_ = hello.result;
    if (hello.result != ResultCode::Ok) {
        state_ = ClientConnectionState::Rejected;
        return true;
    }
    if (hello.session == 0 || hello.player_entity == 0 || hello.reconnect_token_size != kReconnectTokenBytes) {
        error = "invalid successful server hello";
        return false;
    }
    if (!config_.invite_key.empty()) {
        const auto expected = server_proof(hello, pending_hello_.client_nonce, config_.invite_key);
        if (!constant_time_equal(expected.data(), hello.server_proof.data(), expected.size())) {
            error = "server authentication failed";
            return false;
        }
        session_keys_ = derive_session_keys(pending_hello_, hello, config_.invite_key);
        encryption_active_ = true;
    } else {
        encryption_active_ = false;
    }
    session_ = hello.session;
    local_entity_ = hello.player_entity;
    town_seed_ = hello.town_seed;
    town_land_id_ = hello.town_land_id;
    resident_slot_ = hello.resident_slot;
    town_initialized_ = hello.town_initialized;
    town_name_ = hello.town_name;
    latest_server_tick_ = hello.server_tick;
    server_tick_received_ms_ = now_ms;
    reconnect_token_ = hello.reconnect_token;
    reconnect_token_size_ = hello.reconnect_token_size;
    state_ = ClientConnectionState::Connected;
    last_receive_ms_ = now_ms;
    remotes_.clear();
    has_authoritative_local_ = false;
    has_baseline_ = false;
    baseline_revision_ = 0;
    return true;
}

bool ClientRuntime::handle_snapshot(const DecodedPacket& packet, std::uint64_t now_ms) {
    TransformSnapshot snapshot;
    if (!decode(packet.payload, snapshot)) return false;
    latest_server_tick_ = snapshot.server_tick;
    house_light_mask_ = snapshot.house_light_mask;
    server_tick_received_ms_ = now_ms;
    for (const PlayerSnapshot& player : snapshot.players) {
        if (player.account == config_.account) {
            authoritative_local_ = player.transform;
            authoritative_ack_ = player.acknowledged_input;
            has_authoritative_local_ = true;
            predictor_.reconcile(player.transform, player.acknowledged_input);
            continue;
        }
        RemoteTrack& remote = remotes_[player.account];
        if (remote.account == 0 || remote.entity != player.entity) {
            remote = {};
            remote.account = player.account;
            remote.entity = player.entity;
        }
        if (remote.zone != 0 && remote.zone != player.zone) remote.history.clear();
        remote.zone = player.zone;
        /* Snapshot appearance is only a revision watermark. The reliable
         * baseline carries the actual fields and optional custom bitmap. */
        remote.transition_phase = player.transition_phase;
        remote.transition_door = player.transition_door;
        remote.transition_expires_tick = player.transition_expires_tick;
        remote.last_tick = snapshot.server_tick;
        remote.history.push(snapshot.server_tick, player.transform);
    }
    for (auto it = remotes_.begin(); it != remotes_.end();) {
        if (snapshot.server_tick - it->second.last_tick > config_.simulation_rate / 2U) {
            it = remotes_.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

bool ClientRuntime::dispatch(DecodedPacket packet, std::uint64_t now_ms, std::string& error) {
    if ((packet.header.flags & PacketFragment) != 0) {
        Fragment fragment;
        if (!decode_fragment(packet.payload, fragment)) {
            error = "malformed fragment";
            return false;
        }
        const auto complete = fragments_.accept(fragment, now_ms);
        if (!complete.has_value()) return true;
        packet.payload = *complete;
        packet.header.flags = static_cast<std::uint8_t>(packet.header.flags & ~PacketFragment);
    }
    switch (packet.header.message_type) {
        case MessageType::TransformSnapshot:
            if (!handle_snapshot(packet, now_ms)) {
                error = "malformed transform snapshot";
                return false;
            }
            break;
        case MessageType::Baseline:
            if (!decode_baseline(packet.payload, baseline_)) {
                error = "malformed baseline";
                return false;
            }
            baseline_revision_ = baseline_.revision;
            house_light_mask_ = baseline_.house_light_mask;
            town_population_ = baseline_.town_population;
            town_capacity_ = baseline_.town_capacity;
            baseline_received_ms_ = now_ms;
            has_baseline_ = true;
            latest_server_tick_ = baseline_.server_tick;
            server_tick_received_ms_ = now_ms;
            for (const PlayerSnapshot& player : baseline_.players) {
                if (player.account == config_.account) continue;
                RemoteTrack& remote = remotes_[player.account];
                if (remote.account == 0 || remote.entity != player.entity) {
                    remote = {};
                    remote.account = player.account;
                    remote.entity = player.entity;
                }
                if (remote.zone != 0 && remote.zone != player.zone) remote.history.clear();
                remote.zone = player.zone;
                remote.appearance = player.appearance;
                remote.presentation = player.presentation;
                remote.pattern = player.pattern;
                remote.transition_phase = player.transition_phase;
                remote.transition_door = player.transition_door;
                remote.transition_expires_tick = player.transition_expires_tick;
                remote.last_tick = baseline_.server_tick;
                remote.history.push(baseline_.server_tick, player.transform);
            }
            break;
        case MessageType::TownBootstrapResult:
            if (!decode(packet.payload, town_bootstrap_result_.emplace())) {
                error = "malformed town bootstrap result";
                return false;
            }
            if (town_bootstrap_result_->code == ResultCode::Ok && town_bootstrap_result_->initialized)
                town_initialized_ = true;
            break;
        case MessageType::AppearanceResult: {
            AppearanceResult result;
            if (!decode(packet.payload, result)) {
                error = "malformed appearance result";
                return false;
            }
            break;
        }
        case MessageType::ReplicationDeltas: {
            std::vector<ReplicationDelta> decoded;
            if (!decode_deltas(packet.payload, decoded)) {
                error = "malformed replication deltas";
                return false;
            }
            for (const ReplicationDelta& delta : decoded) {
                if (delta.kind == ResourceKind::Tile && has_baseline_) {
                    TileStateDelta tile;
                    if (!decode_tile_delta(delta.payload, tile)) {
                        error = "malformed tile replication delta";
                        return false;
                    }
                    const auto found = std::find_if(baseline_.tiles.begin(), baseline_.tiles.end(),
                        [&](const auto& entry) { return entry.first == tile.address; });
                    if (found != baseline_.tiles.end()) found->second = tile.state;
                    else if (tile.address.zone == baseline_.zone) baseline_.tiles.emplace_back(tile.address, tile.state);
                }
                if (delta.kind == ResourceKind::Mail && has_baseline_) {
                    MailDelta mail;
                    if (!decode_mail_delta(delta.payload, mail) || mail.account != config_.account) {
                        error = "malformed mailbox replication delta";
                        return false;
                    }
                    auto& letters = baseline_.mail;
                    const auto found = std::find_if(letters.begin(), letters.end(),
                        [&](const MailRecord& letter) { return letter.id == mail.record.id; });
                    if (mail.removed) {
                        if (found != letters.end()) letters.erase(found);
                    } else if (found != letters.end()) {
                        /* A take is a location change, not a new letter. */
                        *found = mail.record;
                    } else if (letters.size() < kMailboxCapacity + kCarriedMailCapacity) {
                        letters.push_back(mail.record);
                    }
                    baseline_.mailbox.revision = mail.mailbox_revision;
                    baseline_.mailbox.mail.clear();
                    baseline_.mailbox.carried.clear();
                    for (const MailRecord& letter : letters) {
                        if (letter.location == MailLocation::Carried) baseline_.mailbox.carried.push_back(letter.id);
                        else baseline_.mailbox.mail.push_back(letter.id);
                    }
                }
                if (delta.kind == ResourceKind::Player) {
                    PlayerPresentationDelta presentation;
                    if (!decode_player_delta(delta.payload, presentation)) {
                        error = "malformed player presentation delta";
                        return false;
                    }
                    /* Only for someone already being tracked: a delta for a
                     * player this client has no snapshot history for would
                     * create a remote it can never place or retire. */
                    const auto tracked = remotes_.find(presentation.account);
                    if (tracked != remotes_.end() && tracked->second.entity == presentation.entity) {
                        tracked->second.presentation = presentation.presentation;
                    }
                }
                if (delta.kind == ResourceKind::Town) {
                    TownOccupancy occupancy;
                    if (!decode_town_delta(delta.payload, occupancy)) {
                        error = "malformed town occupancy delta";
                        return false;
                    }
                    town_population_ = occupancy.population;
                    town_capacity_ = occupancy.capacity;
                    if (has_baseline_) {
                        baseline_.town_population = occupancy.population;
                        baseline_.town_capacity = occupancy.capacity;
                    }
                }
                if (delta.revision > baseline_revision_) baseline_revision_ = delta.revision;
            }
            break;
        }
        case MessageType::WorldResult:
            if (!decode(packet.payload, world_result_.emplace())) return false;
            if (has_baseline_ && world_result_->code == ResultCode::Ok) {
                baseline_.inventory.revision = world_result_->inventory_revision;
                if (world_result_->transferred_item != 0 &&
                    world_result_->inventory_slot < baseline_.inventory.slots.size()) {
                    ItemSlot& slot = baseline_.inventory.slots[world_result_->inventory_slot];
                    if (slot.item == world_result_->transferred_item) slot = {};
                    else slot.item = world_result_->transferred_item;
                }
            }
            break;
        case MessageType::InventoryResult:
            if (!decode(packet.payload, economy_result_.emplace())) return false;
            if (has_baseline_ && economy_result_->code == ResultCode::Ok) {
                baseline_.inventory.revision = economy_result_->inventory_revision;
                baseline_.inventory.bells = economy_result_->bells;
                /* The balance and debt are echoed by every operation, but
                 * auxiliary_revision belongs to whichever authority the
                 * operation touched. Copying it into the ledger unconditionally
                 * left a shop purchase pointing the bank mirror at the shop's
                 * revision, and the next deposit would have been rejected as
                 * stale. */
                baseline_.ledger.bank_balance = economy_result_->balance;
                baseline_.ledger.debt = economy_result_->debt;
                switch (economy_result_->type) {
                    case EconomyOpType::Deposit:
                    case EconomyOpType::Withdraw:
                    case EconomyOpType::PayDebt:
                        baseline_.ledger.revision = economy_result_->auxiliary_revision;
                        break;
                    case EconomyOpType::Buy:
                    case EconomyOpType::Sell:
                        baseline_.shop.revision = economy_result_->auxiliary_revision;
                        break;
                    case EconomyOpType::AttachMail:
                    case EconomyOpType::TakeMail:
                    case EconomyOpType::ClaimMail:
                    case EconomyOpType::DiscardMail:
                        baseline_.mailbox.revision = economy_result_->auxiliary_revision;
                        break;
                    case EconomyOpType::HoldItem:
                    case EconomyOpType::Donate:
                    case EconomyOpType::AdminGrantBells:
                    case EconomyOpType::AdminSendMail:
                        /* Holding touches only the inventory, the museum is not
                         * mirrored, and the operator operations never reach a
                         * client. */
                        break;
                }
                if (economy_result_->type == EconomyOpType::HoldItem) {
                    /* Mirror the swap rather than the "item appeared in a slot"
                     * heuristic below, which cannot express two slots changing
                     * at once and would drop whatever was already in the hand. */
                    if (economy_result_->inventory_slot < baseline_.inventory.slots.size()) {
                        std::swap(baseline_.inventory.slots[economy_result_->inventory_slot],
                                  baseline_.inventory.equipped);
                    }
                } else if (economy_result_->item != 0 &&
                           economy_result_->inventory_slot < baseline_.inventory.slots.size()) {
                    ItemSlot& slot = baseline_.inventory.slots[economy_result_->inventory_slot];
                    if (slot.item == economy_result_->item) slot = {};
                    else slot.item = economy_result_->item;
                }
            }
            break;
        case MessageType::TradeResult:
            if (!decode(packet.payload, trade_result_.emplace())) return false;
            break;
        case MessageType::ConversationResult:
            if (!decode(packet.payload, conversation_result_.emplace())) return false;
            break;
        case MessageType::ZoneTransferOffer:
            if (!decode(packet.payload, transfer_offer_.emplace())) return false;
            break;
        case MessageType::FurnitureResult:
            if (!decode(packet.payload, furniture_result_.emplace())) return false;
            if (has_baseline_ && furniture_result_->code == ResultCode::Ok) {
                baseline_.inventory.revision = furniture_result_->inventory_revision;
                if (furniture_result_->item != 0 &&
                    furniture_result_->inventory_slot < baseline_.inventory.slots.size()) {
                    ItemSlot& slot = baseline_.inventory.slots[furniture_result_->inventory_slot];
                    if (slot.item == furniture_result_->item) slot = {};
                    else slot.item = furniture_result_->item;
                }
            }
            break;
        case MessageType::HouseUpdateResult:
            if (!decode(packet.payload, house_update_result_.emplace())) return false;
            break;
        case MessageType::EncounterResult:
            if (!decode(packet.payload, encounter_result_.emplace())) return false;
            if (has_baseline_ && encounter_result_->code == ResultCode::Ok) {
                baseline_.inventory.revision = encounter_result_->inventory_revision;
                if (encounter_result_->caught && encounter_result_->inventory_slot < baseline_.inventory.slots.size())
                    baseline_.inventory.slots[encounter_result_->inventory_slot].item = encounter_result_->item;
            }
            break;
        case MessageType::Ping:
            if (!send_payload(MessageType::Pong, Channel::Control, packet.payload, now_ms, error)) return false;
            break;
        case MessageType::Pong:
            break;
        case MessageType::Disconnect:
            session_ = 0;
            state_ = ClientConnectionState::Reconnecting;
            break;
        default:
            break;
    }
    return true;
}

bool ClientRuntime::submit_town_bootstrap(TownBootstrap bootstrap,
                                          std::uint64_t now_ms,
                                          std::string& error) {
    if (state_ != ClientConnectionState::Connected) {
        error = "town bootstrap requires a connected client";
        return false;
    }
    std::vector<std::uint8_t> payload;
    if (!encode(bootstrap, payload)) {
        error = "failed to encode town bootstrap";
        return false;
    }
    return send_payload(MessageType::TownBootstrap, Channel::Bulk, payload, now_ms, error);
}

bool ClientRuntime::update_appearance(AppearanceUpdate update,
                                      std::uint64_t now_ms,
                                      std::string& error) {
    if (state_ != ClientConnectionState::Connected) {
        error = "appearance update requires a connected client";
        return false;
    }
    std::vector<std::uint8_t> payload;
    if (!encode(update, payload)) {
        error = "failed to encode appearance update";
        return false;
    }
    return send_payload(MessageType::AppearanceUpdate, Channel::Transactions, payload, now_ms, error);
}

std::optional<TownBootstrapResult> ClientRuntime::take_town_bootstrap_result() {
    auto value = town_bootstrap_result_;
    town_bootstrap_result_.reset();
    return value;
}

bool ClientRuntime::handle_datagram(const Datagram& datagram, std::uint64_t now_ms, std::string& error) {
    if (!endpoint_matches(datagram, config_)) return true;
    DecodedPacket packet;
    if (!decode_packet(datagram.bytes.data(), datagram.bytes.size(), packet, error)) {
        error.clear();
        return true;
    }
    ++packets_received_;
    if (packet.header.message_type == MessageType::ServerHello) {
        if (state_ == ClientConnectionState::Connected) {
            if (packet.header.session == session_) {
                ServerHello duplicate;
                if (!decode(packet.payload, duplicate)) return true;
                if (!config_.invite_key.empty()) {
                    const auto expected = server_proof(duplicate, pending_hello_.client_nonce, config_.invite_key);
                    if (!constant_time_equal(expected.data(), duplicate.server_proof.data(), expected.size())) return true;
                }
                reliability_.receive(packet.header);
                last_receive_ms_ = now_ms;
            }
            return true;
        }
        if (state_ != ClientConnectionState::Connecting && state_ != ClientConnectionState::Reconnecting) return true;
        reliability_.receive(packet.header);
        return handle_server_hello(packet, now_ms, error);
    }
    if (state_ != ClientConnectionState::Connected || packet.header.session != session_) return true;
    if (encryption_active_) {
        std::vector<std::uint8_t> plaintext;
        if ((packet.header.flags & PacketEncrypted) == 0 ||
            !open_payload(packet.header, session_keys_.server_to_client, packet.payload, plaintext)) {
            error = "server packet authentication failed";
            return false;
        }
        packet.payload = std::move(plaintext);
    } else if ((packet.header.flags & PacketEncrypted) != 0) {
        return true;
    }
    last_receive_ms_ = now_ms;
    if (reliability_.receive(packet.header) != ReceiveDisposition::New) return true;
    if ((packet.header.flags & PacketAckOnly) != 0) return true;
    if ((packet.header.flags & PacketReliable) != 0 && !send_ack(packet.header.channel, now_ms, error)) return false;
    return dispatch(std::move(packet), now_ms, error);
}

bool ClientRuntime::poll(std::uint64_t now_ms, std::string& error) {
    error.clear();
    current_time_ms_ = now_ms;
    if (!socket_.is_open()) {
        error = "client socket is closed";
        return false;
    }
    for (std::size_t i = 0; i < 256; ++i) {
        Datagram datagram;
        if (!socket_.receive(datagram, error)) {
            if (!error.empty()) return false;
            break;
        }
        if (!handle_datagram(datagram, now_ms, error)) return false;
    }
    if ((state_ == ClientConnectionState::Connecting || state_ == ClientConnectionState::Reconnecting) &&
        now_ms - last_hello_ms_ >= 1000) {
        if (!send_hello(now_ms, error)) return false;
    }
    if (state_ == ClientConnectionState::Connected) {
        if (now_ms >= last_receive_ms_ && now_ms - last_receive_ms_ > config_.timeout_ms) {
            state_ = ClientConnectionState::Reconnecting;
            session_ = 0;
            remotes_.clear();
            reliability_ = ReliabilityPeer{};
            if (!send_hello(now_ms, error)) return false;
        } else if (now_ms - last_control_send_ms_ >= 1000) {
            ByteWriter ping(16);
            if (!ping.u64(now_ms) || !send_payload(MessageType::Ping, Channel::Control, ping.data(), now_ms, error))
                return false;
        }
    }
    if (session_ != 0) {
        for (const PendingDatagram& pending : reliability_.retransmissions(now_ms, 250, 8)) {
            if (!socket_.send(config_.server_host, config_.server_port, pending.bytes, error)) return false;
            ++packets_sent_;
        }
    }
    fragments_.expire(now_ms);
    return true;
}

Tick ClientRuntime::estimated_server_tick(std::uint64_t now_ms) const {
    if (latest_server_tick_ == 0 || now_ms < server_tick_received_ms_) return latest_server_tick_;
    const std::uint64_t elapsed = now_ms - server_tick_received_ms_;
    const std::uint64_t advanced = elapsed * config_.simulation_rate / 1000;
    return latest_server_tick_ + static_cast<Tick>(std::min<std::uint64_t>(advanced, 300));
}

bool ClientRuntime::frame(std::uint64_t now_ms,
                          std::int16_t stick_x,
                          std::int16_t stick_y,
                          std::uint16_t buttons,
                          std::uint16_t action,
                          const PlayerAnimation& animation,
                          const Transform& diagnostic_local,
                          Transform& corrected_local,
                          bool& has_correction,
                          std::string& error) {
    corrected_local = diagnostic_local;
    has_correction = false;
    if (!poll(now_ms, error)) return false;
    if (state_ != ClientConnectionState::Connected) return true;
    const std::uint64_t input_interval = std::max<std::uint64_t>(1, 1000 / config_.input_rate);
    if (now_ms - last_input_send_ms_ >= input_interval) {
        InputCommand command =
            predictor_.predict(stick_x, stick_y, buttons, action, animation, estimated_server_tick(now_ms));
        /* The game client owns movement. Send the transform produced by the
         * original player controller/collision code instead of asking the
         * server to reinterpret camera-relative stick axes. */
        command.client_transform = diagnostic_local;
        command.client_transform.action = action;
        std::vector<std::uint8_t> payload;
        if (!encode(command, payload) ||
            !send_payload(MessageType::InputCommand, Channel::Snapshots, payload, now_ms, error)) return false;
    }
    /* Snapshots acknowledge and relay movement but never steer the originating
     * player. Explicit scene/door transitions are handled by the game hooks. */
    has_authoritative_local_ = false;
    (void)authoritative_ack_;
    return true;
}

bool ClientRuntime::request(WorldOperation value, std::uint64_t now_ms, std::string& error) {
    value.account = config_.account;
    std::vector<std::uint8_t> payload;
    if (!encode(value, payload)) { error = "failed to encode world request"; return false; }
    return send_payload(MessageType::WorldRequest, Channel::Transactions, payload, now_ms, error);
}

bool ClientRuntime::request(EconomyRequest value, std::uint64_t now_ms, std::string& error) {
    value.account = config_.account;
    std::vector<std::uint8_t> payload;
    if (!encode(value, payload)) { error = "failed to encode economy request"; return false; }
    return send_payload(MessageType::InventoryRequest, Channel::Transactions, payload, now_ms, error);
}

bool ClientRuntime::request(TradeRequest value, std::uint64_t now_ms, std::string& error) {
    value.account = config_.account;
    std::vector<std::uint8_t> payload;
    if (!encode(value, payload)) { error = "failed to encode trade request"; return false; }
    return send_payload(MessageType::TradeRequest, Channel::Transactions, payload, now_ms, error);
}

bool ClientRuntime::request(ConversationRequest value, std::uint64_t now_ms, std::string& error) {
    value.account = config_.account;
    std::vector<std::uint8_t> payload;
    if (!encode(value, payload)) { error = "failed to encode conversation request"; return false; }
    return send_payload(MessageType::ConversationRequest, Channel::Transactions, payload, now_ms, error);
}

bool ClientRuntime::request(ZoneTransferRequest value, std::uint64_t now_ms, std::string& error) {
    value.account = config_.account;
    std::vector<std::uint8_t> payload;
    if (!encode(value, payload)) { error = "failed to encode zone transfer request"; return false; }
    return send_payload(MessageType::ZoneTransferRequest, Channel::Transactions, payload, now_ms, error);
}

bool ClientRuntime::ready(ZoneReadyRequest value, std::uint64_t now_ms, std::string& error) {
    value.account = config_.account;
    std::vector<std::uint8_t> payload;
    if (!encode(value, payload)) { error = "failed to encode zone ready request"; return false; }
    return send_payload(MessageType::ZoneReady, Channel::Transactions, payload, now_ms, error);
}

bool ClientRuntime::request(FurnitureOperation value, std::uint64_t now_ms, std::string& error) {
    value.account = config_.account;
    std::vector<std::uint8_t> payload;
    if (!encode(value, payload)) { error = "failed to encode furniture request"; return false; }
    return send_payload(MessageType::FurnitureRequest, Channel::Transactions, payload, now_ms, error);
}

bool ClientRuntime::request(HouseUpdate value, std::uint64_t now_ms, std::string& error) {
    value.account = config_.account;
    std::vector<std::uint8_t> payload;
    if (!encode(value, payload)) { error = "failed to encode house update"; return false; }
    return send_payload(MessageType::HouseUpdate, Channel::Bulk, payload, now_ms, error);
}

bool ClientRuntime::request(EncounterRequest value, std::uint64_t now_ms, std::string& error) {
    value.account = config_.account;
    std::vector<std::uint8_t> payload;
    if (!encode(value, payload)) { error = "failed to encode encounter request"; return false; }
    return send_payload(MessageType::EncounterRequest, Channel::Transactions, payload, now_ms, error);
}

std::optional<WorldResult> ClientRuntime::take_world_result() {
    auto result = std::move(world_result_); world_result_.reset(); return result;
}
std::optional<EconomyResult> ClientRuntime::take_economy_result() {
    auto result = std::move(economy_result_); economy_result_.reset(); return result;
}
std::optional<TradeResult> ClientRuntime::take_trade_result() {
    auto result = std::move(trade_result_); trade_result_.reset(); return result;
}
std::optional<ConversationResult> ClientRuntime::take_conversation_result() {
    auto result = std::move(conversation_result_); conversation_result_.reset(); return result;
}
std::optional<TransferOffer> ClientRuntime::take_transfer_offer() {
    auto result = std::move(transfer_offer_); transfer_offer_.reset(); return result;
}
std::optional<FurnitureResult> ClientRuntime::take_furniture_result() {
    auto result = std::move(furniture_result_); furniture_result_.reset(); return result;
}
std::optional<HouseUpdateResult> ClientRuntime::take_house_update_result() {
    auto result = std::move(house_update_result_); house_update_result_.reset(); return result;
}
std::optional<EncounterResult> ClientRuntime::take_encounter_result() {
    auto result = std::move(encounter_result_); encounter_result_.reset(); return result;
}

std::vector<RemotePresentation> ClientRuntime::remote_players() const {
    std::vector<RemotePresentation> output;
    output.reserve(remotes_.size());
    double estimated_tick = static_cast<double>(latest_server_tick_);
    if (current_time_ms_ >= server_tick_received_ms_ && config_.simulation_rate != 0) {
        const std::uint64_t elapsed_ms = std::min<std::uint64_t>(current_time_ms_ - server_tick_received_ms_, 5000);
        estimated_tick += static_cast<double>(elapsed_ms) * config_.simulation_rate / 1000.0;
    }
    /* Keep six simulation ticks buffered, but advance through that buffer at
     * render time rather than holding each sample until another packet lands. */
    const double render_tick = std::max(0.0, estimated_tick - 6.0);
    for (const auto& item : remotes_) {
        const auto sampled = item.second.history.sample(render_tick, 2.0, 1.0 / config_.simulation_rate);
        if (!sampled.has_value()) continue;
        output.push_back({item.second.entity, item.second.account, item.second.zone, *sampled,
                          item.second.appearance, item.second.presentation, item.second.pattern,
                          item.second.transition_phase, item.second.transition_door,
                          item.second.transition_expires_tick});
    }
    std::sort(output.begin(), output.end(), [](const RemotePresentation& a, const RemotePresentation& b) {
        return a.account < b.account;
    });
    return output;
}

} // namespace acnet
