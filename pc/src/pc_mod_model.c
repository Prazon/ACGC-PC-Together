/* pc_mod_model.c - see pc_mod_model.h */
#ifdef TARGET_PC

#include "pc_mod_model.h"

#include "pc_mod_arena.h"

#include "types.h"
#include "PR/gbi.h"
#include "ac_furniture.h"
#include "libforest/gbi_extensions.h"

#include <stdio.h>
#include <string.h>

/* One triangle's three indices, read from the MSH chunk's little-endian u16s. */
static void read_triangle(const uint8_t* triangles, uint32_t index, uint16_t out[3]) {
    const uint8_t* p = triangles + (size_t)index * 6u;
    int i;
    for (i = 0; i < 3; i++) {
        out[i] = (uint16_t)(p[i * 2] | ((uint16_t)p[i * 2 + 1] << 8));
    }
}

/* Source vertex: s16 x,y,z; s16 u,v; u8 r,g,b,a. Matches the VTX chunk layout
 * documented in pc_mod_assets.h. */
static void read_vertex(const uint8_t* vertices, uint32_t index, Vtx* out) {
    const uint8_t* p = vertices + (size_t)index * 16u;
    const int16_t x = (int16_t)(p[0] | ((uint16_t)p[1] << 8));
    const int16_t y = (int16_t)(p[2] | ((uint16_t)p[3] << 8));
    const int16_t z = (int16_t)(p[4] | ((uint16_t)p[5] << 8));
    const int16_t u = (int16_t)(p[6] | ((uint16_t)p[7] << 8));
    const int16_t v = (int16_t)(p[8] | ((uint16_t)p[9] << 8));

    out->v.ob[0] = x;
    out->v.ob[1] = y;
    out->v.ob[2] = z;
    /* flag 1 = NONSHARED matrix, which is what every runtime-built list in the
     * PC layer uses (see mFont_SetVertex_dol). */
    out->v.flag = 1;
    out->v.tc[0] = u;
    out->v.tc[1] = v;
    out->v.cn[0] = p[10];
    out->v.cn[1] = p[11];
    out->v.cn[2] = p[12];
    out->v.cn[3] = p[13];
}

/* Greedy batching.
 *
 * Triangles are packed into a batch until adding one would need more than 32
 * distinct vertices; a vertex used by two batches is emitted twice. Splitting
 * triangles instead is not an option -- a triangle's three indices must all
 * resolve inside one gSPVertex load -- so duplication is the cost of the 5-bit
 * index, not a shortcut.
 *
 * Two passes: the first counts so the arena allocation is exact, the second
 * writes. Counting first avoids either over-reserving or growing mid-build.
 */
typedef struct {
    uint16_t source_index[PC_MOD_MODEL_BATCH_VERTS];
    uint8_t count;
} Batch;

/* Position of `source` in the batch, adding it if there is room. -1 if full. */
static int batch_slot(Batch* batch, uint16_t source) {
    uint8_t i;
    for (i = 0; i < batch->count; i++) {
        if (batch->source_index[i] == source) return i;
    }
    if (batch->count >= PC_MOD_MODEL_BATCH_VERTS) return -1;
    batch->source_index[batch->count] = source;
    return batch->count++;
}

/* Would this triangle fit? Checked without mutating, so a failed fit does not
 * leave partial vertices behind. */
static int triangle_fits(const Batch* batch, const uint16_t tri[3]) {
    uint8_t needed = 0;
    int i;
    for (i = 0; i < 3; i++) {
        uint8_t j;
        int present = 0;
        int k;
        for (j = 0; j < batch->count; j++) {
            if (batch->source_index[j] == tri[i]) { present = 1; break; }
        }
        if (present) continue;
        /* Do not count the same new vertex twice within one triangle. */
        for (k = 0; k < i; k++) {
            if (tri[k] == tri[i]) { present = 1; break; }
        }
        if (!present) needed++;
    }
    return batch->count + needed <= PC_MOD_MODEL_BATCH_VERTS;
}

