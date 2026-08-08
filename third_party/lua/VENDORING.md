# Vendored Lua 5.4.7

Upstream: https://www.lua.org/ftp/lua-5.4.7.tar.gz — the `src/` directory, MIT licensed
(copyright notice at the end of `lua.h`).

## Server-only

These sources link into `AnimalCrossingServer` and `netcode_tests` **only**, never into
`AnimalCrossing`. `net/CLAUDE.md` forbids adding a dependency to `net/` that the client does not
already link, and the client never executes mod code — see `docs/netcode/MODDING_PLAN.md` §1.
`make client-link` is the guard that proves the client still links without server-only
translation units.

## Deliberately removed

Seven upstream files are **not** vendored. Removing them at the build level rather than hiding
them behind an environment whitelist means an accidental `luaL_openlibs` cannot reintroduce them
— the symbols do not exist to link against.

| File | Why |
|-|-|
| `liolib.c` | filesystem access |
| `loslib.c` | process, environment, and **wall-clock time** — mods read town time only |
| `loadlib.c` | dynamic loading, `require` |
| `ldblib.c` | debug library; can defeat every other sandbox control |
| `linit.c` | opens all of the above; the host builds its environment explicitly instead |
| `lua.c` | standalone interpreter `main()` |
| `luac.c` | bytecode compiler `main()` |

`lbaselib.c` **is** vendored, but the host installs only a whitelist of its functions —
`load`, `dofile`, `loadfile`, `collectgarbage`, `rawset`/`rawget` and friends are omitted at
environment-construction time. See `server/src/mod_host.cpp`.

## Build

Compiled with its own flags (`LUA_CFLAGS` in the root `Makefile`), not the project's
`-Werror -Wpedantic` set — upstream Lua is not clean under those and patching it would create a
merge burden on every future update.

## Updating

Replace `*.c`/`*.h` from a new upstream tarball, re-apply the removal list above, and re-run
`make test`. Do not patch vendored sources; if a change is genuinely needed, wrap it in the host
layer instead.
