/* pc_modloader.c - see pc_modloader.h */
#ifdef TARGET_PC

#include "pc_modloader.h"

#include "pc_mod_arena.h"
#include "m_ftr_profile_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

extern int g_pc_verbose;

#define PC_MOD_MAX_OVERRIDES 512
#define PC_MOD_MAX_PATH 512
#define PC_MOD_MAX_MODS 128
/* Bounded so a stray large file in an overrides directory cannot be pulled
 * wholesale into memory before the size check rejects it. The largest real
 * asset is well under this. */
#define PC_MOD_MAX_ASSET_BYTES (8u * 1024u * 1024u)

typedef struct {
    char name[128];   /* the asset table's basename, e.g. "con_kaiwa2_w1_tex.bin" */
    char owner[64];   /* mod id, for conflict reporting */
    void* data;
    unsigned int size;
} PCModOverride;

static PCModOverride g_overrides[PC_MOD_MAX_OVERRIDES];
static int g_override_count = 0;
static int g_initialized = 0;

/* Mods are scanned in id order, not filesystem order. A conflict between two
 * mods must resolve the same way on every machine, or a broken install
 * reproduces for one player and not another. */
static void sort_ids(char ids[][64], int count) {
    int i;
    int j;
    for (i = 1; i < count; i++) {
        char key[64];
        /* memcpy rather than snprintf: the rows are fixed-size and adjacent, and
         * snprintf's restrict-qualified arguments make an overlapping copy
         * undefined (and a -Werror=restrict diagnostic). */
        memcpy(key, ids[i], sizeof(key));
        for (j = i - 1; j >= 0 && strcmp(ids[j], key) > 0; j--) {
            memcpy(ids[j + 1], ids[j], sizeof(key));
        }
        memcpy(ids[j + 1], key, sizeof(key));
    }
}

static const char* basename_of(const char* path) {
    const char* slash = strrchr(path, '/');
    const char* back = strrchr(path, '\\');
    const char* last = (slash > back) ? slash : back;
    return last ? last + 1 : path;
}

static int find_override(const char* name) {
    int i;
    for (i = 0; i < g_override_count; i++) {
        if (strcmp(g_overrides[i].name, name) == 0) return i;
    }
    return -1;
}

static void register_override(const char* mod_id, const char* dir, const char* filename) {
    char full[PC_MOD_MAX_PATH];
    FILE* file;
    long size;
    void* buffer;
    int existing;

    if (strlen(filename) >= sizeof(g_overrides[0].name)) {
        fprintf(stderr, "[Mods] %s: override name too long: %s\n", mod_id, filename);
        return;
    }

    existing = find_override(filename);
    if (existing >= 0) {
        /* Two mods overriding the same asset is a conflict the player must be
         * told about. Silent last-wins would make a broken install impossible
         * to diagnose from the outside. */
        fprintf(stderr, "[Mods] CONFLICT: '%s' is overridden by both '%s' and '%s'; keeping '%s'\n",
                filename, g_overrides[existing].owner, mod_id, g_overrides[existing].owner);
        return;
    }
    if (g_override_count >= PC_MOD_MAX_OVERRIDES) {
        fprintf(stderr, "[Mods] too many overrides (max %d); ignoring %s\n",
                PC_MOD_MAX_OVERRIDES, filename);
        return;
    }

    if (snprintf(full, sizeof(full), "%s/%s", dir, filename) >= (int)sizeof(full)) return;
    file = fopen(full, "rb");
    if (!file) return;

    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0 || (unsigned long)size > PC_MOD_MAX_ASSET_BYTES) {
        fprintf(stderr, "[Mods] %s: %s has an implausible size (%ld bytes)\n", mod_id, filename, size);
        fclose(file);
        return;
    }

    buffer = malloc((size_t)size);
    if (!buffer) { fclose(file); return; }
    if (fread(buffer, 1, (size_t)size, file) != (size_t)size) {
        fprintf(stderr, "[Mods] %s: short read on %s\n", mod_id, filename);
        free(buffer);
        fclose(file);
        return;
    }
    fclose(file);

    snprintf(g_overrides[g_override_count].name, sizeof(g_overrides[0].name), "%s", filename);
    snprintf(g_overrides[g_override_count].owner, sizeof(g_overrides[0].owner), "%s", mod_id);
    g_overrides[g_override_count].data = buffer;
    g_overrides[g_override_count].size = (unsigned int)size;
    g_override_count++;

    if (g_pc_verbose) {
        printf("[Mods] %s overrides %s (%ld bytes)\n", mod_id, filename, size);
    }
}

#ifdef _WIN32
static void scan_overrides_dir(const char* mod_id, const char* dir) {
    char pattern[PC_MOD_MAX_PATH];
    WIN32_FIND_DATAA entry;
    HANDLE handle;

    if (snprintf(pattern, sizeof(pattern), "%s/*", dir) >= (int)sizeof(pattern)) return;
    handle = FindFirstFileA(pattern, &entry);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        register_override(mod_id, dir, entry.cFileName);
    } while (FindNextFileA(handle, &entry));
    FindClose(handle);
}

