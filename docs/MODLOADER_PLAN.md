# Modloader and asset injection — plan

How a mod ships **new original assets** — models, textures, icons — and how they get bound to
game objects at runtime. This is the client-side half of the modding effort;
`docs/netcode/MODDING_PLAN.md` is the server-side half (Lua, authority, holidays).

The two interlock at exactly one point: the server decides *what exists*, the modloader decides
*what it looks like*. Neither can ship the other's half.

Nothing here is implemented.

---

## 1. What "custom assets" actually requires

A mod that adds a new piece of furniture needs six things to exist at once:

| # | Thing | Today | Verdict |
|-|-|-|-|
| 1 | An item ID the game accepts | vanilla `mActor_name_t`; ~11 parallel tables indexed by it | **the hard part** (§3) |
| 2 | A name string | 16-byte records in **writable** BSS arrays | easy (§2.7) |
| 3 | A 32×32 CI4 inventory icon + palette | runtime-filled banks already exist | easy (§2.4) |
| 4 | A model: display list + vertices + texture + palette | runtime display lists already work | tractable (§2.2) |
| 5 | An `aFTR_PROFILE` describing footprint, scale, behaviour | a plain struct of pointers | easy (§2.5) |
| 6 | Every other player in the town to see the same thing | no distribution path | policy choice (§8) |

`docs/netcode/MODDING_PLAN.md` §8.1 called this "the asset wall" and scoped around it. This
document is the plan for removing it.

**The good news, established by survey:** items 2–5 already have a runtime-dynamic path in the
tree. The engine was made data-driven by the PC port for unrelated reasons, and the seams are
reusable.

**The bad news, established by a working reference implementation:** item 1 is not one table. It
is roughly a dozen parallel arrays that must stay exactly the same length and are indexed without
bounds checks. That, not the art pipeline, is where the difficulty actually lives.

---

## 2. Seams that already exist

### 2.1 Assets already load from files

`pc_load_asset` (`pc/src/pc_assets.c:80-93`) has a three-step fallback that is already shipping:

```c
void pc_load_asset(const char* bin_path, void* dest, unsigned int size,
                   unsigned int rom_off, int rom_src, int swap_type) {
    /* 1. ROM-direct from the extracted DOL/REL */
    if (rom_src != SRC_NONE) { ...memcpy(dest, rom + rom_off, size); loaded = 1; }
    /* 2. Loose .bin file on disk  <-- the injection seam */
    if (!loaded && bin_path) { FILE* f = fopen(bin_path, "rb"); ... }
    if (!loaded) fprintf(stderr, "[PC] ASSET MISSING: %s\n", ...);
    if (loaded) do_swap(dest, size, swap_type);
}
```

Step 2 exists for disc-less builds. **Inverting its priority for a named set of assets is a
~40-line change** and yields whole-asset replacement immediately.

Limitation: `dest` is a fixed-size static array generated into `pc/src/pc_assets.c` (~2500 of
them, produced by `pc/tools/gen_runtime_assets.py`, 1056 lines). An override must be **exactly
`size` bytes**. That is fine for reskins, useless for new content.

### 2.2 Display lists already work from heap memory

This is the load-bearing discovery. `pc/src/pc_gbi_runtime.c:11` exists because the game builds
display lists at runtime and emu64 must distinguish an N64 segment address from a real host
pointer:

```c
unsigned int pc_gbi_pack_runtime_ptr(uintptr_t addr, int is_ptr, ...);
```

`emu64::seg2k0` resolves both. So a display list **assembled at runtime in malloc'd memory, with
pointers to malloc'd vertices and textures, renders correctly**. No new rendering work is needed
for mod models — `pc_text_draw.c` and every `GRAPH_ALLOC`-built list already prove the path.

### 2.3 Texture decode is fully general

`pc/src/pc_gx_texture.c:584-593` decodes every GC format to RGBA before upload:
`I4 I8 IA4 IA8 RGB565 RGB5A3 RGBA8 CI4 CI8 CMPR`. A mod-supplied texture in any of these works
unchanged. RGBA8 is the obvious authoring target — no quantisation, no palette.

### 2.4 Runtime-generated texture + palette banks are an established pattern

`mSM_Object_Exchange_keep_new_MenuTexAndPallet` (`src/game/m_submenu.c:649-656`) reserves a
32×32 CI4 texture and a 16-entry palette from the scene's object-exchange heap and fills them at
runtime with the player's current shirt:

```c
char* tex_p = mSM_Object_Exchange_keep_new(play, ACTOR_OBJ_BANK_14, (32 * 32) / 2);
char* pal_p = mSM_Object_Exchange_keep_new(play, ACTOR_OBJ_BANK_15, 16 * sizeof(u16));
mPlib_Load_PlayerTexAndPallet(tex_p, pal_p, ...);
```

`mSc_secure_exchange_keep_bank` (`src/game/m_scene.c:39-63`) sets `rom_addr = 0` for these — a
RAM-only bank the game fills itself. **Item icons for mod items already have a working
precedent.**

### 2.5 Furniture profiles are pure data

`aFTR_PROFILE` (`include/ac_furniture.h:181-201`) is a struct of pointers and scalars:

