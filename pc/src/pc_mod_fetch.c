/* pc_mod_fetch.c - see pc_mod_fetch.h */

#include "pc_mod_fetch.h"

#include "pc_mod_cache.h"

#include <stdlib.h>
#include <string.h>

/* Chunk payload size. Must match acnet::kAssetChunkBytes; duplicated because
 * pc/ does not include the C++ netcode headers. A mismatch here would show up
 * as blobs that never complete rather than as corruption, because the assembled
 * bytes are hash-verified before anything uses them. */
#define PC_MOD_FETCH_CHUNK_BYTES (1136u - 64u)

/* Windows a single asset may be re-requested for before it is abandoned. */
#define PC_MOD_FETCH_MAX_ATTEMPTS 3

typedef struct {
    PCModFetchEntry entry;
    int done;
} Pending;

static Pending* g_pending;
static size_t g_pending_count;
static size_t g_outstanding;
static uint64_t g_bytes_total;
static uint64_t g_bytes_done;

/* The single blob in flight. One at a time rather than several: the window is
 * already 16 chunks, and serialising keeps the assembly buffer to one blob's
 * worth instead of the whole manifest's. */
static int g_active = -1;
static uint8_t* g_assembly;
static uint32_t g_assembly_total_chunks;
static uint8_t* g_chunk_seen;
static uint32_t g_chunks_received;
static int g_attempts;
static uint8_t g_last_completed[PC_MOD_FETCH_HASH_BYTES];
static int g_have_last_completed;

static void clear_active(void) {
    free(g_assembly);
    free(g_chunk_seen);
    g_assembly = NULL;
    g_chunk_seen = NULL;
    g_active = -1;
    g_assembly_total_chunks = 0;
    g_chunks_received = 0;
    g_attempts = 0;
}

void pc_mod_fetch_reset(void) {
    clear_active();
    free(g_pending);
    g_pending = NULL;
    g_pending_count = 0;
    g_outstanding = 0;
    g_bytes_total = 0;
    g_bytes_done = 0;
    g_have_last_completed = 0;
}

/* Ordering: by kind, then smallest first.
 *
 * Name and icon before model is what a player actually notices -- an item shows
 * its correct name and icon in the pocket while its world model is still a
 * placeholder. Smallest-first within a tier means the count of placeholders
 * drops as fast as possible rather than one large asset holding up ten small
 * ones. */
static int priority_of(uint16_t kind) {
    switch (kind) {
        case 1: return 0;   /* icon */
        case 2: return 1;   /* texture */
        case 0: return 2;   /* model */
        case 3: return 3;   /* audio */
        default: return 4;
    }
}

static int compare_pending(const void* left, const void* right) {
    const Pending* a = (const Pending*)left;
    const Pending* b = (const Pending*)right;
    const int pa = priority_of(a->entry.kind);
    const int pb = priority_of(b->entry.kind);
    if (pa != pb) return pa - pb;
    if (a->entry.size != b->entry.size) return (a->entry.size < b->entry.size) ? -1 : 1;
    /* Hash as the final tiebreak, so the order is a pure function of the
     * manifest rather than of whatever order the server listed it in. */
    return memcmp(a->entry.hash, b->entry.hash, PC_MOD_FETCH_HASH_BYTES);
}

int pc_mod_fetch_begin(const PCModFetchEntry* entries, size_t count) {
    size_t i;
    size_t kept = 0;

    pc_mod_fetch_reset();
    if (!entries || count == 0 || count > PC_MOD_FETCH_MAX_ENTRIES) return 0;

    g_pending = (Pending*)calloc(count, sizeof(Pending));
    if (!g_pending) return 0;   /* no memory for bookkeeping: placeholders, not a failure */

    for (i = 0; i < count; i++) {
        if (entries[i].size == 0) continue;
        /* Anything already held is not fetched. This is where cross-town dedup
         * actually pays off: a second town using the same model asks for
         * nothing. */
        if (pc_mod_cache_has(entries[i].hash)) continue;
        g_pending[kept].entry = entries[i];
        g_pending[kept].done = 0;
        g_bytes_total += entries[i].size;
        kept++;
    }

    g_pending_count = kept;
    g_outstanding = kept;
    if (kept > 1) qsort(g_pending, kept, sizeof(Pending), compare_pending);
    return (int)kept;
}

static int first_unfinished(void) {
    size_t i;
    for (i = 0; i < g_pending_count; i++) {
        if (!g_pending[i].done) return (int)i;
    }
    return -1;
}

