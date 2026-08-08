/* pc_mod_arena.c - see pc_mod_arena.h */

#include "pc_mod_arena.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

static unsigned char* g_base;
static size_t g_capacity;
static size_t g_used;

int pc_mod_arena_init(size_t bytes) {
    void* mapping = NULL;

    if (g_base != NULL) return 1;          /* already up */
    if (bytes == 0) return 0;

#ifdef _WIN32
    /* Asking for a base address rather than accepting whatever the allocator
     * picks: the arena must sit above the N64 segment range. */
    mapping = VirtualAlloc((LPVOID)PC_MOD_ARENA_MIN_ADDRESS, bytes, MEM_RESERVE | MEM_COMMIT,
                           PAGE_READWRITE);
    if (mapping == NULL) mapping = VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    mapping = mmap((void*)PC_MOD_ARENA_MIN_ADDRESS, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) mapping = NULL;
#endif

    if (mapping == NULL) {
        fprintf(stderr, "[Mods] could not reserve a %zu byte asset arena\n", bytes);
        return 0;
    }

    /* The hint is only a hint. If the kernel placed it low, the pointers inside
     * a mod's display list could be misread by emu64 as N64 segment addresses,
     * so refuse rather than render garbage that is very hard to trace back
     * here. Placeholders are a much better failure. */
    if ((uintptr_t)mapping < (uintptr_t)PC_MOD_ARENA_MIN_ADDRESS) {
        fprintf(stderr,
                "[Mods] asset arena landed at %p, below the 0x%zx floor emu64 needs; "
                "mod models disabled\n",
                mapping, (size_t)PC_MOD_ARENA_MIN_ADDRESS);
#ifdef _WIN32
        VirtualFree(mapping, 0, MEM_RELEASE);
#else
        munmap(mapping, bytes);
#endif
        return 0;
    }

    g_base = (unsigned char*)mapping;
    g_capacity = bytes;
    g_used = 0;
    return 1;
}

void* pc_mod_arena_alloc(size_t bytes, size_t align) {
    size_t aligned;
    void* result;

    if (g_base == NULL || bytes == 0) return NULL;
    if (align == 0 || (align & (align - 1)) != 0) align = 8;

    aligned = (g_used + (align - 1)) & ~(align - 1);
    /* Written as a subtraction against the remaining space so the sum cannot
     * overflow on a large request. */
    if (aligned > g_capacity || bytes > g_capacity - aligned) return NULL;

    result = g_base + aligned;
    g_used = aligned + bytes;
    return result;
}

size_t pc_mod_arena_used(void) { return g_used; }
size_t pc_mod_arena_capacity(void) { return g_capacity; }

void pc_mod_arena_shutdown(void) {
    if (g_base == NULL) return;
#ifdef _WIN32
    VirtualFree(g_base, 0, MEM_RELEASE);
#else
    munmap(g_base, g_capacity);
#endif
    g_base = NULL;
    g_capacity = 0;
    g_used = 0;
}