```c
typedef struct ftr_profile_s {
    Gfx* opaque0;  Gfx* opaque1;  Gfx* translucent0;  Gfx* translucent1;
    u8* texture;   u16* palette;
    aFTR_rig_c* rig;   aFTR_tex_anim_c* tex_anim;
    f32 height;  f32 scale;
    u8 shape;  u8 move_bg_type;  u8 check_rotation;  u8 kankyo_map;  u8 contact_action;
    u16 interaction_type;
    aFTR_vtable_c* vtable;
} aFTR_PROFILE;
```

**If a mod can produce those pointers, it has a furniture item.** No engine change is required to
*describe* one — only to allocate, populate and register it. That is the single strongest reason
to believe this project is tractable.

### 2.6 Texture packs already replace art per-client

`pc/src/pc_texture_pack.c` is a Dolphin-compatible loader keyed on XXHash64 of texture content
(plus TLUT hash for CI formats), reading `tex1_{W}x{H}_{hash}_{fmt}.dds`. It replaces existing
textures. It cannot add new ones, and it is a per-client install with no server involvement.

### 2.7 Item names are mutable BSS, not ROM strings

`src/data/item/item_name.c` declares the name tables as **writable arrays filled at boot**:

```c
#ifdef TARGET_PC
unsigned char itemName_paper[0x1000];      /* filled by pc_load_asset from the disc */
#else
unsigned char itemName_paper[] = { #include "assets/itemName_paper.inc" };
#endif
```

Records are `mIN_ITEM_NAME_LEN` = 16 bytes, space-padded, in the game's own codepage
(`include/m_font.h`), **not** ASCII. So naming a mod item is a 16-byte write into an existing
array at the right index — no ROM patching, no `RESOURCE_STRING` entry. This is materially easier
than `docs/netcode/MODDING_PLAN.md` §3.1 assumed for holiday names, and the same mechanism serves
both.

---

## 3. The lockstep table problem

Furniture is described by several **parallel arrays indexed by the furniture enum**, which must
all be the same length. Adding an item means appending to every one of them.

**Corrected against the tree during P0 implementation.** An earlier draft of this section, working
from a description of another fork's pipeline, claimed "roughly a dozen tables indexed without
bounds checks". The measured reality is better: `FTR_NUM` is **1266**, and of the
furniture-indexed tables, **five are declared `[FTR_NUM]`**, so the declaration itself enforces
the length and a short initialiser merely zero-fills. Only two take their length from the
initialiser:

| Table | File | Declared | Indexing |
|-|-|-|-|
| `furniture_quality[]` (→ `aFTR_PROFILE*`) | `ac_furniture_profile_data.c_inc:1` | `[]` — 1266 | **unchecked** ← the real hazard |
| `ftr_price_table[]` | `src/data/item/ftr_price.c:1` | `[]` — 1267 (+ `-1` sentinel) | runtime-checked via `mSP_CountPriceTableElement` |
| `ftrArt[]` | `src/game/m_item_name.c:246` | `[FTR_NUM]` | checked (`ftr_idx < FTR_NUM`) |
| `mMkRm_ftr_info[]` | `src/game/m_mark_room_ovl.c:153` | `[FTR_NUM]` | unchecked, but self-sized |
| `mMkRm_ftr_info[]` | `src/game/m_huusui_room_ovl.c:29` | `[FTR_NUM]` | unchecked, but self-sized |
| `mRmTp_ftr_se_type[]` | `src/game/m_room_type.c:75` | `[FTR_NUM]` | self-sized |
| `mRmTp_birth_type[]` | `src/game/m_room_type.c:520` | `[FTR_NUM]` | self-sized |

Plus the two enums that define the index space (`include/m_ftr_def.h`, `enum ftr1_e` in
`include/m_name_table.h`), the 16-byte name record in `src/data/item/item_name.c`, and the
catalogue/my-room data files — none of which are flat FTR-indexed arrays, so they are appended to
rather than asserted on.

**So exactly one table is genuinely exposed**, and it is the one the reference implementation's
warning was really about. `_Static_assert`s for the two `[]`-declared tables landed in P0.

### 3.1 Indexing is unchecked

`m_catalog_ovl.c:170` is the one that matters, and the reason `furniture_quality` gets an assert:

```c
static void mCL_dma_furniture_program(mCL_Item_c* item) {
    item->profile = furniture_quality[item->ftr_actor.name];   /* no bounds check */
}
static void mCL_dma_furniture_bank(mCL_Item_c* item, mActor_name_t item_no) {
    aFTR_PROFILE* profile = item->profile;
    if (profile->vtable != NULL && ...)                        /* immediate deref */
```

An index past the end of `furniture_quality[]` reads a wild pointer and dereferences it on the
next line. **A short table is not a graceful failure; it is a crash or worse.**

### 3.2 Append-only, after `FTR_DUMMY`

New entries go **after `FTR_DUMMY`** (`include/m_ftr_def.h:1274`), never inserted earlier.
Inserting shifts every subsequent furniture ID, which silently rewrites the meaning of every
furniture reference already stored in a save, a house layout, a catalogue record, or the server's
journal. In a multiplayer context that is worse than a client crash — it corrupts the authoritative
town.

### 3.3 What this means for the design

Adding an item is not "three call sites". It is **one append to each parallel table plus two
enums**, with a hard invariant that they stay equal length. Any runtime scheme must either extend
all of them or intercept every one of their read sites.

The five `[FTR_NUM]`-declared tables make this cheaper than feared for the *build-time* tier
(T1.5) — a forgotten append there is a zero-filled entry, not corruption. It makes it no cheaper
for the *runtime* tier (T2), because growing a `[FTR_NUM]` array at load still requires
redirecting the symbol (§7.5 Option A).