static void scan_mods_root(const char* root) {
    char pattern[PC_MOD_MAX_PATH];
    WIN32_FIND_DATAA entry;
    HANDLE handle;
    char ids[PC_MOD_MAX_MODS][64];
    int count = 0;
    int i;

    if (snprintf(pattern, sizeof(pattern), "%s/*", root) >= (int)sizeof(pattern)) return;
    handle = FindFirstFileA(pattern, &entry);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (entry.cFileName[0] == '.') continue;
        if (strlen(entry.cFileName) >= sizeof(ids[0])) continue;
        if (count >= PC_MOD_MAX_MODS) break;
        memcpy(ids[count], entry.cFileName, strlen(entry.cFileName) + 1);
        count++;
    } while (FindNextFileA(handle, &entry));
    FindClose(handle);

    sort_ids(ids, count);
    for (i = 0; i < count; i++) {
        char overrides[PC_MOD_MAX_PATH];
        if (snprintf(overrides, sizeof(overrides), "%s/%s/overrides", root, ids[i]) >=
            (int)sizeof(overrides)) continue;
        scan_overrides_dir(ids[i], overrides);
    }
}
#else
static void scan_overrides_dir(const char* mod_id, const char* dir) {
    DIR* handle = opendir(dir);
    struct dirent* entry;
    if (!handle) return;
    while ((entry = readdir(handle)) != NULL) {
        char full[PC_MOD_MAX_PATH];
        struct stat info;
        if (entry->d_name[0] == '.') continue;
        if (snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name) >= (int)sizeof(full)) continue;
        if (stat(full, &info) != 0 || !S_ISREG(info.st_mode)) continue;
        register_override(mod_id, dir, entry->d_name);
    }
    closedir(handle);
}

static void scan_mods_root(const char* root) {
    DIR* handle = opendir(root);
    struct dirent* entry;
    char ids[PC_MOD_MAX_MODS][64];
    int count = 0;
    int i;

    if (!handle) return;
    while ((entry = readdir(handle)) != NULL && count < PC_MOD_MAX_MODS) {
        char mod_dir[PC_MOD_MAX_PATH];
        struct stat info;
        if (entry->d_name[0] == '.') continue;
        if (strlen(entry->d_name) >= sizeof(ids[0])) continue;
        if (snprintf(mod_dir, sizeof(mod_dir), "%s/%s", root, entry->d_name) >= (int)sizeof(mod_dir))
            continue;
        if (stat(mod_dir, &info) != 0 || !S_ISDIR(info.st_mode)) continue;
        memcpy(ids[count], entry->d_name, strlen(entry->d_name) + 1);
        count++;
    }
    closedir(handle);

    sort_ids(ids, count);
    for (i = 0; i < count; i++) {
        char overrides[PC_MOD_MAX_PATH];
        if (snprintf(overrides, sizeof(overrides), "%s/%s/overrides", root, ids[i]) >=
            (int)sizeof(overrides)) continue;
        scan_overrides_dir(ids[i], overrides);
    }
}
#endif

int pc_modloader_init(void) {
    if (g_initialized) return g_override_count;
    g_initialized = 1;
    /* Scanned in id order (see sort_ids), so a conflict resolves identically
     * everywhere: the alphabetically-first mod wins and the clash is reported. */
    scan_mods_root("mods");
    if (g_override_count > 0) {
        printf("[Mods] %d asset override(s) active\n", g_override_count);
    }
    return g_override_count;
}

const void* pc_modloader_override(const char* bin_path, unsigned int expect_size) {
    int index;
    if (!g_initialized || g_override_count == 0 || !bin_path) return NULL;

    index = find_override(basename_of(bin_path));
    if (index < 0) return NULL;

    if (g_overrides[index].size != expect_size) {
        /* The destination is a fixed-size array, so this cannot be honoured.
         * Reported loudly and once: a silently ignored override looks exactly
         * like a mod that does not work. */
        fprintf(stderr, "[Mods] %s: '%s' is %u bytes but the game expects %u; override ignored\n",
                g_overrides[index].owner, g_overrides[index].name,
                g_overrides[index].size, expect_size);
        /* Drop it so the message does not repeat every load. */
        free(g_overrides[index].data);
        g_overrides[index] = g_overrides[--g_override_count];
        return NULL;
    }
    return g_overrides[index].data;
}

int pc_modloader_grow_furniture(size_t extra, const void* base_profile) {
    aFTR_PROFILE** grown;
    size_t total;
    size_t i;

    if (extra == 0) return 1;
    total = furniture_quality_count + extra;

    /* From the mod arena, not malloc: these pointers end up inside display
     * lists, where emu64 distinguishes a host pointer from an N64 segment
     * address by range. */
    grown = (aFTR_PROFILE**)pc_mod_arena_alloc(total * sizeof(*grown), 8);
    if (grown == NULL) {
        fprintf(stderr, "[Mods] could not grow the furniture table to %zu entries; "
                        "mod furniture disabled\n", total);
        return 0;
    }

    memcpy(grown, furniture_quality, furniture_quality_count * sizeof(*grown));
    for (i = furniture_quality_count; i < total; i++) {
        grown[i] = (aFTR_PROFILE*)base_profile;
    }

    /* Published last, as a single store. Everything the new table needs is in
     * place before any reader can observe it. */
    furniture_quality = grown;
    furniture_quality_count = total;
    return 1;
}

void pc_modloader_shutdown(void) {
    int i;
    for (i = 0; i < g_override_count; i++) free(g_overrides[i].data);
    g_override_count = 0;
    g_initialized = 0;
}

#endif /* TARGET_PC */
