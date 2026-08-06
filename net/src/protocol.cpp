#include "acnet/protocol.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace acnet {
namespace {

constexpr std::size_t kHeaderBytes = 36;

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    append_u32(out, static_cast<std::uint32_t>(value >> 32));
    append_u32(out, static_cast<std::uint32_t>(value));
}

bool valid_channel(std::uint8_t value) {
    return value < static_cast<std::uint8_t>(Channel::Count);
}

bool valid_message(std::uint16_t value) {
    switch (static_cast<MessageType>(value)) {
        case MessageType::ClientHello:
        case MessageType::ServerHello:
        case MessageType::Disconnect:
        case MessageType::Ping:
        case MessageType::Pong:
        case MessageType::TownBootstrap:
        case MessageType::TownBootstrapResult:
        case MessageType::InputCommand:
        case MessageType::TransformSnapshot:
        case MessageType::Baseline:
        case MessageType::ReplicationDeltas:
        case MessageType::WorldRequest:
        case MessageType::WorldResult:
        case MessageType::InventoryRequest:
        case MessageType::InventoryResult:
        case MessageType::TradeRequest:
        case MessageType::TradeResult:
        case MessageType::FurnitureRequest:
        case MessageType::FurnitureResult:
        case MessageType::HouseUpdate:
        case MessageType::HouseUpdateResult:
        case MessageType::ConversationRequest:
        case MessageType::ConversationResult:
        case MessageType::EncounterRequest:
        case MessageType::EncounterResult:
        case MessageType::ZoneTransferRequest:
        case MessageType::ZoneTransferOffer:
        case MessageType::ZoneReady:
        case MessageType::AdminCommand:
        case MessageType::AdminResult:
            return true;
    }
    return false;
}

bool encode_transform(ByteWriter& writer, const Transform& transform) {
    return writer.f32(transform.position.x) && writer.f32(transform.position.y) &&
           writer.f32(transform.position.z) && writer.f32(transform.velocity.x) &&
           writer.f32(transform.velocity.y) && writer.f32(transform.velocity.z) &&
           writer.i16(transform.yaw) && writer.u16(transform.action);
}

bool decode_transform(ByteReader& reader, Transform& transform) {
    if (!reader.f32(transform.position.x) || !reader.f32(transform.position.y) ||
        !reader.f32(transform.position.z) || !reader.f32(transform.velocity.x) ||
        !reader.f32(transform.velocity.y) || !reader.f32(transform.velocity.z) ||
        !reader.i16(transform.yaw) || !reader.u16(transform.action)) {
        return false;
    }
    return finite(transform.position) && finite(transform.velocity);
}

bool encode_appearance(ByteWriter& writer, const PlayerAppearance& appearance) {
    return appearance.gender <= 2 && appearance.face < 8 &&
           writer.bytes(appearance.name.data(), appearance.name.size()) && writer.u8(appearance.gender) &&
           writer.u8(appearance.face) && writer.u16(appearance.clothing) && writer.u16(appearance.equipped_item);
}

bool decode_appearance(ByteReader& reader, PlayerAppearance& appearance) {
    return reader.bytes(appearance.name.data(), appearance.name.size()) && reader.u8(appearance.gender) &&
           reader.u8(appearance.face) && reader.u16(appearance.clothing) &&
           reader.u16(appearance.equipped_item) && appearance.gender <= 2 && appearance.face < 8;
}

} // namespace

Delivery delivery_for(Channel channel) {
    switch (channel) {
        case Channel::Snapshots:
            return Delivery::UnreliableSequenced;
        case Channel::Events:
            return Delivery::ReliableUnordered;
        case Channel::Control:
        case Channel::Transactions:
        case Channel::Chat:
        case Channel::Bulk:
        case Channel::Count:
            return Delivery::ReliableOrdered;
    }
    return Delivery::ReliableOrdered;
}

bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

ByteWriter::ByteWriter(std::size_t limit) : limit_(limit) {
    data_.reserve(limit < 256 ? limit : 256);
}

bool ByteWriter::reserve(std::size_t count) {
    if (!ok_ || count > limit_ || data_.size() > limit_ - count) {
        ok_ = false;
        return false;
    }
    return true;
}

bool ByteWriter::u8(std::uint8_t value) {
    if (!reserve(1)) return false;
    data_.push_back(value);
    return true;
}

bool ByteWriter::u16(std::uint16_t value) {
    if (!reserve(2)) return false;
    append_u16(data_, value);
    return true;
}

bool ByteWriter::i16(std::int16_t value) {
    return u16(static_cast<std::uint16_t>(value));
}