---

## 4. Four tiers

| Tier | Capability | Requires a recompile? | Multiplayer-safe? | Cost |
|-|-|-|-|-|
| **T0** | Replace an existing texture | no | yes (per-client cosmetic) | **exists today** |
| **T1** | Replace any whole asset, same size | no | yes (per-client cosmetic) | ~1 week |
| **T1.5** | Add real new items, build-time | **yes** | only if every client ships the same build | proven, see §5 |
| **T2** | Add new items, runtime-loaded | no | yes | the real project |

**T1.5 is the pipeline described by the PlayStation-console mod, and it works today.** It is worth
documenting and supporting as a first-class tier rather than treating it as a stepping stone,
because it is the only tier that currently exists for *new* content and it is entirely adequate
for single-player and for an operator who ships a custom client build to their own players.

T2 is what multiplayer-at-large needs, because a build-time item requires every client in the town
to run a byte-identical binary — which is not a modding framework, it is a fork.

---

## 5. T1.5 — build-time item addition (the proven pipeline)

Documented from a working implementation (the PlayStation-console furniture mod) and verified
against this tree. Recorded here because it is the reference for what T2 must reproduce at
runtime, and because it is shippable now.

### 5.1 Convert the model, offline

```
tools/converters/obj2gfx.py  model.obj  --name int_foo
    parse OBJ → vertices, UVs, faces, materials
    if textured:  load, resize to power-of-two, convert to RGBA16
    else:         synthesise a palette texture; point every face's UVs at its colour cell
    apply --rotate-y / --fit transforms
    quantise positions to s16, UVs to the N64 ST format
    emit src/data/model/int_foo.c:
        static Vtx arrays
        static texture data
        Gfx int_foo_model[]  (gsSPVertex / gsSPNTriangles / …)
```

Output is a C file compiled into the executable. No runtime loading, no container format.

The untextured path — synthesising a palette texture and pointing each face's UVs at its colour
cell — is a genuinely good trick. It means a flat-shaded OBJ with per-material colours needs no
texture authoring at all, which removes the single biggest barrier for a first-time mod author.
**T2's converter should keep it.**

### 5.2 Append to every lockstep table

Per §3, in the same slot, all after `FTR_DUMMY`:

```
include/m_ftr_def.h                        += FTR_FOO
include/m_name_table.h  (enum ftr1_e)      += entry
src/actor/ac_furniture_profile_data.c_inc  += &profile        (which behaviour actor runs)
src/actor/ac_furniture_data.c_inc          += int_foo_model, size, rotation
src/data/item/ftr_price.c                  += price           (BEFORE the -1 terminator)
src/game/m_room_type.c                     += ×2 entries      (room-type / feng-shui class)
src/game/m_mark_room_ovl.c                 += entry
src/game/m_huusui_room_ovl_data.inc        += entry
src/game/m_catalog_ovl_data.c_inc          += catalogue row
src/actor/ac_my_room_data.c_inc            += entry
src/data/item/item_name.c                  += 16-byte name record
```

Verified detail on the price table: `ftr_price_table[]` is declared `unsigned short` but ends
`..., 0, -1, };` — a `0xFFFF` sentinel. A new price must be inserted **before** it, or any scan
that stops at the sentinel silently ignores the addition. This is the sharpest edge in the set,
because the failure is a wrong price rather than a crash.

**A build-time assertion that every per-furniture table has identical length is mandatory**, not
optional hygiene. Given §3.1, a missed table is undefined behaviour rather than a missing item.
`static_assert(ARRAY_COUNT(furniture_quality) == FTR_NUM, ...)` and siblings, one per table, is
the cheapest possible insurance and should be added **even before any modding work lands** — it
protects the existing tree too.

### 5.3 Name injection at runtime

`pc/src/pc_custom_furniture.c`, called at startup, writes the 16-byte space-padded record into
the item-name array at the new index (§2.7). Sanitise: the charset is the game's codepage, not
ASCII, and unmappable characters must fail loudly at startup rather than render as garbage.

### 5.4 Behaviour, only if interactive

The profile points at the generic furniture actor for a decorative item. An interactive one gets
a real actor — the PlayStation mod reuses the stock Famicom yes/no dialogue and fade path, then
branches in `famicom_emu_init()` on a sentinel ROM id.

One inherited hazard worth repeating: that sentinel is stored in an `s8`, so it must stay ≤ 127.
This is the kind of constraint that is invisible until it wraps.

### 5.5 Getting one in your pocket

Debug path: find an empty inventory slot, write `ITEM_BASE + new_index`. Fine for authoring;
**not** how a mod item should enter a multiplayer inventory — see §9.

---

## 6. T1 — asset override

### 6.1 Package layout

```
mods/
  lantern-set/
    mod.toml
    overrides/
      ftr_lamp01_tex.bin        # names match the generated asset table
      ftr_lamp01_v.bin
```

### 6.2 Loader change

`pc_assets.c` gains an override index built at startup by scanning `mods/*/overrides/`:

```c
/* Consulted first, before ROM. Keyed on the generated bin_path string,
 * which is already a stable unique name for every asset in the table. */
static const ModOverride* pc_mod_override_lookup(const char* bin_path);
```

`pc_load_asset` gains one branch ahead of the ROM read. The **size must match exactly**; a
mismatch is a load-time error naming the mod, not a silent truncation.

