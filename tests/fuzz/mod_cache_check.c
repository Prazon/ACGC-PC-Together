/* Checks the client-side content cache.
 *
 * The SHA-256 here is a second implementation (net/src/crypto.cpp is C++ and the
 * client links a strict subset of the netcode), so it is checked against the
 * published vectors rather than assumed correct. A wrong hash would not corrupt
 * anything -- it would silently make every cache hit a miss and every store a
 * rejection -- which is exactly the kind of failure that goes unnoticed.
 */

#include "pc_mod_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect(int condition, const char* what) {
    if (!condition) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static void hex_of(const uint8_t* hash, char* out) {
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        out[i * 2] = digits[hash[i] >> 4];
        out[i * 2 + 1] = digits[hash[i] & 0x0F];
    }
    out[64] = '\0';
}

static void check_vector(const char* input, const char* expected) {
    uint8_t hash[32];
    char hex[65];
    pc_mod_cache_sha256(input, strlen(input), hash);
    hex_of(hash, hex);
    if (strcmp(hex, expected) != 0) {
        printf("FAIL: sha256(\"%.16s\") = %s, expected %s\n", input, hex, expected);
        failures++;
    }
}

int main(void) {
    uint8_t hash[32];
    uint8_t wrong[32];
    void* loaded;
    size_t size = 0;
    const char* payload = "lantern night";
    char big[200000];

    /* Published SHA-256 vectors, including the lengths that exercise both
     * padding paths (a block that fits the length field and one that does not). */
    check_vector("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_vector("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_vector("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    /* 55 bytes: the largest input that still pads inside a single block. 56
     * would spill into a second, so these two together cover both padding
     * paths -- which is where a hand-written SHA-256 usually goes wrong. */
    check_vector("0123456789012345678901234567890123456789012345678901234",
                 "f34d5a0f80c0cbf84c8c0b90218c22637abd199965249da736a20143c8c9c9d9");
    /* 64 bytes: exactly one block, so padding adds a whole second one. */
    check_vector("0123456789012345678901234567890123456789012345678901234567890123",
                 "9674d9e078535b7cec43284387a6ee39956188e735a85452b0050b55341cda56");

    expect(pc_mod_cache_open("cache") == 1, "cache opens");

    pc_mod_cache_sha256(payload, strlen(payload), hash);
    expect(!pc_mod_cache_has(hash), "empty cache reports a miss");

    expect(pc_mod_cache_store(hash, payload, strlen(payload)) == 1, "store succeeds");
    expect(pc_mod_cache_has(hash) == 1, "stored blob is present");

    loaded = pc_mod_cache_load(hash, &size);
    expect(loaded != NULL, "stored blob loads");
    expect(size == strlen(payload), "loaded size matches");
    expect(loaded && memcmp(loaded, payload, strlen(payload)) == 0, "loaded bytes match");
    free(loaded);

    /* Content that does not match its claimed hash is refused outright. This is
     * the check that makes delivery over an untrusted link safe. */
    memcpy(wrong, hash, 32);
    wrong[0] ^= 0xFF;
    expect(pc_mod_cache_store(wrong, payload, strlen(payload)) == 0, "mismatched hash is refused");
    expect(!pc_mod_cache_has(wrong), "refused blob was not written");

    /* Storing the same thing twice is idempotent, which is what makes a
     * retried download harmless. */
    expect(pc_mod_cache_store(hash, payload, strlen(payload)) == 1, "restore is idempotent");

    /* Oversized and empty blobs are refused before any file work. */
    expect(pc_mod_cache_store(hash, payload, 0) == 0, "empty blob refused");
    expect(pc_mod_cache_store(hash, big, sizeof(big)) == 0, "wrong-size blob refused");

    /* A blob corrupted on disk after storing must be rejected on read, not
     * handed to the parser. */
    {
        char path[512];
        char hex[65];
        FILE* file;
        hex_of(hash, hex);
        snprintf(path, sizeof(path), "cache/%c%c/%s", hex[0], hex[1], hex);
        file = fopen(path, "r+b");
        expect(file != NULL, "corrupt-test can open the blob");
        if (file) {
            fputc('X', file);
            fclose(file);
        }
        loaded = pc_mod_cache_load(hash, &size);
        expect(loaded == NULL, "corrupted blob is rejected on read");
        free(loaded);
        expect(!pc_mod_cache_has(hash), "corrupted blob is dropped so it refetches");
    }

    /* A cache that cannot be opened degrades to no cache rather than failing. */
    pc_mod_cache_close();
    expect(pc_mod_cache_open("/proc/definitely/not/writable") == 0, "unusable directory refuses");
    expect(pc_mod_cache_has(hash) == 0, "closed cache reports misses");
    expect(pc_mod_cache_store(hash, payload, strlen(payload)) == 0, "closed cache refuses stores");

    printf(failures == 0 ? "mod_cache: PASS\n" : "mod_cache: %d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