bool ByteWriter::u32(std::uint32_t value) {
    if (!reserve(4)) return false;
    append_u32(data_, value);
    return true;
}

bool ByteWriter::u64(std::uint64_t value) {
    if (!reserve(8)) return false;
    append_u64(data_, value);
    return true;
}

bool ByteWriter::f32(float value) {
    static_assert(sizeof(float) == sizeof(std::uint32_t), "32-bit float required");
    if (!std::isfinite(value)) {
        ok_ = false;
        return false;
    }
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return u32(bits);
}

bool ByteWriter::bytes(const std::uint8_t* data, std::size_t size) {
    if ((data == nullptr && size != 0) || !reserve(size)) return false;
    data_.insert(data_.end(), data, data + size);
    return true;
}

ByteReader::ByteReader(const std::uint8_t* data, std::size_t size)
    : data_(data), size_(size), ok_(data != nullptr || size == 0) {}

ByteReader::ByteReader(const std::vector<std::uint8_t>& data)
    : ByteReader(data.data(), data.size()) {}

bool ByteReader::take(std::size_t count, const std::uint8_t*& data) {
    if (!ok_ || count > size_ || offset_ > size_ - count) {
        ok_ = false;
        return false;
    }
    data = data_ + offset_;
    offset_ += count;
    return true;
}

bool ByteReader::u8(std::uint8_t& value) {
    const std::uint8_t* p;
    if (!take(1, p)) return false;
    value = p[0];
    return true;
}

bool ByteReader::u16(std::uint16_t& value) {
    const std::uint8_t* p;
    if (!take(2, p)) return false;
    value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
    return true;
}

bool ByteReader::i16(std::int16_t& value) {
    std::uint16_t raw;
    if (!u16(raw)) return false;
    value = static_cast<std::int16_t>(raw);
    return true;
}

bool ByteReader::u32(std::uint32_t& value) {
    const std::uint8_t* p;
    if (!take(4, p)) return false;
    value = (static_cast<std::uint32_t>(p[0]) << 24) |
            (static_cast<std::uint32_t>(p[1]) << 16) |
            (static_cast<std::uint32_t>(p[2]) << 8) |
            static_cast<std::uint32_t>(p[3]);
    return true;
}

bool ByteReader::u64(std::uint64_t& value) {
    std::uint32_t high;
    std::uint32_t low;
    if (!u32(high) || !u32(low)) return false;
    value = (static_cast<std::uint64_t>(high) << 32) | low;
    return true;
}

bool ByteReader::f32(float& value) {
    std::uint32_t bits;
    if (!u32(bits)) return false;
    std::memcpy(&value, &bits, sizeof(value));
    if (!std::isfinite(value)) {
        ok_ = false;
        return false;
    }
    return true;
}