### 6.3 What T1 buys

Any vanilla item can be re-modelled and re-textured. Combined with the server-side
`furniture.define { base = ... }` sketch in `docs/netcode/MODDING_PLAN.md` §7, a mod can already
ship "a lantern that used to be a lamp" with correct pricing, availability and catalogue entry —
just occupying a vanilla ID.

**What it does not buy:** two mods cannot both override the same asset, and nothing is added.

---

## 7. T2 — the modloader subsystem

### 7.1 Components

```
pc/src/pc_modloader.c      discovery, manifest parse, load order, digest
pc/src/pc_mod_arena.c      dedicated asset heap, outside ARAM and the scene banks
pc/src/pc_mod_assets.c     .pcasset container reader → typed asset handles
pc/src/pc_mod_model.c      intermediate model format → runtime Gfx display list
pc/src/pc_mod_registry.c   handle → aFTR_PROFILE / tool profile / icon binding
pc/include/pc_modloader.h  the C API the decomp side calls (with no-op fallbacks)
```

### 7.2 Package layout

```
mods/lantern-set/
├── mod.toml                    same manifest as the server-side package
├── overrides/                  T1 assets (optional)
└── content/
    ├── lantern.pcasset         compiled model bundle
    ├── lantern_icon.png        32×32 inventory icon
    └── items.toml              declares handles and binds assets to them
```

```toml
# content/items.toml
[[furniture]]
handle   = "lantern"            # namespaced to lantern-set.lantern
model    = "lantern.pcasset"
icon     = "lantern_icon.png"
base     = "FTR_LAMP01"         # inherit footprint/shape/vtable defaults
scale    = 1.0
height   = 0.8
shape    = "square_1x1"
```

`base` matters: it lets a mod inherit the fields of `aFTR_PROFILE` it does not care about —
`vtable`, `move_bg_type`, `contact_action`, `interaction_type`. A mod overrides only what it
changes. This keeps `items.toml` short and, more importantly, keeps mods working when a field is
added to the struct.

### 7.3 The arena

Mod assets must **not** live in ARAM or the scene object-exchange heap:

- ARAM (`pc/src/pc_aram.c`) is a 16 MB **bump allocator with a no-op free**. Anything placed there
  is permanent and competes with the game's own resource budget.
- The scene object-exchange heap is bounded by `max_ram_address` and is reset per scene.

So: `pc_mod_arena.c`, a plain malloc'd region sized from the loaded mod set, with mod assets
resident for the process lifetime. On a 64-bit host with no 16 MB ceiling this is the simplest
correct answer, and it keeps mod content entirely out of the original memory budgets — which
matters because those budgets are the thing most likely to produce a subtle, hard-to-attribute
bug.

**One inherited constraint:** `pc/DOCUMENTATION.md` warns that PC heap pointers can collide with
N64 segment addresses, mitigated by a proximity heuristic plus a `VirtualAlloc`/`mmap` arena at
≥ `0x10000000`. The mod arena must be allocated from that same high arena, not plain `malloc`, or
mod pointers will be misread by `seg2k0` as segment addresses.

### 7.4 Model pipeline

The `.pcasset` container is a simple chunked binary — the point is that it is *not* a raw display
list, because hand-authoring GBI is not something to ask of mod authors:

```
PCAS magic, version
  VTX chunk   position/normal/color/uv arrays
  TEX chunk   RGBA8 (or a GC format), w, h, format tag
  PAL chunk   optional TLUT
  MSH chunk   material + triangle index ranges
  META chunk  bounds, suggested scale
```

`pc_mod_model.c` compiles this into runtime structures in the mod arena at load:

1. `Vtx` array — the same layout `pc_text_draw.c:90` writes.
2. Texture upload through the existing `pc_gx_texture.c` path.
3. A `Gfx` display list assembled with the normal macros; heap pointers pass through
   `pc_gbi_pack_runtime_ptr` (§2.2) so `seg2k0` resolves them.
4. `aFTR_PROFILE` fields filled: `opaque0`, `texture`, `palette`, `scale`, `height`, plus whatever
   `base` supplies.

**This is the largest single work item and the main schedule risk.** It also needs an offline
authoring tool — see §7.

### 7.5 Binding to the game — the honest version

An earlier draft claimed "three call sites". **That was wrong**, and §3 is why: a furniture item
is defined by ~11 parallel tables, so a runtime item must satisfy every one of their read sites,
not just the three that draw it.

There are two ways to do that, and the choice is the central design decision of T2.

**Option A — grow the tables at load.** Replace each static array with a pointer plus a count,
sized at boot to `FTR_NUM + mod_count` and copied from the static initialiser. Every existing
`table[index]` read keeps working unchanged.

- *For:* zero changes at read sites, so the ~11 unchecked indexers in §3.1 stay correct by
  construction. Behaviour is identical to T1.5, which is proven.
- *Against:* touches ~11 declarations in `src/`, each a merge conflict; needs a one-time
  allocation and copy per table; and the tables are declared `static` inside `.c_inc` files
  included into a parent TU, which constrains how they can be re-exported.

**Option B — intercept every read.** A `PC_FTR_TABLE(name, idx)` accessor macro that returns the
mod entry above `FTR_NUM` and the static entry below.

- *For:* no reallocation, no lifetime questions.
- *Against:* every read site must be found and converted. Missing one is exactly the §3.1 wild
  dereference, and there is no compiler assistance in finding them.