int pc_mod_model_compile(const PCAssetModel* source, PCModModel* out) {
    Batch batch;
    uint32_t tri_index;
    size_t total_vertices = 0;
    size_t total_batches = 0;
    size_t total_commands = 0;
    Vtx* vertices;
    Gfx* commands;
    size_t vertex_cursor = 0;
    size_t command_cursor = 0;

    if (!source || !out || source->vertex_count == 0 || source->triangle_count == 0) return 0;

    /* --- Pass 1: measure ------------------------------------------------- */
    batch.count = 0;
    for (tri_index = 0; tri_index < source->triangle_count; tri_index++) {
        uint16_t tri[3];
        read_triangle(source->triangles, tri_index, tri);
        if (!triangle_fits(&batch, tri)) {
            total_vertices += batch.count;
            total_batches++;
            batch.count = 0;
        }
        (void)batch_slot(&batch, tri[0]);
        (void)batch_slot(&batch, tri[1]);
        (void)batch_slot(&batch, tri[2]);
    }
    if (batch.count > 0) {
        total_vertices += batch.count;
        total_batches++;
    }
    if (total_batches == 0) return 0;

    /* Per batch: one gSPVertex, one triangle-init packet, then one continuation
     * per further four triangles. Bounded generously and then trimmed to what
     * was actually written, so the arena is never over-run. */
    total_commands = total_batches * 2 + source->triangle_count + 4;

    vertices = (Vtx*)pc_mod_arena_alloc(total_vertices * sizeof(Vtx), 16);
    commands = (Gfx*)pc_mod_arena_alloc(total_commands * sizeof(Gfx), 16);
    if (!vertices || !commands) {
        fprintf(stderr, "[Mods] model needs %zu vertices and %zu commands; arena is full\n",
                total_vertices, total_commands);
        return 0;
    }

    /* --- Pass 2: emit ---------------------------------------------------- */
    batch.count = 0;
    {
        uint16_t pending[64][3];       /* triangles buffered for the open batch */
        size_t pending_count = 0;
        size_t batch_first_vertex = 0;

        /* Writes the open batch: its vertices, then its triangles. */
        #define FLUSH_BATCH()                                                                     \
            do {                                                                                  \
                if (batch.count > 0) {                                                            \
                    uint8_t vi;                                                                   \
                    size_t t;                                                                     \
                    for (vi = 0; vi < batch.count; vi++) {                                        \
                        read_vertex(source->vertices, batch.source_index[vi],                     \
                                    &vertices[batch_first_vertex + vi]);                           \
                    }                                                                             \
                    gSPVertex(&commands[command_cursor++], &vertices[batch_first_vertex],         \
                              batch.count, 0);                                                    \
                    for (t = 0; t < pending_count; t++) {                                         \
                        gSP1Triangle(&commands[command_cursor++], pending[t][0], pending[t][1],   \
                                     pending[t][2], 0);                                           \
                    }                                                                             \
                    batch_first_vertex += batch.count;                                            \
                    vertex_cursor = batch_first_vertex;                                           \
                }                                                                                 \
                batch.count = 0;                                                                  \
                pending_count = 0;                                                                \
            } while (0)

        for (tri_index = 0; tri_index < source->triangle_count; tri_index++) {
            uint16_t tri[3];
            int a;
            int b;
            int c;

            read_triangle(source->triangles, tri_index, tri);
            /* A batch is also flushed when the pending buffer is full, so the
             * fixed-size array below can never overflow. */
            if (!triangle_fits(&batch, tri) || pending_count >= 64) FLUSH_BATCH();

            a = batch_slot(&batch, tri[0]);
            b = batch_slot(&batch, tri[1]);
            c = batch_slot(&batch, tri[2]);
            if (a < 0 || b < 0 || c < 0) return 0;   /* cannot happen after the fit check */

            pending[pending_count][0] = (uint16_t)a;
            pending[pending_count][1] = (uint16_t)b;
            pending[pending_count][2] = (uint16_t)c;
            pending_count++;
        }
        FLUSH_BATCH();
        #undef FLUSH_BATCH
    }

    gSPEndDisplayList(&commands[command_cursor++]);

    out->vertices = vertices;
    out->vertex_count = vertex_cursor;
    out->display_list = commands;
    out->command_count = command_cursor;
    out->batch_count = total_batches;
    return 1;
}

int pc_mod_model_fill_profile(const PCModModel* model, const PCAssetModel* source,
                              const aFTR_PROFILE* base, aFTR_PROFILE* out, float scale,
                              float height) {
    if (!model || !source || !base || !out) return 0;

    /* Start from the base so every field a mod does not set keeps the vanilla
     * behaviour: vtable, footprint, contact and interaction. A mod overrides
     * what an item looks like, not how it behaves. */
    *out = *base;
    out->opaque0 = (Gfx*)model->display_list;
    out->opaque1 = NULL;
    out->translucent0 = NULL;
    out->translucent1 = NULL;
    out->texture = (u8*)source->texture;
    out->palette = (u16*)source->palette;
    if (scale > 0.0f) out->scale = scale;
    if (height > 0.0f) out->height = height;
    return 1;
}

#endif /* TARGET_PC */
