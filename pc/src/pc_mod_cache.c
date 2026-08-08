/* pc_mod_cache.c - see pc_mod_cache.h */

#include "pc_mod_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define pc_mkdir(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define pc_mkdir(path) mkdir((path), 0755)
#endif

#define PC_MOD_CACHE_MAX_PATH 512

static char g_root[PC_MOD_CACHE_MAX_PATH];
static int g_open = 0;

/* --- SHA-256 -------------------------------------------------------------
 * Self-contained rather than shared with net/src/crypto.cpp: that is C++ and
 * the client's pc/ layer links a strict subset of the netcode. A second
 * implementation is a real cost, so it is checked against known vectors in the
 * test rather than assumed correct. */

static uint32_t rotate_right(uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

static void sha256_block(uint32_t state[8], const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
        0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
        0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
        0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
        0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
        0xc67178f2u,
    };
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        const uint32_t s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 64; i++) {
        const uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + s1 + ch + k[i] + w[i];
        const uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void pc_mod_cache_sha256(const void* data, size_t size, uint8_t out[PC_MOD_CACHE_HASH_BYTES]) {
    uint32_t state[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    const uint8_t* bytes = (const uint8_t*)data;
    uint8_t tail[128];
    size_t remaining = size;
    size_t tail_len;
    uint64_t bits = (uint64_t)size * 8u;
    int i;

    while (remaining >= 64) {
        sha256_block(state, bytes);
        bytes += 64;
        remaining -= 64;
    }

    memset(tail, 0, sizeof(tail));
    if (remaining > 0) memcpy(tail, bytes, remaining);
    tail[remaining] = 0x80;
    tail_len = (remaining < 56) ? 64 : 128;
    for (i = 0; i < 8; i++) {
        tail[tail_len - 1 - (size_t)i] = (uint8_t)((bits >> (i * 8)) & 0xFFu);
    }
    sha256_block(state, tail);
    if (tail_len == 128) sha256_block(state, tail + 64);

    for (i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)state[i];
    }
}

/* --- Store --------------------------------------------------------------- */

static void hex_of(const uint8_t hash[PC_MOD_CACHE_HASH_BYTES], char out[65]) {
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 0; i < PC_MOD_CACHE_HASH_BYTES; i++) {
        out[i * 2] = digits[hash[i] >> 4];
        out[i * 2 + 1] = digits[hash[i] & 0x0F];
    }
    out[64] = '\0';
}

static int blob_path(const uint8_t hash[PC_MOD_CACHE_HASH_BYTES], char* out, size_t out_len,
                     int make_shard) {
    char hex[65];
    char shard[PC_MOD_CACHE_MAX_PATH];
    hex_of(hash, hex);
    if (snprintf(shard, sizeof(shard), "%s/%c%c", g_root, hex[0], hex[1]) >= (int)sizeof(shard))
        return 0;
    if (make_shard) pc_mkdir(shard);
    return snprintf(out, out_len, "%s/%s", shard, hex) < (int)out_len;
}

int pc_mod_cache_open(const char* directory) {
    if (!directory || !*directory) return 0;
    if (snprintf(g_root, sizeof(g_root), "%s", directory) >= (int)sizeof(g_root)) return 0;
    pc_mkdir(g_root);
    {
        /* Prove it is usable rather than assuming mkdir succeeded: a
         * read-only or missing parent must degrade to "no cache", not to a
         * failure at the first store. */
        char probe[PC_MOD_CACHE_MAX_PATH];
        FILE* file;
        if (snprintf(probe, sizeof(probe), "%s/.probe", g_root) >= (int)sizeof(probe)) return 0;
        file = fopen(probe, "wb");
        if (!file) { g_open = 0; return 0; }
        fclose(file);
        remove(probe);
    }
    g_open = 1;
    return 1;
}

int pc_mod_cache_has(const uint8_t hash[PC_MOD_CACHE_HASH_BYTES]) {
    char path[PC_MOD_CACHE_MAX_PATH];
    FILE* file;
    if (!g_open || !hash) return 0;
    if (!blob_path(hash, path, sizeof(path), 0)) return 0;
    file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}

int pc_mod_cache_store(const uint8_t hash[PC_MOD_CACHE_HASH_BYTES], const void* data, size_t size) {
    char path[PC_MOD_CACHE_MAX_PATH];
    char temp[PC_MOD_CACHE_MAX_PATH];
    uint8_t actual[PC_MOD_CACHE_HASH_BYTES];
    FILE* file;

    if (!g_open || !hash || !data || size == 0 || size > PC_MOD_CACHE_MAX_BLOB) return 0;

    /* Verify before storing. This is the check that makes delivery over an
     * untrusted link safe: what is written is what was advertised, or nothing
     * is written at all. */
    pc_mod_cache_sha256(data, size, actual);
    if (memcmp(actual, hash, PC_MOD_CACHE_HASH_BYTES) != 0) return 0;

    if (!blob_path(hash, path, sizeof(path), 1)) return 0;
    /* Written to a temporary and renamed, so a crash mid-write cannot leave a
     * truncated blob under a name that says it is complete. */
    if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp)) return 0;
    file = fopen(temp, "wb");
    if (!file) return 0;
    if (fwrite(data, 1, size, file) != size) {
        fclose(file);
        remove(temp);
        return 0;
    }
    fclose(file);
    remove(path);              /* rename over an existing file fails on Windows */
    if (rename(temp, path) != 0) {
        remove(temp);
        return 0;
    }
    return 1;
}

void* pc_mod_cache_load(const uint8_t hash[PC_MOD_CACHE_HASH_BYTES], size_t* size_out) {
    char path[PC_MOD_CACHE_MAX_PATH];
    FILE* file;
    long size;
    void* buffer;
    uint8_t actual[PC_MOD_CACHE_HASH_BYTES];

    if (size_out) *size_out = 0;
    if (!g_open || !hash) return NULL;
    if (!blob_path(hash, path, sizeof(path), 0)) return NULL;

    file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0 || (unsigned long)size > PC_MOD_CACHE_MAX_BLOB) { fclose(file); return NULL; }

    buffer = malloc((size_t)size);
    if (!buffer) { fclose(file); return NULL; }
    if (fread(buffer, 1, (size_t)size, file) != (size_t)size) {
        free(buffer);
        fclose(file);
        return NULL;
    }
    fclose(file);

    /* Re-verified on read. A blob corrupted on disk after it was stored is
     * rejected here rather than handed to the .pcasset parser. */
    pc_mod_cache_sha256(buffer, (size_t)size, actual);
    if (memcmp(actual, hash, PC_MOD_CACHE_HASH_BYTES) != 0) {
        free(buffer);
        remove(path);          /* drop it so the next join refetches */
        return NULL;
    }

    if (size_out) *size_out = (size_t)size;
    return buffer;
}

void pc_mod_cache_close(void) {
    g_open = 0;
    g_root[0] = '\0';
}