**Recommendation: Option A.** It converts an open-ended "did we find every read?" problem into a
closed "did we grow every table?" problem, and the latter is enforceable with the
`static_assert` set from §5.2 — which then does double duty. Option B's failure mode is a crash
in a rarely-visited overlay months later; Option A's is a build error.

Either way, three lookups remain for the asset side, with no-op fallbacks so a
`PC_ENHANCEMENTS`-off build is unaffected:

```c
/* include/pc_modloader.h */
const aFTR_PROFILE* pc_mod_furniture_profile(mActor_name_t item);  /* NULL if not a mod item */
int  pc_mod_item_icon(mActor_name_t item, u8** tex_out, u16** pal_out);
int  pc_mod_item_name(mActor_name_t item, u8* out, int out_len);   /* game codepoints */
```

**Revised decomp footprint:** ~11 table declarations plus 3 lookup sites, not 3 files. Still
hook-shaped and still mechanical, but roughly four times the estimate this plan originally
carried, and it should be scheduled as such.

### 7.6 ID space

Mod items need `mActor_name_t` values. That type is a `u16` with a 4-bit type nibble
(`ITEM_NAME_GET_TYPE`, `include/m_name_table.h:205`), giving 4096 IDs per type.

**Headroom in the `FTR0`/`FTR1` and `ITM_TOOL` ranges has not been surveyed and must be before
committing.** Two options:

- **Direct allocation** — carve a high sub-range per type for mods. Simple, fast, but risks
  colliding with unused-but-referenced vanilla IDs and burns real space.
- **Indirection** — mods get handles from a reserved band; a translation table maps handle →
  profile at the three call sites above. Costs a lookup, needs no contiguous space, and survives a
  vanilla ID range being denser than expected.

Indirection is the safer default. The lookups in §7.5 are already indirection; making the ID
itself a handle costs nothing extra.

**Caveat added after §3:** indirection solves the *asset* lookup but not the *table* problem — a
mod handle still has to index `ftr_price[]`, `furniture_quality[]` and the rest. Under §7.5
Option A the handle must therefore be a real index into the grown tables, which means the two
schemes collapse into one: allocate contiguously above `FTR_NUM`, and let "indirection" mean the
asset lookups only.

**The server assigns handles.** Per `docs/netcode/MODDING_PLAN.md`, the mod set is a property of
the town, so handle assignment must be too — otherwise two clients disagree about what item 0x3F01
is. The server's manifest digest covers the handle table.

---

## 8. Client delivery — server-served asset packs

The server owns the mods; the client must be able to *see* them without a manual install. This is
the delivery subsystem: a content-addressed pack store on the server, a resumable chunk protocol,
a global client cache, and a placeholder-until-resident render path so **nothing ever blocks on a
download**.

### 8.1 The governing principle

> **A missing asset is never fatal and never blocks.** It renders as its `base` vanilla model
> until the real one arrives, then swaps.

Everything else follows. A player with a cold cache joins immediately and watches the town fill
in; a player on a slow link plays the whole session with placeholders and loses nothing but
appearance. A join is never refused for content reasons, a download failure is never a
disconnect, and the loading screen is **optional UX**, not a gate.

This is the difference between a robust system and a fragile one, and it is worth defending
against the temptation to "just make them wait" — a join gate turns every network hiccup and every
oversized pack into a player who cannot get into their town.

### 8.2 Do not reuse the fragmentation path

`net/include/acnet/fragmentation.hpp` supports 16 MB transfers, 8 concurrent, with a 10-second
reassembly timeout. It is the wrong tool here:

| Fragmentation | Asset delivery needs |
|-|-|
| whole transfer buffered in RAM | disk-backed, streamed |
| all-or-nothing; timeout drops everything | resumable across sessions |
| 10 s window | minutes, over a slow link |
| server-driven push | client-paced pull |
| one message | thousands of chunks, deduplicated |

So: a **dedicated chunk protocol** on the existing `Channel::Bulk`. Fragmentation stays for what
it is good at — a single oversized message like a baseline.

### 8.3 Content addressing

Every asset is identified by the BLAKE3 (or SHA-256) hash of its bytes. Not by name, not by mod
id, not by version.

```
cache/                          ~/.local/share/AnimalCrossing/modcache/  (or %LOCALAPPDATA%)
  ab/
    ab3f9c…d21   asset blob, filename == its hash
  index.db       hash → size, last_used, source town (for LRU + diagnostics)
```

Consequences, all of them good:

- **Cross-town dedup.** Joining a second town that uses the same lantern model downloads nothing.
- **Verification is free.** The hash *is* the integrity check; a corrupted or tampered chunk fails
  to hash and is refetched. No signature scheme needed for integrity.
- **Cache is safe to share and safe to delete.** Worst case on deletion is a re-download.
- **Version churn is cheap.** A mod that changes one texture re-ships one blob, not the pack.

### 8.4 Protocol

Four new messages, all on `Channel::Bulk`, all reliable:

```cpp
enum class MessageType : std::uint16_t {
    /* … existing … */
    AssetManifest     = 60,   /* S→C: what this town needs, sent right after ServerHello */
    AssetChunkRequest = 61,   /* C→S: give me chunks [first, first+count) of blob H */
    AssetChunk        = 62,   /* S→C: one chunk */
    AssetStatus       = 63,   /* C→S: advisory progress, lets the server show operators a stall */
};
```

