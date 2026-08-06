#include "acnet/crypto.hpp"

#include "acnet/protocol.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <sys/random.h>
#endif

namespace acnet {
namespace {

std::uint32_t rotate_right(std::uint32_t value, unsigned count) {
    return (value >> count) | (value << (32 - count));
}

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8)); out.push_back(static_cast<std::uint8_t>(value));
}
void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24)); out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8)); out.push_back(static_cast<std::uint8_t>(value));
}
void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    append_u32(out, static_cast<std::uint32_t>(value >> 32)); append_u32(out, static_cast<std::uint32_t>(value));
}

std::vector<std::uint8_t> string_key(const std::string& value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

std::vector<std::uint8_t> client_proof_bytes(const ClientHello& hello) {
    std::vector<std::uint8_t> bytes;
    append_u16(bytes, hello.minimum_version); append_u16(bytes, hello.maximum_version);
    append_u64(bytes, hello.build_id); append_u64(bytes, hello.feature_flags);
    append_u64(bytes, hello.town); append_u64(bytes, hello.account); append_u64(bytes, hello.client_nonce);
    bytes.push_back(hello.reconnect_token_size);
    bytes.insert(bytes.end(), hello.reconnect_token.begin(),
                 hello.reconnect_token.begin() + hello.reconnect_token_size);
    return bytes;
}

std::vector<std::uint8_t> server_proof_bytes(const ServerHello& hello, std::uint64_t client_nonce) {
    std::vector<std::uint8_t> bytes;
    append_u16(bytes, static_cast<std::uint16_t>(hello.result)); append_u16(bytes, hello.negotiated_version);
    append_u64(bytes, hello.session); append_u64(bytes, hello.player_entity); append_u32(bytes, hello.server_tick);
    append_u64(bytes, client_nonce); append_u64(bytes, hello.server_nonce);
    append_u32(bytes, hello.town_seed); append_u16(bytes, hello.town_land_id);
    bytes.push_back(hello.resident_slot); bytes.push_back(hello.town_initialized ? 1 : 0);
    bytes.insert(bytes.end(), hello.town_name.begin(), hello.town_name.end());
    bytes.push_back(hello.reconnect_token_size);
    bytes.insert(bytes.end(), hello.reconnect_token.begin(), hello.reconnect_token.begin() + hello.reconnect_token_size);
    return bytes;
}

std::uint32_t load32_le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
void store32_le(std::uint8_t* p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value); p[1] = static_cast<std::uint8_t>(value >> 8);
    p[2] = static_cast<std::uint8_t>(value >> 16); p[3] = static_cast<std::uint8_t>(value >> 24);
}
std::uint32_t rotate_left(std::uint32_t value, unsigned count) {
    return (value << count) | (value >> (32 - count));
}
void quarter(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c, std::uint32_t& d) {
    a += b; d ^= a; d = rotate_left(d, 16); c += d; b ^= c; b = rotate_left(b, 12);
    a += b; d ^= a; d = rotate_left(d, 8); c += d; b ^= c; b = rotate_left(b, 7);
}

void chacha20(const CryptoKey& key, const std::array<std::uint8_t, 12>& nonce,
              const std::uint8_t* input, std::uint8_t* output, std::size_t size) {
    static constexpr std::array<std::uint32_t, 4> constants{{0x61707865, 0x3320646e, 0x79622d32, 0x6b206574}};
    std::uint32_t counter = 1;
    std::size_t offset = 0;
    while (offset < size) {
        std::array<std::uint32_t, 16> state{};
        for (std::size_t i = 0; i < 4; ++i) state[i] = constants[i];
        for (std::size_t i = 0; i < 8; ++i) state[4 + i] = load32_le(key.data() + i * 4);
        state[12] = counter++;
        state[13] = load32_le(nonce.data()); state[14] = load32_le(nonce.data() + 4);
        state[15] = load32_le(nonce.data() + 8);
        auto working = state;
        for (int round = 0; round < 10; ++round) {
            quarter(working[0], working[4], working[8], working[12]);
            quarter(working[1], working[5], working[9], working[13]);
            quarter(working[2], working[6], working[10], working[14]);
            quarter(working[3], working[7], working[11], working[15]);
            quarter(working[0], working[5], working[10], working[15]);
            quarter(working[1], working[6], working[11], working[12]);
            quarter(working[2], working[7], working[8], working[13]);
            quarter(working[3], working[4], working[9], working[14]);
        }
        std::array<std::uint8_t, 64> block{};
        for (std::size_t i = 0; i < 16; ++i) store32_le(block.data() + i * 4, working[i] + state[i]);
        const std::size_t count = std::min<std::size_t>(64, size - offset);
        for (std::size_t i = 0; i < count; ++i) output[offset + i] = input[offset + i] ^ block[i];
        offset += count;
    }
}

