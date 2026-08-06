#pragma once

#include "acnet/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace acnet {

class ByteWriter {
public:
    explicit ByteWriter(std::size_t limit = kMaxPayloadBytes);

    bool u8(std::uint8_t value);
    bool u16(std::uint16_t value);
    bool i16(std::int16_t value);
    bool u32(std::uint32_t value);
    bool u64(std::uint64_t value);
    bool f32(float value);
    bool bytes(const std::uint8_t* data, std::size_t size);

    bool ok() const { return ok_; }
    const std::vector<std::uint8_t>& data() const { return data_; }

private:
    bool reserve(std::size_t count);
    std::vector<std::uint8_t> data_;
    std::size_t limit_;
    bool ok_ = true;
};

class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size);
    explicit ByteReader(const std::vector<std::uint8_t>& data);

    bool u8(std::uint8_t& value);
    bool u16(std::uint16_t& value);
    bool i16(std::int16_t& value);
    bool u32(std::uint32_t& value);
    bool u64(std::uint64_t& value);
    bool f32(float& value);
    bool bytes(std::uint8_t* output, std::size_t size);

    bool finished() const { return ok_ && offset_ == size_; }
    bool ok() const { return ok_; }
    std::size_t remaining() const { return ok_ ? size_ - offset_ : 0; }

private:
    bool take(std::size_t count, const std::uint8_t*& data);
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_ = 0;
    bool ok_ = true;
};

struct DecodedPacket {
    PacketHeader header;
    std::vector<std::uint8_t> payload;
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t size);

bool encode_packet(const PacketHeader& header,
                   const std::vector<std::uint8_t>& payload,
                   std::vector<std::uint8_t>& output,
                   std::string& error);
bool decode_packet(const std::uint8_t* data,
                   std::size_t size,
                   DecodedPacket& output,
                   std::string& error);

bool encode(const ClientHello& message, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, ClientHello& message);
bool encode(const ServerHello& message, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, ServerHello& message);
bool encode(const InputCommand& message, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, InputCommand& message);
bool encode(const TransformSnapshot& message, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, TransformSnapshot& message);

} // namespace acnet