```cpp
struct AssetManifestEntry {
    std::array<std::uint8_t, 32> hash{};
    std::uint32_t size = 0;
    std::uint16_t kind = 0;        /* model | icon | texture */
    std::uint16_t item_handle = 0; /* which mod item this dresses; 0 = none */
};

struct AssetManifest {
    Revision revision = 0;
    std::array<std::uint8_t, 32> manifest_digest{};   /* same digest as MODDING_PLAN §8.3 */
    std::vector<AssetManifestEntry> entries;          /* <= kMaxAssetEntries */
};
```

**Client-paced pull.** The client asks for a bounded window of chunks and only asks for more when
the previous ones land. The server never pushes unrequested chunk data. This gives three things
for free:

- The client controls its own bandwidth — it can throttle when the player is in a busy scene.
- Resume is trivial: on reconnect the client asks for what it is missing.
- A slow or hostile client cannot make the server buffer anything.

Chunk size: `kMaxPlaintextPayloadBytes` (1136) minus the chunk header, so one chunk is one packet
and fragmentation is never involved.

Rate limiting reuses the existing token-bucket in `allow_message`
(`server/src/town_runtime.cpp:698`). `AssetChunkRequest` gets its own bucket, tuned so a full
window refill is comfortably possible but a request flood is not. **Gameplay traffic must always
win**: the asset channel is serviced only after transactions and snapshots for that tick.

### 8.5 Join sequence

```
ClientHello ──────────────────────────────────►
◄────────────────────────────── ServerHello (carries manifest_digest)
◄────────────────────────────── AssetManifest
   │
   ├─ client diffs manifest against its global cache
   │
   ├─ everything cached? ──► join immediately, no UI at all
   │
   └─ missing N blobs, M bytes:
         ├─ join immediately anyway  (§8.1)
         ├─ placeholders render for anything not yet resident
         └─ background fetch, window of ~16 chunks in flight
              └─ each blob completes → verify hash → install → hot-swap
```

The player is in their town before the first chunk arrives. That is the whole point.

### 8.6 Placeholder and hot-swap

This composes with the base-fallback design already in §7.5. The lookup gains a third state:

```c
const aFTR_PROFILE* pc_mod_furniture_profile(mActor_name_t item) {
    const ModItem* m = registry_find(item);
    if (!m) return NULL;                       /* not a mod item */
    if (!m->assets_resident) return m->base_profile;  /* placeholder */
    return &m->profile;                        /* the real thing */
}
```

**The one real hazard: cached profile pointers.** `mCL_dma_furniture_program`
(`m_catalog_ovl.c:170`) stores the result in `mCL_Item_c::profile` and dereferences it later, and
the furniture actor does the same. Swapping the pointer under a live actor risks a torn read of a
half-installed profile.

Mitigation, in order of preference:

1. **Install atomically, publish last.** Build the complete `aFTR_PROFILE` in the mod arena, then
   publish it with a single pointer store. A reader either sees the old (base) pointer or the new
   one, never a partial struct. On x86-64 and ARM64 an aligned pointer store is atomic; make it
   explicit with a relaxed atomic rather than relying on it.
2. **Swap at safe points only.** Apply pending swaps at scene transitions and when the submenu
   closes — both are already synchronisation points where nothing holds a profile.
3. **Never free the base profile.** It is static. So even a stale cached pointer stays valid; the
   item just keeps rendering as its placeholder until something re-reads.

Belt and braces: do 1 and 2, and rely on 3 as the backstop. The failure mode then degrades to "one
item renders as its placeholder for a few seconds longer", which is invisible.

A subtle nicety: because the icon, model and name are separate blobs, they can land independently.
An item can show its correct **name and icon** in the pocket while its world model is still a
placeholder — the parts that matter most for play arrive first if the client requests them first.
Priority order should be **name → icon → model**.

### 8.7 Loading screen

Optional, and off by default once the cache is warm. When the missing set is large enough to be
worth mentioning (proposed: > 2 MB or > 20 blobs), offer — not impose — a progress screen:

```
                    Lantern Town

              Downloading town content…
              ▓▓▓▓▓▓▓▓▓▓░░░░░░░░░░  6.2 / 11.4 MB

                 [ Play now ]  (placeholders)
```

"Play now" must always be present and always work. Implementation is the PC menu stack from
`docs/UI_SYSTEMS.md` §7 — `pc_text_draw` into `font_thaga`, `pc_menu_util` for layout, drawn from
`graph_main` like the pause menu. No decomp change.

A persistent, unobtrusive progress indicator (a small corner line, reusing the event-banner module
from `docs/CUSTOM_CONTENT_SYSTEMS.md` Example D) covers the background case.

### 8.8 Security

The threat is a malicious or compromised town server sending a client bad data.

**The most important property: this is data, not code.** Lua never leaves the server
(`docs/netcode/MODDING_PLAN.md` §1). A pack contains geometry, pixels and strings. That bounds the
worst case to a parser exploit rather than arbitrary execution, and it is worth protecting that
property deliberately — no bytecode, no scripts, no shaders in a pack, ever.

