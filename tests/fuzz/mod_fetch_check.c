/* Checks the client fetch loop against a simulated server.
 *
 * The loop is the piece that decides what a player waits for and what they
 * never get. Its failure modes are all "quietly wrong" rather than loud: an
 * asset silently never requested, progress that can never reach its total, a
 * corrupted blob accepted. So each of those is asserted here explicitly.
 */

#include "pc_mod_cache.h"
#include "pc_mod_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK_BYTES (1136u - 64u)

static int failures;

static void expect(int condition, const char* what) {
    if (!condition) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

/* A blob of `size` bytes with deterministic contents, plus its real hash. */
static uint8_t* make_blob(uint32_t size, uint8_t seed, uint8_t hash[32]) {
    uint8_t* data = (uint8_t*)malloc(size);
    uint32_t i;
    for (i = 0; i < size; i++) data[i] = (uint8_t)(seed + (i * 7u));
    pc_mod_cache_sha256(data, size, hash);
    return data;
}

/* Answers a request the way the real chunk service does, feeding the loop. */
static void serve(const PCModFetchRequest* request, const uint8_t* blob, uint32_t size,
                  int corrupt_first_byte) {
    const uint32_t total = (size + CHUNK_BYTES - 1) / CHUNK_BYTES;
    uint16_t i;
    for (i = 0; i < request->chunk_count; i++) {
        const uint32_t index = request->first_chunk + i;
        uint32_t offset;
        uint32_t length;
        if (index >= total) break;
        offset = index * CHUNK_BYTES;
        length = (size - offset < CHUNK_BYTES) ? (size - offset) : CHUNK_BYTES;
        if (corrupt_first_byte && index == 0) {
            uint8_t* tampered = (uint8_t*)malloc(length);
            memcpy(tampered, blob + offset, length);
            tampered[0] ^= 0xFF;
            pc_mod_fetch_on_chunk(request->hash, index, total, tampered, length);
            free(tampered);
        } else {
            pc_mod_fetch_on_chunk(request->hash, index, total, blob + offset, length);
        }
    }
}

int main(void) {
    PCModFetchEntry entries[3];
    PCModFetchRequest request;
    uint8_t* model;
    uint8_t* icon;
    uint8_t* audio;
    int rounds;

    expect(pc_mod_cache_open("cache") == 1, "cache opens");

    /* A model spanning several chunks, a small icon, and audio. */
    model = make_blob(5000, 0x10, entries[0].hash);
    entries[0].size = 5000;
    entries[0].kind = 0;            /* model */
    entries[0].item_handle = 1;

    icon = make_blob(200, 0x20, entries[1].hash);
    entries[1].size = 200;
    entries[1].kind = 1;            /* icon */
    entries[1].item_handle = 1;

    audio = make_blob(900, 0x30, entries[2].hash);
    entries[2].size = 900;
    entries[2].kind = 3;            /* audio */
    entries[2].item_handle = 1;

    expect(pc_mod_fetch_begin(entries, 3) == 3, "all three assets are outstanding");
    expect(pc_mod_fetch_bytes_total() == 5000 + 200 + 900, "total bytes counted");
    expect(pc_mod_fetch_bytes_done() == 0, "nothing done yet");

    /* Icon first: a player notices a pocket icon long before a world model. */
    expect(pc_mod_fetch_next_request(&request) == 1, "a request is produced");
    expect(memcmp(request.hash, entries[1].hash, 32) == 0, "icon is requested first");

    /* Drive to completion, answering whatever is asked. */
    for (rounds = 0; rounds < 50 && pc_mod_fetch_outstanding() > 0; rounds++) {
        if (!pc_mod_fetch_next_request(&request)) break;
        if (memcmp(request.hash, entries[0].hash, 32) == 0) serve(&request, model, 5000, 0);
        else if (memcmp(request.hash, entries[1].hash, 32) == 0) serve(&request, icon, 200, 0);
        else serve(&request, audio, 900, 0);
    }

    expect(pc_mod_fetch_outstanding() == 0, "everything fetched");
    expect(pc_mod_fetch_bytes_done() == pc_mod_fetch_bytes_total(), "progress reaches its total");
    expect(pc_mod_cache_has(entries[0].hash), "model is cached");
    expect(pc_mod_cache_has(entries[1].hash), "icon is cached");
    expect(pc_mod_cache_has(entries[2].hash), "audio is cached");

    /* Rejoining the same town asks for nothing. This is the dedup that makes
     * content addressing worth the indirection. */
    expect(pc_mod_fetch_begin(entries, 3) == 0, "a warm cache fetches nothing");
    expect(pc_mod_fetch_next_request(&request) == 0, "and produces no request");

    /* A tampered blob must not become resident. The bytes are assembled, fail
     * their hash on store, and the asset is dropped rather than retried -- a
     * lying server would just send the same thing again. */
    {
        uint8_t hash[32];
        uint8_t* other = make_blob(3000, 0x40, hash);
        PCModFetchEntry bad;
        memcpy(bad.hash, hash, 32);
        bad.size = 3000;
        bad.kind = 0;
        bad.item_handle = 2;

        expect(pc_mod_fetch_begin(&bad, 1) == 1, "tampered asset is outstanding");
        for (rounds = 0; rounds < 20 && pc_mod_fetch_outstanding() > 0; rounds++) {
            if (!pc_mod_fetch_next_request(&request)) break;
            serve(&request, other, 3000, 1);   /* corrupt chunk 0 */
        }
        expect(!pc_mod_cache_has(hash), "tampered blob is not cached");
        expect(pc_mod_fetch_outstanding() == 0, "tampered asset is dropped, not retried forever");
        expect(pc_mod_fetch_last_completed() == NULL, "no completion reported for it");
        free(other);
    }

    /* A server that answers the manifest but never a chunk must not wedge the
     * loop: after the attempt cap the asset is abandoned and the total drops so
     * progress can still reach 100%. */
    {
        uint8_t hash[32];
        uint8_t* silent = make_blob(4000, 0x50, hash);
        PCModFetchEntry entry;
        memcpy(entry.hash, hash, 32);
        entry.size = 4000;
        entry.kind = 0;
        entry.item_handle = 3;

        expect(pc_mod_fetch_begin(&entry, 1) == 1, "silent asset is outstanding");
        for (rounds = 0; rounds < 10 && pc_mod_fetch_outstanding() > 0; rounds++) {
            if (!pc_mod_fetch_next_request(&request)) break;
            pc_mod_fetch_timeout();            /* server never answers */
        }
        expect(pc_mod_fetch_outstanding() == 0, "unanswered asset is abandoned");
        expect(pc_mod_fetch_bytes_done() == pc_mod_fetch_bytes_total(),
               "abandoning adjusts the total so progress can complete");
        free(silent);
    }

    /* Resume: half the chunks arrive, a timeout intervenes, and the retry picks
     * up from the gap rather than restarting. */
    {
        uint8_t hash[32];
        uint8_t* resumed = make_blob(6000, 0x60, hash);
        PCModFetchEntry entry;
        uint32_t first_after_timeout;
        memcpy(entry.hash, hash, 32);
        entry.size = 6000;
        entry.kind = 0;
        entry.item_handle = 4;

        expect(pc_mod_fetch_begin(&entry, 1) == 1, "resume asset is outstanding");
        expect(pc_mod_fetch_next_request(&request) == 1, "first window requested");
        /* Answer only the first two chunks of the window. */
        {
            PCModFetchRequest partial = request;
            partial.chunk_count = 2;
            serve(&partial, resumed, 6000, 0);
        }
        pc_mod_fetch_timeout();
        expect(pc_mod_fetch_next_request(&request) == 1, "retry produces a request");
        first_after_timeout = request.first_chunk;
        expect(first_after_timeout == 2, "retry resumes from the first missing chunk");

        for (rounds = 0; rounds < 20 && pc_mod_fetch_outstanding() > 0; rounds++) {
            if (!pc_mod_fetch_next_request(&request)) break;
            serve(&request, resumed, 6000, 0);
        }
        expect(pc_mod_cache_has(hash), "resumed asset completes");
        free(resumed);
    }

    free(model);
    free(icon);
    free(audio);
    pc_mod_fetch_reset();
    pc_mod_cache_close();

    printf(failures == 0 ? "mod_fetch: PASS\n" : "mod_fetch: %d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
