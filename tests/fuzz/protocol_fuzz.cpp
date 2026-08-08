#include "acnet/fragmentation.hpp"
#include "acnet/messages.hpp"
#include "acnet/protocol.hpp"
#include "acnet/replication.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::uint64_t iterations = 200000;
    std::uint64_t seed = 0xAC6C2026ULL;
    if (argc > 1) iterations = std::strtoull(argv[1], nullptr, 10);
    if (argc > 2) seed = std::strtoull(argv[2], nullptr, 10);
    std::mt19937_64 random(seed);
    std::string error;
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        const std::size_t size = static_cast<std::size_t>(random() % (acnet::kMaxPacketBytes + 64));
        std::vector<std::uint8_t> bytes(size);
        for (std::uint8_t& byte : bytes) byte = static_cast<std::uint8_t>(random());
        acnet::DecodedPacket packet;
        if (acnet::decode_packet(bytes.data(), bytes.size(), packet, error)) {
            acnet::ClientHello client_hello;
            acnet::ServerHello server_hello;
            acnet::InputCommand input;
            acnet::TransformSnapshot snapshot;
            acnet::WorldOperation world;
            acnet::EconomyRequest economy;
            acnet::Fragment fragment;
            acnet::ZoneBaseline baseline;
            std::vector<acnet::ReplicationDelta> deltas;
            (void)acnet::decode(packet.payload, client_hello);
            (void)acnet::decode(packet.payload, server_hello);
            (void)acnet::decode(packet.payload, input);
            (void)acnet::decode(packet.payload, snapshot);
            (void)acnet::decode(packet.payload, world);
            (void)acnet::decode(packet.payload, economy);
            (void)acnet::decode_fragment(packet.payload, fragment);
            (void)acnet::decode_baseline(packet.payload, baseline);
            (void)acnet::decode_deltas(packet.payload, deltas);
            /* Mod-declared calendar. Reachable from any town that installs a
             * mod, and the client indexes its string array directly, so it gets
             * the same bounded-garbage treatment as every other parser. */
            {
                acnet::ModCalendarState mod_calendar;
                (void)acnet::decode_mod_calendar(packet.payload, mod_calendar);
            }
        }
        if ((iteration % 1000) == 0) {
            acnet::ClientHello hello;
            hello.town = 1;
            hello.account = iteration + 1;
            hello.client_nonce = random();
            std::vector<std::uint8_t> payload;
            if (!acnet::encode(hello, payload)) return 2;
            acnet::PacketHeader header;
            header.message_type = acnet::MessageType::ClientHello;
            header.channel = acnet::Channel::Control;
            header.sequence = 1;
            std::vector<std::uint8_t> valid;
            if (!acnet::encode_packet(header, payload, valid, error)) return 3;
            valid[static_cast<std::size_t>(random() % valid.size())] ^= 1U << (random() % 8);
            (void)acnet::decode_packet(valid.data(), valid.size(), packet, error);
        }
    }
    std::cout << "{\"fuzz\":\"pass\",\"iterations\":" << iterations << ",\"seed\":" << seed << "}\n";
    return 0;
}
