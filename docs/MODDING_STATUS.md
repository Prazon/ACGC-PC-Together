# Modding — implementation status

Branch `worktree-modding-p0-p1`. Tracks what is actually built against the phase map in
`docs/MODDING_IMPLEMENTATION.md` §1.

| Phase | State | Commit |
|-|-|-|
| **P0** table `static_assert`s | **done** | `8122b17` |
| **P1** Lua vendored, host, sandbox | **done** | `5ec2241` |
| **P2** calendar API + holidays | **partial** — resolver and Lua API done; query/effect side and runtime wiring outstanding | `c5f2538`, `bb18bee` |
| **P3** `ModCalendar` replication | not started | |
| **P4** T1 asset override | not started | |
| **P5** arena, `.pcasset`, model compiler | not started | |
| **P6** table growth, registry, placeholder | not started | |
| **P7** delivery, server side | not started | |
| **P8** client cache, fetch, loading UI | not started | |
| **P9** custom songs and discs | not started | |

Gate at time of writing: `make check` exits 0, 69/69 tests, `client_link` pass at 20 objects.

---

## What works today

A mod directory under a town's `mods/` is discovered, validated, sandboxed, loaded, and can
declare holidays that resolve to real dates:

```
mods/lantern-night/
  mod.toml     id, version, api_version, entry
  init.lua     calendar.register { ... }
```

Nothing consumes the resolved calendar yet — that is P2's remaining half plus P3.

## Corrections made to the plan while building

Both are recorded in the docs they affect, and both were the result of measuring rather than
trusting a prior description.

1. **The lockstep-table survey was wrong.** `FTR_NUM` is 1266, not 1267, and only **two**
   furniture tables are declared `[]` — `furniture_quality` and `ftr_price_table`. The other five
   are `[FTR_NUM]`, so their declaration enforces the length. P0 is 2 assertions, not the ~11 the
   plan predicted.
2. **`packaging/server.ini` still said `capacity = 16`** while the code default had already moved
   to 4, so an operator using the shipped template got a sixteen-player town. The 1.0 scoping was
   only half applied; fixed in `5ec2241`.

## Things a future phase must not forget

- **Protocol is already v21.** `MODDING_IMPLEMENTATION.md` §6.1 says P3 bumps 20 → 21; a parallel
  change took 21 first, so **P3 must use v22** and update `docs/netcode/PROTOCOL.md` in the same
  commit.
- **`music_box` is 64 bits with 55 used** — 9 spare song slots before P9 forces a save and wire
  change. See `MODDING_IMPLEMENTATION.md` §11b.2; bundling the widening with
  `EXTENDED_RESIDENTS_PLAN.md` is probably right rather than paying for it twice.
- **P8 is blocked on fuzzing the `.pcasset` parser.** It is the first point where the client parses
  bytes an arbitrary server chose.
- **Lua must never enter `NET_SOURCES`.** `make client-link` is the guard; it links exactly the
  objects the shipped client is made of.

## Verifying the sandbox by hand

```sh
mkdir -p /tmp/t/mods/probe
printf 'id = "probe"\nversion = "1"\napi_version = 1\n' > /tmp/t/mods/probe/mod.toml
printf 'if io or os or require then error("sandbox leak") end\n' > /tmp/t/mods/probe/init.lua
```

Loading this must succeed. If it errors, a library that should not exist is reachable — check
`third_party/lua/VENDORING.md` for what is deliberately not vendored.
