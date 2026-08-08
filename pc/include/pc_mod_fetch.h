/* pc_mod_fetch.h - client fetch loop for server-delivered content (P8).
 *
 * A pure state machine. It is told what the town has (a manifest), what it
 * already holds (the cache), and what arrives (chunks); it says what to ask for
 * next. Sending is the caller's job, so this is testable without a socket and
 * the networking layer stays free of download bookkeeping.
 *
 * The governing rule from docs/MODLOADER_PLAN.md sec 8.1: a missing asset never
 * blocks and never fails. Nothing here can stall a join -- the player is in the
 * town before the first chunk arrives, and an asset that never turns up simply
 * leaves its item rendering as a placeholder forever. Every failure path
 * degrades to "not resident yet" rather than to an error.
 *
 * Priority is name -> icon -> model -> audio, smallest first within a tier. The
 * parts a player notices in their pocket arrive before the world model does.
 */
#ifndef PC_MOD_FETCH_H
#define PC_MOD_FETCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PC_MOD_FETCH_HASH_BYTES 32
/* Matches acnet::kMaxAssetEntries and kAssetWindowChunks. Duplicated because
 * pc/ must not include the C++ netcode headers; a mismatch shows up as a
 * refused manifest, not as corruption. */
#define PC_MOD_FETCH_MAX_ENTRIES 4096
#define PC_MOD_FETCH_WINDOW 16

typedef struct {
    uint8_t hash[PC_MOD_FETCH_HASH_BYTES];
    uint32_t size;
    uint16_t kind;          /* 0 model, 1 icon, 2 texture, 3 audio */
    uint16_t item_handle;
} PCModFetchEntry;

typedef struct {
    uint8_t hash[PC_MOD_FETCH_HASH_BYTES];
    uint32_t first_chunk;
    uint16_t chunk_count;
} PCModFetchRequest;

/* Resets and takes the manifest, dropping anything already cached. Returns the
 * number of assets that still need fetching. A manifest larger than the caps,
 * or one the cache cannot be consulted for, yields 0 outstanding -- the town
 * simply renders placeholders. */
int pc_mod_fetch_begin(const PCModFetchEntry* entries, size_t count);

/* Fills `out` with the next window to ask for. Returns 0 when there is nothing
 * outstanding or a request is already in flight -- the caller re-asks each
 * frame and this decides whether there is anything to say. */
int pc_mod_fetch_next_request(PCModFetchRequest* out);

/* Feeds one arrived chunk. Ignores anything unexpected: a chunk for an asset
 * that is not in flight, a duplicate, or an index past the end. Returns 1 when
 * this chunk completed a blob, which is the caller's cue to check
 * pc_mod_fetch_last_completed. */
int pc_mod_fetch_on_chunk(const uint8_t hash[PC_MOD_FETCH_HASH_BYTES], uint32_t index,
                          uint32_t total_chunks, const void* bytes, size_t length);

/* The blob completed by the most recent pc_mod_fetch_on_chunk, or NULL. Valid
 * until the next call. */
const uint8_t* pc_mod_fetch_last_completed(void);

/* Progress, for the loading screen. */
size_t pc_mod_fetch_outstanding(void);
uint64_t pc_mod_fetch_bytes_total(void);
uint64_t pc_mod_fetch_bytes_done(void);

/* Called when a request times out, so a dropped packet does not wedge the loop
 * forever. The asset returns to the queue. */
void pc_mod_fetch_timeout(void);

void pc_mod_fetch_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_MOD_FETCH_H */