int pc_mod_fetch_next_request(PCModFetchRequest* out) {
    int index;
    uint32_t total;
    uint32_t start = 0;

    if (!out || g_outstanding == 0) return 0;

    if (g_active < 0) {
        index = first_unfinished();
        if (index < 0) return 0;

        total = (g_pending[index].entry.size + PC_MOD_FETCH_CHUNK_BYTES - 1) / PC_MOD_FETCH_CHUNK_BYTES;
        if (total == 0) return 0;

        g_assembly = (uint8_t*)malloc(g_pending[index].entry.size);
        g_chunk_seen = (uint8_t*)calloc(total, 1);
        if (!g_assembly || !g_chunk_seen) {
            /* Out of memory mid-download is not fatal: drop this asset and let
             * it render as a placeholder. */
            clear_active();
            g_pending[index].done = 1;
            g_outstanding--;
            return 0;
        }
        g_active = index;
        g_assembly_total_chunks = total;
        g_chunks_received = 0;
    }

    /* Ask from the first chunk still missing, so a retry after a dropped packet
     * resumes rather than restarting. */
    for (start = 0; start < g_assembly_total_chunks; start++) {
        if (!g_chunk_seen[start]) break;
    }
    if (start >= g_assembly_total_chunks) return 0;

    memcpy(out->hash, g_pending[g_active].entry.hash, PC_MOD_FETCH_HASH_BYTES);
    out->first_chunk = start;
    {
        const uint32_t remaining = g_assembly_total_chunks - start;
        out->chunk_count = (uint16_t)(remaining < PC_MOD_FETCH_WINDOW ? remaining : PC_MOD_FETCH_WINDOW);
    }
    return 1;
}

int pc_mod_fetch_on_chunk(const uint8_t hash[PC_MOD_FETCH_HASH_BYTES], uint32_t index,
                          uint32_t total_chunks, const void* bytes, size_t length) {
    size_t offset;
    size_t expected;

    g_have_last_completed = 0;
    if (g_active < 0 || !hash || !bytes) return 0;
    /* A chunk for something else, or with a transfer length that disagrees with
     * what we computed, is discarded rather than trusted. */
    if (memcmp(hash, g_pending[g_active].entry.hash, PC_MOD_FETCH_HASH_BYTES) != 0) return 0;
    if (total_chunks != g_assembly_total_chunks) return 0;
    if (index >= g_assembly_total_chunks) return 0;
    if (g_chunk_seen[index]) return 0;   /* duplicate; harmless */

    offset = (size_t)index * PC_MOD_FETCH_CHUNK_BYTES;
    if (offset >= g_pending[g_active].entry.size) return 0;

    /* The last chunk is short; every other one must be full. A server sending
     * anything else would leave a hole in the assembled blob, and the hash
     * check would catch it -- but refusing here keeps the buffer honest. */
    expected = g_pending[g_active].entry.size - offset;
    if (expected > PC_MOD_FETCH_CHUNK_BYTES) expected = PC_MOD_FETCH_CHUNK_BYTES;
    if (length != expected) return 0;

    memcpy(g_assembly + offset, bytes, length);
    g_chunk_seen[index] = 1;
    g_chunks_received++;
    g_bytes_done += length;

    if (g_chunks_received < g_assembly_total_chunks) return 0;

    /* Complete. Storing verifies the hash, so a blob assembled from a lying or
     * corrupted server is refused here and simply never becomes resident. */
    if (pc_mod_cache_store(g_pending[g_active].entry.hash, g_assembly,
                           g_pending[g_active].entry.size)) {
        memcpy(g_last_completed, g_pending[g_active].entry.hash, PC_MOD_FETCH_HASH_BYTES);
        g_have_last_completed = 1;
    } else {
        /* Bytes did not hash to what was advertised. Do not retry: the server
         * would send the same thing again. The item keeps its placeholder. */
        g_bytes_done -= (g_bytes_done < g_pending[g_active].entry.size)
                            ? g_bytes_done
                            : g_pending[g_active].entry.size;
    }

    g_pending[g_active].done = 1;
    if (g_outstanding > 0) g_outstanding--;
    clear_active();
    return g_have_last_completed;
}

const uint8_t* pc_mod_fetch_last_completed(void) {
    return g_have_last_completed ? g_last_completed : NULL;
}

size_t pc_mod_fetch_outstanding(void) { return g_outstanding; }
uint64_t pc_mod_fetch_bytes_total(void) { return g_bytes_total; }
uint64_t pc_mod_fetch_bytes_done(void) { return g_bytes_done; }

void pc_mod_fetch_timeout(void) {
    if (g_active < 0) return;

    /* Chunks already received are kept: pc_mod_fetch_next_request resumes from
     * the first gap, so a dropped packet costs one window rather than the whole
     * asset. That is the resume the protocol's client-paced pull is for.
     *
     * Attempts are bounded so a server that answers the manifest but never the
     * chunks cannot hold the queue forever -- after the cap the asset is
     * abandoned and its item keeps its placeholder, which is a fine outcome. */
    g_attempts++;
    if (g_attempts < PC_MOD_FETCH_MAX_ATTEMPTS) return;

    /* Give up on this one. The bytes counted toward progress go back, or the
     * loading screen would show a total that can never be reached. */
    {
        const uint64_t received = (uint64_t)g_chunks_received * PC_MOD_FETCH_CHUNK_BYTES;
        g_bytes_done -= (received <= g_bytes_done) ? received : g_bytes_done;
        g_bytes_total -= (g_pending[g_active].entry.size <= g_bytes_total)
                             ? g_pending[g_active].entry.size
                             : g_bytes_total;
    }
    g_pending[g_active].done = 1;
    if (g_outstanding > 0) g_outstanding--;
    clear_active();
}