| Control | Mechanism |
|-|-|
| Integrity | Content addressing (§8.3). A blob that does not hash to its manifest entry is discarded and refetched. |
| Manifest authenticity | The manifest digest is echoed in `ServerHello`, so a MITM cannot swap the manifest without also breaking the session AEAD. |
| Size caps | Per-blob (proposed 4 MB), per-manifest (64 MB), per-town cache quota (256 MB). All enforced **before** allocating. |
| Entry caps | `kMaxAssetEntries` (proposed 4096) so a manifest cannot itself be a DoS. |
| Parser hardening | The `.pcasset` parser is the attack surface. It must be added to `make fuzz` alongside the protocol parsers — `net/CLAUDE.md` already requires new parsers to survive bounded garbage. |
| Resource bounds in the parser | Vertex/triangle/texture-dimension caps validated before allocation, not after. A 32-bit vertex count must not become a 64 GB malloc. |
| Trust prompt | First connection to a town that wants to send assets asks the player once, and remembers per-town. Default for an unknown town: **ask**. |
| Disk quota | LRU eviction from the global cache; a town cannot fill the disk. |
| No path from blob to filesystem | Blobs are stored under their hash. A manifest never supplies a filename, so path traversal is structurally impossible. |

**Deliberately not doing signatures in v1.** There is no PKI and no mod registry, so a signature
would only attest "the same server that you already trusted enough to join sent this". Content
addressing gives integrity; the trust decision is the join itself. Revisit if a public mod
repository ever exists.

### 8.9 Server side

```
towns/default/mods/<id>/content/*        authored files
        │  packed once at startup
        ▼
towns/default/modpack/
  <hash>            one file per blob, content-addressed
  manifest.bin      precomputed AssetManifest payload
```

Built at startup, not per connection: the manifest is computed once, and serving a chunk is a
bounded `pread` from a blob file. Serving costs the server essentially nothing per client, which
matters because a 16-player town all joining cold is the worst case.

An operator with a large pack should be able to point the manifest at an **external HTTP mirror**
instead (`asset_base_url` in `server.ini`), with the server still supplying the manifest and
hashes. Same integrity guarantee — the hash is what is verified, not the source — and it moves
the bandwidth off the game server. Worth designing the manifest to allow this from day one even
if the mirror support lands later.

### 8.10 What this replaces

The three-strategy table this section used to contain is superseded. **Base fallback is not an
alternative to server delivery — it is the mechanism that makes server delivery safe.** The
digest gate is dropped entirely; there is no longer a reason to refuse a client, because a client
with no content still plays correctly.

---

## 9. Authoring toolchain

Nobody will author `.pcasset` by hand. A converter is not optional, and it is the piece most
likely to be underestimated.

```
pc/tools/mod_pack.py
  glTF 2.0 / OBJ  →  .pcasset
    - triangulate, weld, split by material
    - clamp to engine limits (vertex count per DL batch, texture dimensions)
    - encode textures (PNG → RGBA8, or quantise to CI4/CI8 for memory)
    - emit bounds + suggested scale
    - validate against the target profile (footprint vs shape)
```

Python is consistent with the existing tooling (`gen_runtime_assets.py`, `gen_shader_seed.py`,
`.flake8` already configured). glTF as the input format because every DCC tool exports it.

Validation must be **loud and offline**. A mod that would blow the vertex batch limit or ship a
non-power-of-two texture should fail in the packer with a clear message, not produce corrupted
geometry three scenes into a play session.

---

## 10. Constraints

| # | Constraint | Consequence |
|-|-|-|
| 1 | Mod arena must come from the high `VirtualAlloc`/`mmap` arena (≥ `0x10000000`) | plain `malloc` pointers can be misread as N64 segment addresses |
| 2 | T1 overrides must match the original asset size exactly | destination arrays are fixed-size statics |
| 3 | Two mods overriding the same asset is a conflict | load order must be deterministic and the conflict reported |
| 4 | Mod assets must not enter ARAM or the scene object heap | ARAM is a 16 MB bump allocator with no free |
| 5 | Item ID headroom is unsurveyed | do the survey before choosing direct allocation over indirection |
| 6 | Handle assignment is server-owned | otherwise clients disagree about what an ID means |
| 7 | Decomp footprint is ~11 table declarations + 3 lookups (§7.5) | every `src/` edit is a future ac-decomp merge conflict |
| 8 | **New entries append after `FTR_DUMMY`, never insert** | inserting shifts every furniture ID and rewrites the meaning of saves, house layouts and the server journal |
| 9 | **Per-furniture tables are read without bounds checks** | a short table is a wild pointer dereference, not a missing item — see `m_catalog_ovl.c:170` |
| 10 | Item-name records are 16 bytes, space-padded, game codepage | not ASCII; unmappable characters must fail at load |
| 11 | Interactive-item sentinels stored in `s8` fields must stay ≤ 127 | the Famicom-ROM-id pattern used by the reference mod |
| 12 | `NDEBUG` on, `-O0` for decomp TUs | unchanged; the modloader lives in `pc/` and is not subject to this |
| 13 | Nothing ships in the release archive | `mods/` is user content, like `rom/` — never packaged |

---

## 11. Work inventory

