/* pc_mod_model.h - compiles a parsed .pcasset into renderable structures (P5).
 *
 * Turns the borrowed views pcasset_parse produced into a Vtx array and a Gfx
 * display list in the mod arena, plus a filled-in aFTR_PROFILE the furniture
 * table can point at.
 *
 * Why this works at all: the PC layer already builds display lists at runtime
 * from heap memory (pc_text_draw.c, and every GRAPH_ALLOC list), and
 * pc_gbi_pack_runtime_ptr exists so emu64::seg2k0 can tell a host pointer from
 * an N64 segment address. So no new rendering support is needed -- only the
 * assembly.
 *
 * The hard part is batching. A gSPVertex command loads at most 32 vertices into
 * the cache, and triangle commands index them with 5 bits, so every triangle's
 * three vertices must sit in the same batch. Vertices are therefore duplicated
 * across batches as needed rather than triangles being split, which cannot be
 * done.
 */
#ifndef PC_MOD_MODEL_H
#define PC_MOD_MODEL_H

#include "pc_mod_assets.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Vertex-cache size a gSPVertex load can fill, and therefore the ceiling on
 * distinct vertices a batch may reference. Triangle indices are 5-bit. */
#define PC_MOD_MODEL_BATCH_VERTS 32

struct ftr_profile_s;

typedef struct {
    void* vertices;          /* Vtx[], in the arena */
    size_t vertex_count;     /* after duplication across batches */
    void* display_list;      /* Gfx[], in the arena */
    size_t command_count;
    size_t batch_count;
} PCModModel;

/* Compiles `source` into the arena. Returns 0 and leaves `out` untouched if the
 * arena cannot supply the memory, or if the model exceeds what a display list
 * can address -- in both cases the item keeps its placeholder, which is a fine
 * outcome and never a failure.
 *
 * `source` must have come from pcasset_parse, which has already checked that
 * every triangle index addresses a vertex that exists. */
int pc_mod_model_compile(const PCAssetModel* source, PCModModel* out);

/* Fills a profile that the furniture table can point at. `base` supplies every
 * field the model does not: vtable, footprint, contact and interaction
 * behaviour. A mod overrides geometry, not conduct. */
int pc_mod_model_fill_profile(const PCModModel* model, const PCAssetModel* source,
                              const struct ftr_profile_s* base, struct ftr_profile_s* out,
                              float scale, float height);

#ifdef __cplusplus
}
#endif

#endif /* PC_MOD_MODEL_H */