std::array<std::uint8_t, 12> packet_nonce(const PacketHeader& header) {
    std::array<std::uint8_t, 12> nonce{};
    for (int i = 0; i < 8; ++i) nonce[i] = static_cast<std::uint8_t>(header.session >> (i * 8));
    nonce[0] ^= static_cast<std::uint8_t>(header.channel);
    store32_le(nonce.data() + 8, header.sequence);
    return nonce;
}

std::vector<std::uint8_t> authenticated_bytes(const PacketHeader& header,
                                              const std::vector<std::uint8_t>& ciphertext) {
    std::vector<std::uint8_t> bytes;
    append_u16(bytes, header.protocol_version); append_u16(bytes, static_cast<std::uint16_t>(header.message_type));
    bytes.push_back(static_cast<std::uint8_t>(header.channel)); bytes.push_back(header.flags);
    append_u64(bytes, header.session); append_u32(bytes, header.sequence);
    append_u32(bytes, header.acknowledged_sequence); append_u32(bytes, header.acknowledged_bits);
    bytes.insert(bytes.end(), ciphertext.begin(), ciphertext.end());
    return bytes;
}

} // namespace

bool secure_random(std::uint8_t* output, std::size_t size) {
    if (output == nullptr && size != 0) return false;
#ifdef _WIN32
    return BCryptGenRandom(nullptr, output, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t result = getrandom(output + offset, size - offset, 0);
        if (result > 0) { offset += static_cast<std::size_t>(result); continue; }
        if (result < 0 && errno == EINTR) continue;
        break;
    }
    if (offset == size) return true;
    std::ifstream stream("/dev/urandom", std::ios::binary);
    stream.read(reinterpret_cast<char*>(output + offset), static_cast<std::streamsize>(size - offset));
    return stream.good();
#endif
}