| Phase | Deliverable | Size |
|-|-|-|
| **T0.1** | `static_assert` that every per-furniture table equals `FTR_NUM` | ~15 lines — **do this first, it protects the existing tree** |
| **T1.1** | Manifest parse, mod discovery, deterministic load order, conflict reporting | ~250 lines |
| **T1.2** | Override index + `pc_load_asset` branch + size validation | ~100 lines |
| **T1.5** | Document + support the build-time pipeline (§5); land `obj2gfx.py` | ~400 lines Python |
| **T2.1** | Mod arena on the high allocation arena | ~150 lines |
| **T2.2** | `.pcasset` container format + reader | ~400 lines |
| **T2.3** | Model compiler: container → `Vtx` + `Gfx` + texture upload | ~600 lines |
| **T2.4** | **Grow the ~11 lockstep tables at load (§7.5 Option A)** | ~400 lines, ~11 decomp decls |
| **T2.5** | Registry + the three asset lookups + no-op fallbacks | ~300 lines |
| **T2.6** | `mod_pack.py` glTF → `.pcasset` (reuses `obj2gfx.py`'s front half) | ~800 lines Python |
| **T2.7** | Server handle assignment + digest coverage | see modding plan |
| **T2.8** | Placeholder + atomic hot-swap in the profile lookup (§8.6) | ~150 lines |
| **T3.1** | Content-addressed pack store + startup packer (§8.9) | ~350 lines |
| **T3.2** | Four `Asset*` messages, encode/decode, bounds, round-trip tests | ~400 lines |
| **T3.3** | Server chunk service + rate bucket + gameplay-first scheduling | ~300 lines |
| **T3.4** | Client cache (hash store, index, LRU quota) | ~400 lines |
| **T3.5** | Client fetch loop: diff, windowed pull, verify, install | ~350 lines |
| **T3.6** | Loading screen + background progress indicator | ~250 lines |
| **T3.7** | `.pcasset` parser hardening + `make fuzz` integration | ~150 lines |
| **T3.8** | Trust prompt, per-town remembered decision | ~120 lines |

T0.1 is worth doing immediately and independently of any modding work. Given §3.1, the assertion
set is the difference between "a missed table fails the build" and "a missed table corrupts memory
in an overlay nobody opens during testing" — and it costs an afternoon.

### Sequencing against the server-side plan

```
T0.1                 table static_asserts                  ── do first, protects the tree today

MODDING_PLAN 1a-1e   server VM, holidays, replication      ─┐
MODLOADER  T1        asset override                         ├─ independent, parallel
MODLOADER  T1.5      build-time pipeline + obj2gfx         ─┘

MODLOADER  T2.1-2.6  arena, format, compiler, tables       ─┐
MODDING_PLAN 3       custom furniture (server-side data)    ├─ must land together:
MODLOADER  T2.7-2.8  handles, placeholder, hot-swap        ─┘  first feature needing both halves

MODLOADER  T3.1-3.5  pack store, protocol, cache, fetch    ─┐
MODLOADER  T3.7      parser hardening + fuzz                ├─ delivery; T3.7 gates T3.5 shipping
MODLOADER  T3.6, 3.8 loading screen, trust prompt          ─┘  polish, can trail
```

Three observations on ordering:

- **T0.1 is unconditional.** It protects the existing tree whether or not any modding work
  proceeds.
- **T2 must land before T3.** Delivery has nothing to deliver until runtime assets exist, and
  T2.8's placeholder path is the precondition that lets T3 be non-blocking (§8.1).
- **T3.7 gates T3.5.** Do not ship a client that parses network-delivered `.pcasset` data before
  that parser has been through `make fuzz`. This is the one ordering constraint in the plan that
  is a security requirement rather than a convenience.

---

## 12. Open questions

1. **Is T1.5 enough for the first mods?** The build-time pipeline already works and produces real
   new items. If the near-term audience is single-player players and operators who ship their own
   client build, T2 buys combinability and no-recompile installs — valuable, but not the same
   order of urgency. Worth asking the intended mod authors before committing to T2.4, which is the
   expensive irreversible piece.
2. **Animated mod models.** `aFTR_PROFILE` carries `rig` (`cKF_Skeleton_R_c` + `cKF_Animation_R_c`)
   and `tex_anim`. Static geometry first; skeletal animation is a second format problem and should
   not gate v1.
3. **Sound.** Not surveyed. The audio path is `pc_audio.c` + the JAudio tables and is likely a
   harder injection problem than geometry.
4. **Do mod items persist in a save?** They occupy inventory slots and house layouts. If a player
   removes the mod, the server still holds the handle. Proposal: the server degrades unknown
   handles to their `base` ID on read, so removing a mod downgrades items rather than voiding
   them — but this needs to be decided before any mod item is committed to a journal.
5. **Signing / trust.** Addressed in §8.8: content addressing gives integrity, and the trust
   decision is the join itself. Revisit only if a public mod repository ever exists.
6. **Should the client cache be global or per-town?** §8.3 proposes global, for cross-town dedup.
   The counter-argument is privacy: a shared cache lets town B observe (by timing) that you have
   an asset only town A serves. Low stakes, but it is a real fingerprinting channel and the
   mitigation — partitioning the cache per town — costs the dedup win. Worth a deliberate
   decision rather than a default.
7. **Pack rebuild while players are online.** `docs/netcode/MODDING_PLAN.md` §4 forbids hot mod
   reload for the Lua side. The pack store inherits that: rebuilding blobs under a live manifest
   would invalidate hashes clients are mid-download on. Reload is a restart, and the manifest
   revision exists so a client can detect it happened.
8. **Bandwidth policy for a 16-player cold join.** §8.9 makes serving cheap, but sixteen clients
   pulling 11 MB simultaneously is ~180 MB off a home connection. The external-mirror escape
   hatch is the answer; the open question is whether a server should be able to *require* clients
   use it, and what happens to a client that cannot reach it (answer should be: placeholders
   forever, which is fine).
