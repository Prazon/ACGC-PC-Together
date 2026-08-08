/* pc_mod_cache.h - content-addressed client cache (P8).
 *
 * Blobs are stored under the hex of their own hash, sharded one level:
 *
 *   <cache>/ab/ab3f9c...d21
 *
 * Global rather than per-town, so joining a second town that uses the same
 * model downloads nothing. (The trade-off is a fingerprinting channel -- town B
 * can infer by timing that you have an asset only town A serves -- which is
 * logged as an open question in docs/MODLOADER_PLAN.md.)
 *
 * Content addressing does three things at once here: the hash is the name, so a
 * manifest never supplies a filename and path traversal is structurally
 * impossible; verification and identification are the same operation, so no
 * signature scheme is needed for integrity; and a corrupted or tampered blob
 * simply fails to hash and is refetched.
 *
 * Free of SDL, GL and game headers so it can be tested on its own.
 */
#ifndef PC_MOD_CACHE_H
#define PC_MOD_CACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PC_MOD_CACHE_HASH_BYTES 32
/* Matches acnet::kMaxAssetBlobBytes. Duplicated rather than included because
 * pc/ must not depend on the C++ netcode headers; a mismatch would show up as a
 * refused blob, not corruption. */
#define PC_MOD_CACHE_MAX_BLOB (4u * 1024u * 1024u)

/* Points the cache at a directory, creating it if needed. Returns 0 on failure,
 * in which case every other call is a no-op and the client simply renders
 * placeholders -- a cache that cannot be opened must not stop the game. */
int pc_mod_cache_open(const char* directory);

/* 1 if a blob with this hash is already stored. */
int pc_mod_cache_has(const uint8_t hash[PC_MOD_CACHE_HASH_BYTES]);

/* Stores `data`, verifying it hashes to `hash` first. Returns 0 and stores
 * nothing if it does not -- this is the check that makes delivery safe over an
 * untrusted link. */
int pc_mod_cache_store(const uint8_t hash[PC_MOD_CACHE_HASH_BYTES], const void* data, size_t size);

/* Reads a stored blob into a malloc'd buffer the caller frees. Re-verifies on
 * read, so a blob corrupted on disk after storing is rejected rather than
 * handed to the parser. Returns NULL if absent or corrupt. */
void* pc_mod_cache_load(const uint8_t hash[PC_MOD_CACHE_HASH_BYTES], size_t* size_out);

/* SHA-256, exposed because callers need to verify before storing. */
void pc_mod_cache_sha256(const void* data, size_t size, uint8_t out[PC_MOD_CACHE_HASH_BYTES]);

void pc_mod_cache_close(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_MOD_CACHE_H */