std::array<std::uint8_t, 32> sha256(const std::uint8_t* data, std::size_t size) {
    static constexpr std::array<std::uint32_t, 64> k{{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2}};
    std::vector<std::uint8_t> message;
    if (size != 0) message.assign(data, data + size);
    message.push_back(0x80);
    while ((message.size() % 64) != 56) message.push_back(0);
    const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8;
    for (int i = 7; i >= 0; --i) message.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
    std::array<std::uint32_t, 8> h{{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::uint8_t* p = message.data() + offset + i * 4;
            w[i] = (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotate_right(w[i-15],7) ^ rotate_right(w[i-15],18) ^ (w[i-15] >> 3);
            const std::uint32_t s1 = rotate_right(w[i-2],17) ^ rotate_right(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        std::uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1=rotate_right(e,6)^rotate_right(e,11)^rotate_right(e,25);
            const std::uint32_t ch=(e&f)^((~e)&g); const std::uint32_t t1=hh+s1+ch+k[i]+w[i];
            const std::uint32_t s0=rotate_right(a,2)^rotate_right(a,13)^rotate_right(a,22);
            const std::uint32_t maj=(a&b)^(a&c)^(b&c); const std::uint32_t t2=s0+maj;
            hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < h.size(); ++i) {
        digest[i*4]=static_cast<std::uint8_t>(h[i]>>24); digest[i*4+1]=static_cast<std::uint8_t>(h[i]>>16);
        digest[i*4+2]=static_cast<std::uint8_t>(h[i]>>8); digest[i*4+3]=static_cast<std::uint8_t>(h[i]);
    }
    return digest;
}

CryptoKey hmac_sha256(const std::vector<std::uint8_t>& key, const std::uint8_t* data, std::size_t size) {
    std::array<std::uint8_t, 64> block{};
    if (key.size() > block.size()) {
        const auto hashed = sha256(key.data(), key.size()); std::copy(hashed.begin(), hashed.end(), block.begin());
    } else std::copy(key.begin(), key.end(), block.begin());
    std::vector<std::uint8_t> inner(64 + size);
    for (std::size_t i = 0; i < 64; ++i) inner[i] = block[i] ^ 0x36;
    if (size != 0) std::memcpy(inner.data() + 64, data, size);
    const auto inner_hash = sha256(inner.data(), inner.size());
    std::array<std::uint8_t, 96> outer{};
    for (std::size_t i = 0; i < 64; ++i) outer[i] = block[i] ^ 0x5c;
    std::copy(inner_hash.begin(), inner_hash.end(), outer.begin() + 64);
    return sha256(outer.data(), outer.size());
}

bool constant_time_equal(const std::uint8_t* first, const std::uint8_t* second, std::size_t size) {
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < size; ++i) difference |= static_cast<std::uint8_t>(first[i] ^ second[i]);
    return difference == 0;
}

std::array<std::uint8_t, 32> invite_proof(const ClientHello& hello, const std::string& key) {
    const auto bytes = client_proof_bytes(hello); return hmac_sha256(string_key(key), bytes.data(), bytes.size());
}
std::array<std::uint8_t, 32> server_proof(const ServerHello& hello, std::uint64_t nonce, const std::string& key) {
    const auto bytes = server_proof_bytes(hello, nonce); return hmac_sha256(string_key(key), bytes.data(), bytes.size());
}

SessionKeys derive_session_keys(const ClientHello& client, const ServerHello& server, const std::string& key) {
    std::vector<std::uint8_t> seed;
    append_u64(seed, client.town); append_u64(seed, client.account); append_u64(seed, client.client_nonce);
    append_u64(seed, server.server_nonce); append_u64(seed, server.session);
    const CryptoKey master = hmac_sha256(string_key(key), seed.data(), seed.size());
    const std::vector<std::uint8_t> master_key(master.begin(), master.end());
    static const std::uint8_t c2s[] = {'c','l','i','e','n','t','-','t','o','-','s','e','r','v','e','r'};
    static const std::uint8_t s2c[] = {'s','e','r','v','e','r','-','t','o','-','c','l','i','e','n','t'};
    return {hmac_sha256(master_key, c2s, sizeof(c2s)), hmac_sha256(master_key, s2c, sizeof(s2c))};
}

bool seal_payload(const PacketHeader& header, const CryptoKey& key,
                  const std::vector<std::uint8_t>& plaintext, std::vector<std::uint8_t>& sealed) {
    if ((header.flags & PacketEncrypted) == 0 || plaintext.size() > kMaxPlaintextPayloadBytes) return false;
    sealed.resize(plaintext.size() + kEncryptionTagBytes);
    chacha20(key, packet_nonce(header), plaintext.data(), sealed.data(), plaintext.size());
    std::vector<std::uint8_t> ciphertext(sealed.begin(), sealed.begin() + static_cast<std::ptrdiff_t>(plaintext.size()));
    const auto auth = authenticated_bytes(header, ciphertext);
    const CryptoKey tag = hmac_sha256(std::vector<std::uint8_t>(key.begin(), key.end()), auth.data(), auth.size());
    std::copy(tag.begin(), tag.begin() + kEncryptionTagBytes, sealed.begin() + static_cast<std::ptrdiff_t>(plaintext.size()));
    return true;
}

bool open_payload(const PacketHeader& header, const CryptoKey& key,
                  const std::vector<std::uint8_t>& sealed, std::vector<std::uint8_t>& plaintext) {
    if ((header.flags & PacketEncrypted) == 0 || sealed.size() < kEncryptionTagBytes || sealed.size() > kMaxPayloadBytes) return false;
    const std::size_t size = sealed.size() - kEncryptionTagBytes;
    std::vector<std::uint8_t> ciphertext(sealed.begin(), sealed.begin() + static_cast<std::ptrdiff_t>(size));
    const auto auth = authenticated_bytes(header, ciphertext);
    const CryptoKey tag = hmac_sha256(std::vector<std::uint8_t>(key.begin(), key.end()), auth.data(), auth.size());
    if (!constant_time_equal(tag.data(), sealed.data() + size, kEncryptionTagBytes)) return false;
    plaintext.resize(size);
    chacha20(key, packet_nonce(header), ciphertext.data(), plaintext.data(), size);
    return true;
}

} // namespace acnet