bool ByteReader::bytes(std::uint8_t* output, std::size_t size) {
    const std::uint8_t* p;
    if ((output == nullptr && size != 0) || !take(size, p)) return false;
    if (size != 0) std::memcpy(output, p, size);
    return true;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

bool encode_packet(const PacketHeader& header,
                   const std::vector<std::uint8_t>& payload,
                   std::vector<std::uint8_t>& output,
                   std::string& error) {
    if (payload.size() > kMaxPayloadBytes || payload.size() > std::numeric_limits<std::uint16_t>::max()) {
        error = "payload too large";
        return false;
    }
    if (header.channel == Channel::Count) {
        error = "invalid channel";
        return false;
    }
    output.clear();
    output.reserve(kHeaderBytes + payload.size());
    append_u32(output, kWireMagic);
    append_u16(output, header.protocol_version);
    append_u16(output, static_cast<std::uint16_t>(header.message_type));
    output.push_back(static_cast<std::uint8_t>(header.channel));
    output.push_back(header.flags);
    append_u16(output, 0);
    append_u64(output, header.session);
    append_u32(output, header.sequence);
    append_u32(output, header.acknowledged_sequence);
    append_u32(output, header.acknowledged_bits);
    append_u16(output, static_cast<std::uint16_t>(payload.size()));
    append_u16(output, 0);
    output.insert(output.end(), payload.begin(), payload.end());
    const std::uint32_t checksum = crc32(output.data(), output.size());
    append_u32(output, checksum);
    if (output.size() > kMaxPacketBytes) {
        error = "packet exceeds datagram limit";
        output.clear();
        return false;
    }
    return true;
}

bool decode_packet(const std::uint8_t* data,
                   std::size_t size,
                   DecodedPacket& output,
                   std::string& error) {
    if (data == nullptr || size < kHeaderBytes + 4 || size > kMaxPacketBytes) {
        error = "invalid packet size";
        return false;
    }
    const std::uint32_t expected_crc = crc32(data, size - 4);
    ByteReader reader(data, size);
    std::uint32_t magic;
    std::uint16_t type;
    std::uint8_t channel;
    std::uint16_t reserved;
    std::uint16_t trailing_reserved;
    std::uint32_t actual_crc;
    PacketHeader header;
    if (!reader.u32(magic) || !reader.u16(header.protocol_version) || !reader.u16(type) ||
        !reader.u8(channel) || !reader.u8(header.flags) || !reader.u16(reserved) ||
        !reader.u64(header.session) || !reader.u32(header.sequence) ||
        !reader.u32(header.acknowledged_sequence) || !reader.u32(header.acknowledged_bits) ||
        !reader.u16(header.payload_size) || !reader.u16(trailing_reserved)) {
        error = "truncated header";
        return false;
    }
    constexpr std::uint8_t valid_flags = PacketReliable | PacketFragment | PacketEncrypted | PacketAckOnly;
    if (magic != kWireMagic || reserved != 0 || trailing_reserved != 0 ||
        (header.flags & ~valid_flags) != 0 ||
        !valid_channel(channel) || !valid_message(type)) {
        error = "invalid header";
        return false;
    }
    if (header.payload_size > kMaxPayloadBytes || size != kHeaderBytes + header.payload_size + 4) {
        error = "payload length mismatch";
        return false;
    }
    output.payload.resize(header.payload_size);
    if (!reader.bytes(output.payload.data(), output.payload.size()) || !reader.u32(actual_crc) || !reader.finished()) {
        error = "truncated payload";
        return false;
    }
    if (actual_crc != expected_crc) {
        error = "checksum mismatch";
        return false;
    }
    header.message_type = static_cast<MessageType>(type);
    header.channel = static_cast<Channel>(channel);
    output.header = header;
    return true;
}

bool encode(const ClientHello& message, std::vector<std::uint8_t>& output) {
    if (message.reconnect_token_size > message.reconnect_token.size()) return false;
    ByteWriter w;
    if (!w.u16(message.minimum_version) || !w.u16(message.maximum_version) ||
        !w.u64(message.build_id) || !w.u64(message.feature_flags) || !w.u64(message.town) ||
        !w.u64(message.account) || !w.u64(message.client_nonce) ||
        !w.u8(message.reconnect_token_size) ||
        !w.bytes(message.reconnect_token.data(), message.reconnect_token_size) ||
        !w.bytes(message.invite_proof.data(), message.invite_proof.size())) return false;
    output = w.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, ClientHello& message) {
    ByteReader r(input);
    if (!r.u16(message.minimum_version) || !r.u16(message.maximum_version) ||
        !r.u64(message.build_id) || !r.u64(message.feature_flags) || !r.u64(message.town) ||
        !r.u64(message.account) || !r.u64(message.client_nonce) ||
        !r.u8(message.reconnect_token_size) ||
        message.reconnect_token_size > message.reconnect_token.size() ||
        !r.bytes(message.reconnect_token.data(), message.reconnect_token_size) ||
        !r.bytes(message.invite_proof.data(), message.invite_proof.size())) return false;
    return r.finished() && message.minimum_version <= message.maximum_version;
}

bool encode(const ServerHello& message, std::vector<std::uint8_t>& output) {
    if (message.reconnect_token_size > message.reconnect_token.size() || message.town_seed == 0 ||
        (message.town_land_id & 0xFF00U) != 0x3000U ||
        (message.resident_slot >= 4 && message.resident_slot != 0xFF)) return false;
    ByteWriter w;
    if (!w.u16(static_cast<std::uint16_t>(message.result)) ||
        !w.u16(message.negotiated_version) || !w.u64(message.session) ||
        !w.u64(message.player_entity) || !w.u32(message.server_tick) ||
        !w.u64(message.server_nonce) || !w.u32(message.town_seed) || !w.u16(message.town_land_id) ||
        !w.u8(message.resident_slot) || !w.u8(message.town_initialized ? 1 : 0) ||
        !w.bytes(message.town_name.data(), message.town_name.size()) || !w.u8(message.reconnect_token_size) ||
        !w.bytes(message.reconnect_token.data(), message.reconnect_token_size) ||
        !w.bytes(message.server_proof.data(), message.server_proof.size())) return false;
    output = w.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, ServerHello& message) {
    ByteReader r(input);
    std::uint16_t result;
    std::uint8_t town_initialized;
    if (!r.u16(result) || !r.u16(message.negotiated_version) || !r.u64(message.session) ||
        !r.u64(message.player_entity) || !r.u32(message.server_tick) ||
        !r.u64(message.server_nonce) || !r.u32(message.town_seed) || !r.u16(message.town_land_id) ||
        !r.u8(message.resident_slot) || !r.u8(town_initialized) || town_initialized > 1 ||
        !r.bytes(message.town_name.data(), message.town_name.size()) || !r.u8(message.reconnect_token_size) ||
        message.reconnect_token_size > message.reconnect_token.size() ||
        !r.bytes(message.reconnect_token.data(), message.reconnect_token_size) ||
        !r.bytes(message.server_proof.data(), message.server_proof.size()) ||
        result > static_cast<std::uint16_t>(ResultCode::InternalError) || message.town_seed == 0 ||
        (message.town_land_id & 0xFF00U) != 0x3000U ||
        (message.resident_slot >= 4 && message.resident_slot != 0xFF)) return false;
    message.result = static_cast<ResultCode>(result);
    message.town_initialized = town_initialized != 0;
    return r.finished();
}

bool encode(const InputCommand& message, std::vector<std::uint8_t>& output) {
    ByteWriter w;
    if (!w.u32(message.sequence) || !w.u32(message.estimated_server_tick) ||
        !w.i16(message.stick_x) || !w.i16(message.stick_y) || !w.u16(message.buttons) ||
        !w.u16(message.action) || !encode_transform(w, message.client_transform)) return false;
    output = w.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, InputCommand& message) {
    ByteReader r(input);
    if (!r.u32(message.sequence) || !r.u32(message.estimated_server_tick) ||
        !r.i16(message.stick_x) || !r.i16(message.stick_y) || !r.u16(message.buttons) ||
        !r.u16(message.action) || !decode_transform(r, message.client_transform)) return false;
    return r.finished();
}

bool encode(const TransformSnapshot& message, std::vector<std::uint8_t>& output) {
    if (message.players.size() > kMaxPlayersPerZone || (message.occupied_house_mask & 0xF0U) != 0) return false;
    ByteWriter w;
    if (!w.u32(message.server_tick) || !w.u32(message.baseline_revision) ||
        !w.u8(message.occupied_house_mask) ||
        !w.u8(static_cast<std::uint8_t>(message.players.size()))) return false;
    for (const PlayerSnapshot& player : message.players) {
        if (static_cast<std::uint8_t>(player.transition_phase) >
            static_cast<std::uint8_t>(DoorTransitionPhase::Arriving)) return false;
        if (!w.u64(player.entity) || !w.u64(player.account) || !w.u32(player.zone) ||
            !w.u32(player.acknowledged_input) || !encode_transform(w, player.transform) ||
            !encode_appearance(w, player.appearance) ||
            !w.u8(static_cast<std::uint8_t>(player.transition_phase)) ||
            !w.u32(player.transition_door) || !w.u32(player.transition_expires_tick)) return false;
    }
    output = w.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, TransformSnapshot& message) {
    ByteReader r(input);
    std::uint8_t count;
    if (!r.u32(message.server_tick) || !r.u32(message.baseline_revision) ||
        !r.u8(message.occupied_house_mask) || !r.u8(count) || count > kMaxPlayersPerZone ||
        (message.occupied_house_mask & 0xF0U) != 0) return false;
    message.players.clear();
    message.players.reserve(count);
    for (std::uint8_t i = 0; i < count; ++i) {
        PlayerSnapshot player;
        std::uint8_t transition_phase;
        if (!r.u64(player.entity) || !r.u64(player.account) || !r.u32(player.zone) ||
            !r.u32(player.acknowledged_input) || !decode_transform(r, player.transform) ||
            !decode_appearance(r, player.appearance) || !r.u8(transition_phase) ||
            !r.u32(player.transition_door) || !r.u32(player.transition_expires_tick)) return false;
        if (player.entity == 0 || player.account == 0 || player.zone == 0 ||
            transition_phase > static_cast<std::uint8_t>(DoorTransitionPhase::Arriving) ||
            (transition_phase == 0 && (player.transition_door != 0 || player.transition_expires_tick != 0)) ||
            (transition_phase != 0 && (player.transition_door == 0 || player.transition_expires_tick == 0))) return false;
        player.transition_phase = static_cast<DoorTransitionPhase>(transition_phase);
        message.players.push_back(player);
    }
    return r.finished();
}

} // namespace acnet
