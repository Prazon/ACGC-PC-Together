# The server window

Status: **planned** (2026-08-08).

Goal: a host who wants to run a town double-clicks one thing, gets a window
that shows the town — console log, a 2D map, town info, residents, visitors,
villagers, live metrics — and can perform the operator actions that today are
one-shot command-line flags. A host who wants a *dedicated* server keeps
exactly what they have now: `AnimalCrossingServer --console`, an 80-column
ANSI dashboard, no window, no GUI dependency.

Non-goal: replacing the console. The ANSI dashboard in `main.cpp` is good and
stays. The window is an alternative front end, not a successor.

## 1. Shape

Two processes.

```
  AnimalCrossingServer            (C++, unchanged responsibilities)
    ├─ TownRuntime tick loop      authoritative; never blocks on the GUI
    └─ AdminEndpoint              non-blocking TCP on 127.0.0.1:24681
             │  newline-delimited JSON
             ▼
  acgc-server-window              (Rust + egui, tools/server-window/)
    ├─ launches and supervises the server as a child, or attaches to a
    │  running one
    └─ draws the window
```

The decision to split the process — rather than link `acserver_core` into an
ImGui window in-process — was taken deliberately, against the cheaper option.
Three reasons carried it:

1. **A GUI fault cannot take a town down.** The window renders; the town runs.
   These have very different failure tolerances and now have different address
   spaces.
2. **The endpoint is the remote-admin story.** The same protocol served over
   something other than loopback is a remote panel, with no second design.
3. **The window can own the launch.** Because it is the parent, it can start,
   restart and stop the server, and can show the town's own stdout in the log
   viewport without the server having to log twice.

The cost is real and is paid once, here: the runtime's state lives in C++
objects, so it has to be serialized. §2 confines that cost to a single struct.

## 2. `ServerSnapshot` — the only boundary

Everything the window can ever show passes through one struct, built once per
publish from `TownRuntime`'s existing const accessors, and serialized by one
function.

```
server/include/acserver/admin_snapshot.hpp   ServerSnapshot + build_snapshot()
server/src/admin_snapshot.cpp                the JSON writer
```

This is the discipline that keeps the split from metastasizing. No panel
reaches past the snapshot; no serialization lives anywhere else. Adding a
field to the window is: add a field to the struct, write it, read it. If the
protocol is ever versioned or re-hosted, exactly one file changes.

JSON is hand-written — a few hundred lines of writer, no new third-party
dependency, and it must compile clean under the tree's
`-Wall -Wextra -Wpedantic -Werror`.

### What the snapshot carries

Already exposed on `TownRuntime`, needing no new accessor:

| Section | Source |
|-|-|
| Metrics | `metrics()` |
| Clock, weather | `clock_state()` |
| Players, residents, visitors | `player_statuses()`, `connected_*()` |
| Log events | `recent_events()` |
| Island | `island_status()` |
| Economy | `turnip_market()`, `shop_tier()`, `shop_sales_sum()` |
| Accounts | `account_summaries()` |
| Villager summary counts | `villager_count()`, `villagers_leaving()`, … |
| Map tiles | `tile(zone, x, z)` |

Needing a new const accessor (§3):

- the villager roster itself, for names, species, mood, and house position
- NPC states, for positions and conversation ownership
- zone occupancy, for who is where
- bulk exterior tile iteration, because per-tile `tile()` calls across a
  7×10×16×16 grid is 125k virtual calls a frame

## 3. Server-side work

### 3.1 Accessors

Add const accessors to `TownRuntime` for the four items above. All are
read-only views over state the class already holds; none introduce authority.

### 3.2 The event ring

`record_event` currently keeps 12 entries (`town_runtime.cpp:201`), tuned to
the dashboard's 8 visible rows. The window wants scrollback. Widen the ring to
512 and let a client poll with `since_sequence` — events already carry a
monotonic `sequence`, so the client accumulates unbounded history locally and
the server's memory stays bounded. The console dashboard still reads the last
8 and is unaffected.

### 3.3 `AdminEndpoint`

`server/src/admin_endpoint.cpp`. A non-blocking TCP listener, **polled once
per tick from the main loop**. No threads, therefore no locking around runtime
state, therefore no way for the window to stall or race the tick. The tree has
no TCP abstraction — only `acnet::UdpSocket` — so this brings its own socket
handling, reusing the winsock/BSD split and `WinsockLifetime` pattern already
established in `net/src/transport.cpp`.

Protocol: newline-delimited JSON, one object per line, request/response.

```
→ {"cmd":"auth","token":"…"}
← {"ok":true,"protocol":1}
→ {"cmd":"snapshot","since_sequence":417}
← {"ok":true,"snapshot":{…}}
→ {"cmd":"grant_bells","account":1,"amount":5000}
← {"ok":true,"message":"…"}
```

Budget: a client polls at 4 Hz, matching the console dashboard's repaint rate.
Snapshot assembly is a memcpy-scale walk over live structures; the map section
is sent only when its revision changes rather than every frame.

### 3.4 Authentication

The endpoint performs operator actions — granting bells, posting mail, banning
accounts — so it is authenticated even though it binds loopback by default.

On startup the server writes a 256-bit random token to
`<data_directory>/admin.token`, mode 0600 where the platform has modes. The
window reads that file when it can (it launched the server, or it runs on the
same box), so the common path needs no configuration at all. An operator
attaching from elsewhere copies the token.

Unauthenticated connections may send nothing but `auth`. Bind address is
configurable; anything other than loopback should be behind a tunnel and the
config comment will say so.

### 3.5 Config and flags

New `server.ini` section, with matching CLI overrides:

```ini
[admin]
; The server window connects here. Loopback only unless you know why not.
enabled = true
bind = "127.0.0.1"
port = 24681
```

`--console` forces the ANSI dashboard and is the documented dedicated-server
invocation. Today's `--no-dashboard` (plain log) is untouched.

### 3.6 Commands

The window exposes the operator actions that already exist as one-shot flags,
routed through the same `TownRuntime` methods — `grant_bank_bells`,
`send_mail`, `grant_house_song`, `set_shop_sales`, `set_account_banned`,
`checkpoint_now`, `import_gci`, `export_gci`. These already commit through the
audited authority, journal and transaction path, so exposing them is wiring,
not new authority. No command is added that does not already exist.

One behavioural difference worth stating: the one-shot flags run with the town
stopped. Through the endpoint they run against a live town. Every one of them
is already safe to call mid-tick — they are the same calls the tick loop makes
in response to player requests — but the window will confirm before any of
them, because a mis-click on a live town is not recoverable by closing the
window.

## 4. The window

`tools/server-window/`, Rust, `eframe`/`egui`. Chosen over iced for immediate
mode: the content is a 4 Hz refresh of a snapshot, which is precisely what
immediate mode is good at, and the custom-painted map is far less code against
egui's painter.

### 4.1 Layout

```
┌────────────────────────────────────────────────────────────────┐
│  NetTown            ● ONLINE   24680    up 04:21:07     ⏻ ⟳    │
├──────────────┬─────────────────────────────────┬───────────────┤
│  TOWN        │                                 │  RESIDENTS    │
│  time        │                                 │  [1] …        │
│  weather     │          2D TOWN MAP            │  [2] …        │
│  world       │      acre grid, 7 × 10          │               │
│  data dir    │                                 │  VISITORS     │
│              │      · villager houses          │               │
│  ECONOMY     │      · player dots              │  VILLAGERS    │
│  shop tier   │      · trees, holes, items      │               │
│  turnips     │                                 │               │
│              │                                 │               │
│  METRICS     ├─────────────────────────────────┤               │
│  tick / rx   │  CONSOLE LOG          [scroll]  │               │
│  tx / snaps  │                                 │               │
└──────────────┴─────────────────────────────────┴───────────────┘
```

Minimalist: one accent colour, one type scale, generous whitespace, no
chrome that does not carry information. Dark by default, because it sits open
next to a game.

### 4.2 The map

The server knows the town's *contents* but not its *art*. Acre kinds and
terrain artwork are client-side; what the server holds is the tile grid, the
villagers' house coordinates, and live positions. So the map is a schematic,
and an honest one:

- the 7 × 10 acre grid from `zone.hpp`, 16 units per acre (`kBlockUnits`)
- villager houses plotted from `home_block_x/z` + `home_ut_x/z`
- players from `player_transform()`, labelled, resident and visitor distinct
- NPCs from the NPC states
- per-tile marks for `TerrainState` — tree, stump, hole, planted — plus
  dropped items, buried items and placed furniture
- the island drawn as an inset when `island_status().terrain_ready`

A town that has not been bootstrapped yet draws the empty grid and says so,
the same way the console prints `AWAITING FIRST RESIDENT`.

### 4.3 Log viewport

Two sources, merged on one timeline: structured events from the endpoint
(`sequence`-cursored, so nothing is missed or duplicated across polls) and,
when the window launched the server, the child's raw stdout. Filterable,
searchable, with a follow-tail toggle. Scrollback is client-side and bounded
by memory, not by the server's 512-entry ring.

### 4.4 Launch and supervise

The window's landing state is a small launcher: pick or edit a `server.ini`,
Start. It spawns `AnimalCrossingServer`, reads the token, connects, and
switches to the dashboard. Stop sends a clean shutdown and waits for the
checkpoint. It can also attach to a server it did not start, given host, port
and token.

## 5. Build and packaging

`cargo` is a new build input. It is **not** made a prerequisite of the C++
build: `build_pc.bat` builds the server and client exactly as it does today,
and the window is a separate, optional target. Someone who never wants the GUI
never installs Rust. Packaging ships the built binary alongside when it is
present.

## 6. Testing

- Snapshot serializer: golden-JSON round trip over a synthetic runtime state.
- Endpoint framing: partial lines, oversized lines, multiple commands per
  read, unauthenticated command rejection, client disconnect mid-response.
- Auth: wrong token, missing token, token file permissions.
- The tick loop must be shown not to stall on a stuck client — a client that
  connects and never reads must not back-pressure the town.

## 7. Phases

| # | Phase | Delivers |
|-|-|-|
| 1 | Accessors + event ring | Server-side data reachable |
| 2 | `ServerSnapshot` + JSON | The boundary, with tests |
| 3 | `AdminEndpoint` | Auth, framing, snapshot polling |
| 4 | Commands | Operator actions over the wire |
| 5 | Config + `--console` | Dedicated-server path documented |
| 6 | Rust scaffold | Connect, attach, launch, supervise |
| 7 | Panels | Info, residents, visitors, villagers, metrics, log |
| 8 | Map | The 2D town view |
| 9 | Actions UI | Forms and confirmations |
| 10 | Build, packaging, docs | `crossing-servers.md` |

Phases 1–5 leave the tree shippable on their own: a server with a documented,
authenticated status endpoint and an unchanged console, which is useful even
if the window never lands.

## 8. Deferred

- Remote administration over anything but a tunnel. The protocol allows it;
  the defaults do not encourage it, and there is no TLS here.
- Live `server.ini` editing. Several fields cannot safely change mid-run and
  sorting which is a separate piece of work.
- Multi-town views. One window, one town.
- Any write path that does not already exist as a command-line flag.
