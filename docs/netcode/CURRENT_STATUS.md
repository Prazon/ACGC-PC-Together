# Dedicated Town Netcode Status

Last updated: 2026-08-07

## Overall

The initial dedicated-town roadmap is implemented. The shipped configuration
is one authoritative town per process, four original resident slots, visitors,
and up to sixteen concurrent encrypted UDP sessions. Zones are isolated logical
workers inside the portable server; they do not share or transmit original
game-process memory.

The PC client and server have one supported ABI: 64-bit. Windows builds use
native MinGW-w64 CMake plus Ninja and produce PE32+ x86-64 executables. CMake
and the C headers reject a non-64-bit build.

## Delivered systems

- Explicit protocol v16 codecs, selective reliability, sequencing,
  fragmentation, authenticated encryption, invitation proofs, rate limits,
  reconnect credentials, bounded parsing, and stable generated entity IDs.
- Client-authoritative original movement/collision with finite bounded transform
  relay and no server pullback. Remote presentation uses buffered interpolation,
  render-rate position/facing smoothing, zone-reset teleport handling, the real
  boy/girl player skeleton, and replicated face/clothing resources.
- Furniture push/pull now treats the original owner-side animation as a local
  prediction and submits its completed candidate layout immediately for server
  validation. Player action IDs ride the existing transform field, buffered
  remote transforms advance continuously between snapshots, and remote avatars
  use the original push/pull animations. An accepted adjacent layer-zero move
  glides its furniture actor to the authoritative cell; rejected owner moves and
  visitor-only predictions glide back after both original furniture/player
  state machines settle. Identical authoritative layouts no longer rebuild the
  room's furniture actors, removing the mid-animation teardown that could leave
  push/pull state applying root motion out of bounds.
- Atomic, revisioned, idempotent world, inventory, bells, banking, debt, shop,
  donation, mail, escrow trade, furniture, tool, fish, and insect operations.
- Mail is a complete server-authoritative round trip that follows the original
  game's two steps. Each account owns a house mailbox and a set of carried
  letters, both bounded at ten (`HOME_MAILBOX_SIZE` and
  `mPr_INVENTORY_MAIL_COUNT`), under one shared revision. `AttachMail` posts a
  letter, `TakeMail` moves it from the mailbox into the player's hands,
  `ClaimMail` takes the present out of a carried letter and leaves the letter
  behind with an empty attachment, and `DiscardMail` throws away a letter that
  no longer holds a present -- refused while one does, since that would destroy
  the item. Every operation checks ownership, the observed mail revision, and
  capacity, and is idempotent under replay; a full mailbox is refused with
  `Capacity` rather than consuming the item, and self-addressed mail is refused.
  Item conservation holds across the whole path. A letter carries its whole
  original content -- location, font, mail and paper type, sender name, header,
  body, footer -- as opaque bytes in the game's own encoding, so it projects
  into the original letter UI without reinterpretation. The viewer's letters
  ride the `Baseline`, and account-targeted `ResourceKind::Mail` deltas keep
  them live between baselines.
- Mail is wired into the original UI. The house mailbox array
  (`Save_Get(homes[slot]).mailbox`) and the carried mail array
  (`Now_Private->mail`) are projections rebuilt whenever the mail revision
  moves, so the mailbox flag rises for an authoritative letter and the letter
  reads normally. `mTG_trans_mail`/`mTG_trans_mail_mark` send `TakeMail` instead
  of copying locally, and `mTG_present_proc` sends `ClaimMail`. Because the
  server puts the present straight into a pocket slot, online it arrives in the
  inventory rather than in the drag hand -- the one visible difference from the
  original.
- Picking a letter up into the drag hand is refused online
  (`mTG_catch_item_from_table`). Rearranging letters, moving them to card
  storage, and attaching an item to a letter are not supported yet, and letting
  the drag appear to work would empty a slot locally only for the next
  authoritative refresh to undo it -- the same two-writer bug that was just
  removed from banking. Refusing the drag makes it inert instead.
- Throwing a letter away is wired (`mTG_move_delete`, all three of its paths:
  one letter, marked carried letters, marked mailbox letters), so a full pocket
  is no longer a dead end. It had been left with a server operation and a C API
  entry point but no call site, which meant ten carried letters online could
  never be reduced and every further `TakeMail` would fail with `Capacity`. A
  letter still holding a present is refused rather than destroying the item with
  it -- the present has to be taken out first. Covered by a regression test that
  fills the pocket, sees the refusal, discards, and retries.
- `EconomyResult` echoes its operation type, and the client routes
  `auxiliary_revision` by it: the ledger for Deposit/Withdraw/PayDebt, the shop
  for Buy/Sell, the mailbox for the mail operations. It previously copied every
  result's `auxiliary_revision` into the cached ledger revision, so a shop
  purchase left the bank mirror pointing at the shop's revision and the next
  deposit would have been rejected as stale. Pinned by the mail-claim case in
  "operator gifts survive a restart", confirmed to fail with the routing removed.
- Banking is wired into the original UI. The ABD overlay (`m_bank_ovl.c`) and the
  post office loan overlay (`m_repay_ovl.c`) used to commit the bank balance,
  the loan, and the wallet straight into the save while the authoritative apply
  overwrote all three, so the two writers fought and an online deposit was
  reverted. Online they now send `Net_RequestBankTransfer` / `Net_RequestPayDebt`
  and leave the save alone; the accepted result moves the money. Both overlays
  are clamped to wallet bells online (`Net_BankingAuthoritative`), because their
  local commit also converts money-sack items and the server ledger only moves
  `inventory.bells` -- so no sack is created or destroyed behind the server's
  back. The consequence is a real narrowing: bells held as sacks above the
  99,999 wallet cap cannot be deposited online until the server models sacks as
  currency.
- The bank ledger has its own revision watermark in the authoritative apply.
  Balance and debt used to refresh only when the *inventory* revision changed,
  so an operator `--grant-bells`, which touches the ledger and nothing else,
  never reached the save.
- Startup replay is idempotent for mail. `decode_state` runs twice at startup
  (checkpoint, then the newest journalled state) on the same authority, and mail
  is the one collection restored by appending rather than by key, so the mail
  set is dropped before each restore. Without that, a letter present in both
  payloads was rejected as a duplicate and the server refused to start after a
  crash, and a letter claimed between the checkpoint and the crash would have
  come back to life and been claimable a second time. Covered by the restart
  case in "operator gifts survive a restart", which was confirmed to fail with
  the reset removed.
- Operator gifts: `--list-accounts`, `--grant-bells ACCOUNT=AMOUNT` (straight
  into the bank, never the pocket), and
  `--send-mail ACCOUNT [--mail-item ITEM] [--mail-text TEXT]`. They are one-shot
  commands like `--ban`, and they are not a side channel around authority: they
  run through `EconomyAuthority`, bump the same ledger and mailbox revisions,
  append the town state to the journal before reporting success, and write an
  `audit_log` row. The `AdminGrantBells`/`AdminSendMail` operation types sit
  above `kMaximumClientEconomyOp`, so the request codec and
  `EconomyAuthority::apply` both refuse them from a client.
- Conversation leases, multiplayer player queries, NPC schedule state, signed
  zone transfers, public interiors, and four persistent resident houses. House
  zones derive from the visited house owner (not the visitor's resident slot),
  so multiple clients remain visible together indoors and after leaving.
  Full three-floor room state now replicates furniture layout/facing, switch
  bits, lights, and music through the original furniture actors. Post-bootstrap
  furniture additions/removals reconcile atomically with authoritative
  inventory, while moves/rotations conserve the item multiset and forged items
  or free size changes are rejected.
- Each authoritative interior `main_light_on` bit drives its original exterior
  light; the original renderer retains its day/night presentation. Door
  handoffs replicate leaving/arriving phases plus a short source ghost, wait for
  the destination scene, and carry its exact doorway transform. Peers therefore
  run the original house-door keyframes and entrance motion without observing a
  placeholder-position teleport.
- Original-game hooks for startup/frame/scene/actor lifecycle, remote actors,
  authoritative inventory/clock/weather/foreground application, coordinated
  doors, pickup/drop/dig/fill/bury/plant/chop, and fishing/net outcomes.
- Authenticated pre-boot town identity, deterministic canonical foreground
  bootstrap, server-assigned resident slots, and first-resident persistence.
  Online Rover keeps player name/gender but skips local time/town naming; the
  first face/outfit are randomized per town resident slot. Starter outfit rolls
  span all 255 stock designs with a coprime slot stride, guaranteeing the four
  resident slots receive distinct starting shirts. The train
  arrival remains synchronized, then Tom Nook's house/tutorial onboarding is
  completed in save state and skipped, with the assigned house ready at once.
- Append-only CRC journal, torn-tail repair, atomic rotating checkpoints,
  SQLite WAL metadata/migrations/audit/bans, semantic native GCI import/export,
  clean-shutdown markers, and restart replay.
- Authoritative IANA timezone clock, DST handling, weather, empty-town hourly
  NPC jobs, daily shop/growth renewal, catch-up limits, and accelerated-month
  restart/crash coverage. The optional `[clock] sync_to_system_clock` INI flag
  (default `false`) slaves town time to the host system clock plus the timezone
  offset on every advance, overriding `clock_mode`, `clock_scale`,
  `starting_datetime`, a persisted town's stored time, and drift from an
  administrative time set; the server prints a startup notice when it overrides
  one of those. `realtime` mode with no `starting_datetime` already tracked the
  system clock, so the flag exists for seeded, scaled, frozen, or manually
  nudged towns. A backwards host clock is still gated by `allow_time_travel`.
- Town-wide occupancy: `town_population`/`town_capacity` ride the v8 `Baseline`
  next to the clock and weather fields, and `ResourceKind::Town` deltas (2-byte
  payload, published whenever the connected count changes) keep the pair live
  between baselines, since baselines only arrive on join and zone transfer.
  Like `Clock`/`Weather`, a `Town` delta bypasses zone and distance interest.
  Population 0 means "not reported"; codecs reject `capacity == 0` and
  `population > capacity` in both directions. Exposed as
  `acnet_client_town_population()`. Landed inside commit `5ca4845`, whose
  message covers only the shared-house work.
- Discord Rich Presence reflects online sessions: connection state comes from
  `acnet_client_status()`, the town name from the authoritative
  `acnet_client_town_name()` with a local-save fallback, and the player count
  from `acnet_client_town_population()` ("N of M in town" / "alone in town"),
  falling back to the interest set ("with N others nearby") only when a server
  reports no population. Read-only and client-side; it sends no host, account,
  town ID, or invitation key to Discord. Wording lives in a pure
  `pc_discord_compose()` (`pc/src/pc_discord_text.c`) and is asserted by
  `scripts/smoke_online_windows.ps1` against a real server.
- Operator configuration, one-shot checkpoint/ban/import/export commands,
  a four-Hz Windows operator console with resident slots, town time/weather,
  traffic/errors and recent activity, structured lifecycle metrics, source and Windows server smoke tests, and an
  asset-free x86-64 release packager with SHA-256 manifests.
- The operator console repaints without flicker. It used to erase the screen
  and then draw, and the two halves reached the console as separate writes —
  `std::unitbuf` split the POSIX `ED 2` from the frame body, and the Windows
  path blanked the whole screen buffer with `FillConsoleOutput*` before
  `WriteConsoleA` — so a blank frame was presented four times a second. Frames
  are now drawn over the previous one from the home position, each row erasing
  its own tail (`EL`) with a single `ED 0` for the leftovers, emitted as one
  write and wrapped in synchronized output (DECSET 2026). The full-screen wipe
  runs once, on the first frame. Windows enables
  `ENABLE_VIRTUAL_TERMINAL_PROCESSING` and shares the POSIX path; pre-VT
  conhost falls back to space-padded rows written from the origin. The visitor
  and activity sections are padded to fixed row counts (4 and 8) so the layout
  cannot shift vertically between frames.
- The PC overlay menu (`pc/src/pc_pause_menu.c`) no longer stops the simulation
  offline. `Game_play_move` used to return early on `g_pc_paused &&
  !Net_IsConnected()`, so an offline town froze while the overlay drew over a
  static frame; online towns already kept running. Both cases now keep
  simulating and rendering behind the overlay, and only `padmgr` input
  suppression plus the release drain (`g_pc_pause_input_drain`) remain, so the
  local player idles instead of acting on menu presses. The header no longer
  reads "- Paused -" offline.
- **Open issue — submenus still freeze the screen online (attempted and
  reverted).** Only the *simulation* is unpaused today: the `online_menu` gates
  in `Game_play_move_fbdemo_not_move` keep actors, events, camera, and the clock
  advancing, but `makeBumpTexture` copies the EFB to `prbuf` on
  `mSM_MODE_PRERENDER_INIT` and then blits that snapshot with an early
  `return 1` on every later frame — before `Actor_info_draw_actor`,
  `Camera2_draw`, and `mMsg_Draw`. So the town moves behind a still photograph
  and remote players jump forward when the menu closes. This affects every
  submenu, not just the pocket: `mSM_submenu_ctrl` sets `PRERENDER_INIT` for all
  menu types, and `mSM_MODE_OTHER` is only ever compared against, never
  assigned, so there is no non-snapshot path.

  Skipping the capture and the blit online (letting the menu draw over the live
  world) **makes the menu itself invisible** and was reverted. The submenu
  assumes `POLY_OPA` contains nothing but the copy-mode background rect when it
  appends: `mSM_setup_view` emits only a scissor, viewport, and ortho projection
  matrix, and the panels then draw at z=140 through ROM mode display lists
  (`inv_mwin_mode`, `inv_mwin_item_frame_mode`). With the world drawn first,
  those inherit a depth buffer full of world geometry — `emu64.c:2362` derives
  `GL_DEPTH_TEST` from `othermode_low & Z_CMP` — plus whatever combine and
  render state the last actor left behind. A working fix needs a depth clear and
  a render-state reset emitted between the world draw and `mSM_submenu_draw`,
  and it has to be iterated against a running client; there is no depth-clear
  primitive exposed mid-frame today (`DisplayList_initialize` clears through
  `JW_setClearColor` at frame start, outside the display list).

  Related and still open: the local player actor is skipped entirely by
  `Actor_info_call_actor_except_player`, so its animation does not advance while
  a menu is up. Remote players do animate — they are `ACTOR_PART_CONTROL` with
  `ACTOR_STATE_NO_CULL`, and `Net_Remote_Player_move` drives
  `cKF_SkeletonInfo_R_play`. Running the local player's move proc instead would
  need its input neutralised for the duration of the call, since the submenu
  reads the same pads.
- The town map names the owner of every player house in an online town. Both
  `mMP_set_house_data` label sources were local: it read `Save_Get(private_data)`
  and kept a slot only if `mPr_CheckPrivate` passed, which is a `land_id` test on
  the client's *own* save. A connected client's save only ever holds the account
  it logged in as, so the other three houses always drew the `"free    "`
  placeholder even when `town.db` had residents in them. The server now owns the
  roster: `TownRuntime::current_roster` reads the persistent `accounts_` table
  (so a logged-out resident still owns their house), ships it in every
  `Baseline`, and republishes it as a town-wide `ResourceKind::Resident` delta
  from the tick whenever it changes. `Net_ResidentIdentity` exposes it as a
  tri-state — `-1` no authoritative roster, fall back to `Save_t`; `0`
  authoritatively vacant; `1` occupied — so the offline path is untouched.

- The island is an authoritative shared zone rather than an unvalidated hole.
  Before this change the island was reachable and completely unauthoritative:
  its acres sit at block row 8, outside the town's registered tile rectangle, so
  every dig, bury, plant, chop, drop, and pickup there missed `tiles_.find` and
  was rejected after the client had already predicted it; the cabin scenes had
  no zone at all, so entering one silently dropped the player out of
  replication; and the Kapp'n ferry never changes scene, so no transfer was even
  attempted and the player simply teleported 130 units north inside zone 1. Two
  players could stand on the island together and diverge.

  Zone 300 is the island exterior, 301 the cabin, 302 the islander's hut. Island
  tiles keep their **global** unit coordinates, which is what lets
  `mFI_UtNumtoFGSet_common` route an island write into `Save_t.island.fgblock`
  unchanged — `mFM_SetFgUtPtoSaveData` already pointed those acres' item arrays
  at island storage when the field was built. The island's coordinates cannot
  collide with the town's because its acres are outside the town rectangle.
  Island and town baselines keep separate revision watermarks on the client, so
  crossing between them cannot suppress the arriving baseline.

  The ferry needs no new hook. `Net_EnsureSceneZone` already reconciles a
  mismatch between the local scene's zone and the authoritative baseline zone,
  and `Net_SceneZone` is now position-aware for `SCENE_FG`: the acre kind
  decides town or island. Crossing therefore requests the ferry door on its own,
  and `request_transfer` validates the source zone and capacity rather than
  proximity — the same deliberate trust it already extends to building doors.
- The island cabin is a **shared house**. `Save_t.island.cottage` belongs to the
  town, not to a resident — all four original residents already shared one
  cabin, which `EXTENDED_RESIDENTS_PLAN.md` flags as an oddity and which is
  exactly the property multiplayer wants. `HousingAuthority` gained ownerless
  houses: no owner, `original_slot = 0xFF`, and presence in the zone as the
  whole authorization, so the ownership test is skipped and the zone test stops
  being optional. Furniture still moves through the same revisioned, journalled
  `HouseUpdate`/`FurnitureRequest` transactions as every other room, and a
  `HouseUpdate` already re-baselines everyone in the house's zone, so occupants
  see each other redecorate. The client's room sync was generalized from
  "`Save_t.homes[slot]`, three floors" to a binding over an `mHm_flr_c` array,
  so the cabin's single floor reuses the existing prediction/correction path;
  the cabin contributes no light switches and no upgrade level because it has
  neither.
- The island acre layout is reported by the client, not assumed by the server:
  `mFI_GetIslandBlockNumX` discovers it from the acre kinds, and `TownBootstrap`
  carries the two acre columns plus 512 island tiles. A client that cannot read
  the field layout yet sends no island section and the server waits. That is
  also the migration path for a town created before the island had a zone — the
  island is adopted on any login while it is still empty, and refused
  afterwards on the same rule that stops a second bootstrap replacing the world.
- The operator console has an Island row: readiness, tile count, how many
  players are on the shore / in the cabin / in the hut, cabin furniture count,
  and whether the islander is registered. "Awaiting acre layout" is a real state
  an operator needs to see, not a startup blip, so it is reported distinctly
  from the town's own readiness.

## Automated verification

- 36 unit/integration tests cover protocol, crypto, fragments, malformed
  packets, sessions, reconnects, prediction, concurrency, conservation,
  economy/trade, leases, zones/housing, persistence, GCI, time, runtime bots,
  production client loopback (including two-client shared-house enter/exit,
  leaving/arriving animations, source ghosts, exact arrival transforms, shared house-state
  convergence, inventory conservation, mutual visibility, client movement
  authority, and clock continuity), bounded
  bootstrap codecs, canonical first-writer behavior/restart persistence,
  configuration validation, server-authoritative mail/banking (revisions,
  idempotent replay, mailbox capacity, wrong-claimant and stale-revision
  refusals, conservation, and the refusal of administrative operation types on
  the request path), and operator gifts surviving a restart with live mailbox
  replication to a connected client.
- Bounded protocol fuzzing, eight-client encrypted load, loss/jitter/
  duplication/reordering/reconnect chaos, headless smoke, ASan/UBSan, and an
  accelerated 31-day crash/restart soak are available from the root Makefile.
- CI builds and verifies the Linux 64-bit client/server and Windows PE32+
  x86-64 client/server, runs the Windows server, and uploads a clean package.
- The Windows game boots from a user-supplied legitimate CISO in both offline
  and online smoke runs. The title scene has been visually verified with the
  renderer fixes retained from the upstream x64/rendering work.

### Patterned-clothing equip crash (runtime path)

- Completes the earlier "Patterned-clothing crash fix" below, whose stated
  limitation was that it only covered *booting* while already wearing a pattern
  and never the inventory equip gesture. Equipping a design through
  Pockets -> design page -> Wear crashed the client on the spot.
- Cause: `mPlib_change_player_cloth` (`src/game/m_player_lib.c`) took its
  destination from `mPlib_get_player_tex_p`/`mPlib_get_player_pallet_p`, which
  return NULL when the player texture object bank is unregistered or its DMA is
  still pending. Stock clothing survived that because its ARAM branch reaches
  `JKRAram::aramToMainRam`, which allocates a buffer when handed a null
  destination (`src/static/JSystem/JKernel/JKRAram.cpp`); a custom design takes
  the `bcopy` branch instead, which faults. That asymmetry is exactly why stock
  shirts worked and patterns did not. Both copies are now skipped on a null
  destination, matching the guards `mPlib_change_player_face` and
  `mPlib_change_player_face_pallet` already had.
- `pc_crash_protection_init` was defined in `pc/src/pc_main.c` but never called
  anywhere in the tree, so the fault handler was never installed and a fatal
  fault closed the window silently. It is now installed at startup, once
  `pc_image_base` is known, and any fatal fault writes `crash.log` beside the
  executable (and to stderr) with the faulting address, the data address, and a
  stack scan of return addresses, all as image offsets. Resolve an offset with
  `nm --numeric-sort AnimalCrossing.exe`; the report prints the matching "nm
  address" directly. Reporting never changes control flow, so the existing
  `jmp_buf` recovery path is unaffected.
- Verification: fixed and confirmed by hand through `run_two_clients.bat` with
  two visible online clients against a real dedicated server. Note that this
  launcher runs staged copies under `pc/build64/manual-two-client-test/`, not
  `pc/build64/bin/` — restage after every build or the test exercises a stale
  binary.
- Follow-up worth doing: the guard removes the crash, but a null destination
  means the texture/palette copy is skipped entirely, so a pattern equipped at
  that moment may render stale until something else refreshes the bank. Why the
  bank is unavailable during the submenu gesture is not yet explained, and the
  automated coverage gap called out below — no input replay for the equip
  gesture — still stands.

### Island as a shared zone

- `make test`: 36/36, including two new cases. "island authoritative shared
  zone" drives a real server through the whole path: no island before a client
  reports one, adoption by a *later* login on an already-bootstrapped town, the
  island tile landing at its global unit coordinate with no town tile at the
  same coordinate, survival across a restart, and a returning client's island
  report being refused rather than overwriting a played-in island. "island cabin
  shared room" covers the ownerless house — anyone present may place, a second
  occupant may take the same item straight back out, someone outside the zone is
  refused `OutOfRange`, a resident-house floor index is refused, and the
  baseline round-trips `shared` from the ownerless `(owner, original_slot)` pair.
- `make check`: test, fuzz (50k), 8-client load, chaos, 31-day month soak with
  five restarts and a recovered torn write, and the headless smoke all pass.
- `make sanitize`: the same 36/36 under ASan + UBSan.
- `src/game/m_net_hooks.c` passes a `NETCODE_ENABLED`/`TARGET_PC` syntax build
  with the repository's PC defines.
- Not verified: the graphical client on a real disc. No SDL2 is available in
  this environment, so the ferry crossing, the cabin interior, and two players
  redecorating the cabin have not been exercised in the running game — only in
  the portable core. That is the next thing to run.
- Known limitations. The islander is registered as a shared NPC with a
  conversation lease but has no dialogue or schedule behaviour yet; the island's
  32 per-player deed flags (`mISL_PLAYER_ACTION_*`) are still written through
  the sole-player `Common_Get(now_private)` path and the file-static
  `l_misl_count_table` in `src/game/m_island.c`, which is shared across players
  and is already wrong with two of them on one island. `ANIMAL_MEMORY_NUM` is 7,
  so the islander cannot remember sixteen residents. Island weather is still the
  client-side global swapped by the boat demo rather than per-zone server state.
  `aBT_check_other_boat` still deletes a second boat actor, so the ferry is
  effectively one boat per field.

### Patterned-clothing crash fix

- Custom designs store their texture and palette in live save-memory buffers,
  while stock clothing uses 32-bit ARAM offsets. The player resource helpers
  returned both through `u32`, truncating custom-design pointers in the
  supported 64-bit client. Equipping a pattern could consequently pass an
  invalid source address to `bcopy`. Texture/palette sources now remain
  `uintptr_t` through initial loading, runtime clothing changes, and the shared
  appearance loader; only the explicit ARAM path narrows an offset to `u32`.
- `scripts/smoke_pattern_clothing_windows.ps1` makes a temporary,
  checksum-valid copy of a user-supplied GCI, equips resident design slot 0,
  and boots that save plus an observer save against a real dedicated server.
  The pre-fix PE32+ client reproduced an `0xc0000005` access violation in
  `msvcrt.dll` before gameplay. Both rebuilt clients now reach gameplay and
  remain alive with the identical fixture, and the observer confirms that it
  loaded the patterned texture for the other account. No disc image or save is
  copied into the repository.
- Runtime clothing changes now use a reliable `AppearanceUpdate` transaction.
  The server validates the bounded custom-design index, palette, and 512-byte
  texture; assigns the authoritative appearance revision; journals the change;
  and sends a full appearance baseline to viewers. High-rate transform
  snapshots carry only that revision, so even a 16-player snapshot stays below
  the plaintext MTU rather than repeating pattern bitmaps. GCI import/export
  and town persistence retain the selected pattern and its texture data.
- Verification: the 64-bit Windows client and server build successfully, the
  two-client pattern-clothing smoke passes, `make test` passes 36/36, and the
  same 36/36 tests pass under ASan/UBSan from an isolated native-filesystem
  worktree. The production loopback test also changes a connected client's
  pattern and asserts byte-for-byte convergence at the second client.
- Known limitation: the Windows smoke loads a character already wearing the
  pattern, while the production loopback drives the underlying runtime update
  programmatically; it does not automate the inventory UI gesture itself.
- Next recommended follow-up: add input replay for that UI gesture without
  weakening the current asset-free, temporary-fixture boundary.

### Furniture push/pull follow-up

- `make test`: 34/34 tests passed, including snapshot action round trips and a
  two-client production loopback assertion that each peer receives the other's
  action state.
- `make sanitize`: the same 34/34 tests passed under ASan and UBSan.
- The three changed game-side C translation units pass a NETCODE/TARGET_PC
  syntax build with the repository's PC defines, and the changed C++ client
  compiles with `-Wall -Wextra -Wpedantic -Werror`.
- Known limitation: the presentation glide recognizes one adjacent layer-zero
  push/pull. Rotations, bulk layout edits, and groups with independently moving
  furniture on top still reconcile by snapping to the authoritative baseline.
- Follow-up fix: owner predictions now remain in place until the successful
  `HouseUpdateResult` revision is observed, and rejected updates hold the old
  baseline through the action startup/settle window so a stale room rebuild
  cannot invalidate the original player PUSH/PULL state.
- Next recommended task: with a legitimate disc and a running client, retry the
  live-submenu change from the open issue above — draw the world, then emit a
  depth clear and a render-state reset before `mSM_submenu_draw` — and verify
  the pocket panels are still visible before touching the local player's
  animation. Separately, confirm the map's four house labels match the server's
  `--list-accounts` roster.

### Equipment, animation, and tool authority

- Holding an item is now a server transaction (`EconomyOpType::HoldItem`): a
  swap between one pocket slot and `InventoryState::equipped`. Equipping,
  putting away, and swapping tools are the same operation, so it can neither
  create nor destroy an item. Previously equipping was purely local — it cleared
  a pocket slot that the next authoritative projection filled back in while the
  tool was still held, duplicating the tool on the player's next transaction.
- `Private_c::equipment` is a projection of the authoritative inventory.
  `Net_ReconcileEquipment` detects the net effect of any local write to it — the
  L/R tool cycle, the pocket submenu's drag onto the player, closing the menu
  with a tool in the cursor — and reports it as that one swap. The submenu state
  machine is untouched, and no equip path can be missed.
- Tool checks read the hand, not the bag. `WorldAuthority` and
  `EncounterAuthority` validate `InventoryState::equipped`; `tool_slot` is gone
  from `WorldOperation` and `EncounterRequest`. A shovel in a pocket no longer
  digs, and a rod in a pocket no longer catches.
- `WorldAuthority::total_item_units()` counts the hand, so the conservation
  invariant stays honest once anyone equips something.
- Player animation is replicated. `PlayerAnimation` — two `mPlayer_ANIM_*`
  indices, an `mPlayer_PART_TABLE_*`, an `mPlayer_ITEM_MAIN_*`, and the two
  playback bits — rides `InputCommand`, is bounds-checked in
  `MovementSimulator::submit`, and is published as a zone-scoped reliable
  `ResourceKind::Player` delta on change. `ac_net_remote_player.c` drives
  keyframe0 and keyframe1 through the replicated part table with
  `cKF_SkeletonInfo_R_combine_play`, replacing the four-state
  WAIT/WALK/PUSH/PULL table: remote players now show runs, tool swings,
  fishing, digging, sitting, and the rest of the original animation set.
- Remote players draw what they are holding. A hand-joint callback captures the
  matrix during the skeleton walk and the equipped item's model is drawn there.
- `InputCommand.action` is validated against `mPlayer_INDEX_NUM`. It was
  previously accepted and forwarded unchecked.
- `AppearanceUpdate` no longer carries equipment, has its own rate bucket (1/s,
  burst 4), and only re-baselines every connection when the visible appearance
  actually changed. Tool cycling could previously drive up to twenty full
  baselines per second per client, each one journalled.

Verification:

- `make check`: 39/39 tests, protocol fuzz, eight-client load, chaos, the
  accelerated 31-day soak, and headless smoke. `make sanitize`: the same 39/39
  under ASan and UBSan.
- New coverage: `held items are a conserving swap` (swap, replay, straight
  tool-to-tool swap, put-away, empty-hand-and-empty-slot refusal, stale
  `expected_item` refusal, out-of-bounds slot, and unit conservation across all
  of it) and `player presentation replication` (input-command and delta round
  trips, truncation, trailing bytes, every out-of-range index and flag bit, and
  the movement authority's refusal of an out-of-range animation or action). The
  production-client loopback now holds a rod before catching, stows it before
  dropping, and asserts both the animation and the held item converge on the
  other client.
- The Windows client and dedicated server build and link (`build_pc.bat`,
  4057/4057), which covers the three changed decompiled translation units.
- The state v7 → v8 migration was exercised against a copy of a real
  two-resident town directory: it loads with both residents intact.

Known limitations:

- Villagers remain client-local. `AUTHORITY_MATRIX.md` previously claimed NPC
  state was server-owned and replicated; that row has been corrected.
  Server-authoritative NPC transforms and animation are a separate roadmap
  phase.
- The held item is drawn as a plain model in the hand rather than through
  `Player_actor_Item_draw_*`. A remote fishing rod has no line and a remote net
  has no catch label; the swing itself is correct because it comes from the
  animation.
- `item_state` (`mPlayer_ITEM_MAIN_*`) is replicated but not yet consumed by the
  remote actor. It is what a full tool-actor presentation would need.
- Not verified with a legitimate disc: the client has been built and linked but
  the new animations and held-item rendering have never been drawn on screen.

*Superseded by "Remote faces, tools and locomotion speed" below: the last three
bullets are now addressed.*

### Item drops are shown, not popped in (protocol v15)

Dropping and picking up an item were correctly replicated but had no
presentation. `bIT_actor_player_drop_entry` returned before
`bIT_actor_drop_entry`, so no `bg_item_drop_c` was ever created online: the arc,
the bounce, and the landing SFX (`sAdo_OngenTrgStart(0x2A, ...)`, which lives in
the drop actor's move proc) never played for anybody, and the item teleported
onto the ground when the authoritative tile projection next ran. This change
makes an online drop look like an offline one on every client.

- `TileStateDelta` carries `actor` (u64) and `cause` (`TileChangeCause`, u8).
  Without them a viewer cannot tell a thrown item from a sapling that grew
  overnight, and cannot find the hand the arc should start from — two players
  standing together are otherwise indistinguishable. Server-originated changes
  (growth, GCI import, operator commands) publish `actor = 0` / `Server` and are
  never animated.
- The dropping client animates at request time. `Net_BeginPredictedDrop` claims
  the tile and the original body runs unchanged, reserving the cell with
  `RSV_NO` exactly as offline; the drop actor writes the field itself on
  landing. Observers animate from the drained tile change, arcing from the named
  player's hand via `bIT_net_remote_drop_entry` → `bIT_drop_entry_v1`. A dropper
  outside the viewer's interest set has no hand to throw from, so that change is
  projected directly — which is what every drop did before.
- A claim is resolved by what the field actually shows, not by a timer: once the
  cell holds the predicted item the animation has finished, and the mirror then
  says whether the server agreed. Disagreement force-writes the authoritative
  value and rolls the pocket back. The 90-frame budget is only the backstop for
  an animation that never lands (the longest player drop is ~65 frames).
- A refusal is invisible to the mirror — the tile revision does not move and the
  item never appears — so `Net_ExpireRefusedClaims` consumes the `WorldResult`
  that nothing previously read and marks the claim. It deliberately does not
  correct the tile there: an animation still in the air would land after the
  correction and paint the refused item straight back. The reconciler acts the
  moment it touches down. Missing a result (the client keeps only the newest)
  costs no more than falling back to the frame budget.
- A refused request now returns 0 rather than -1. All four callers of
  `mTG_common_throw_put_field` test the result as a boolean and -1 is truthy, so
  a refusal read as a successful drop: the pocket was cleared, no warning window
  opened, and nothing had been sent. Only the netcode branch changed; the
  offline path keeps the original -1.
- `Net_ApplyAuthoritativeState` keys its bulk projection on a new baseline
  *serial* instead of `acnet_client_baseline_revision()`. That revision moves for
  every delta of every kind, so one nearby player changing animation rewrote the
  entire 256-tile interest chunk and rebuilt the field's draw and collision
  tables with it. Individual changes now arrive through a bounded (256) client
  queue drained per frame; an overflow is reported and triggers one full
  reprojection rather than silently losing a change. Because the chunk buffer is
  now filled only from real baselines, it can no longer be overrun by tile
  deltas accumulating between them.
- The town and island revision watermarks collapse into one serial: it is
  monotonic across zones, so crossing between them always arrives on a value the
  client has not seen.
- Pickup is symmetric — the cell is cleared when the request goes out instead of
  a round trip later, under the same claim.
- `Net_RequestDrop`/`Net_RequestPickup` no longer zero `last_inventory_revision`.
  That forced reprojection was the rollback mechanism, but it also undid the
  caller's optimistic pocket change on the very next frame, which is the whole
  of the prediction. `Net_ReconcileTileClaims` rolls back instead, and only on an
  actual refusal.
- `ClientRuntime::dispatch` is public so a test can drive message handling
  without a server and a handshake. It authenticates nothing; the transport
  still does.

Verification:

- `make check`: 41/41 tests, protocol fuzz, eight-client load, chaos, the
  accelerated 31-day soak, and headless smoke. `make sanitize`: the same 41/41
  under ASan and UBSan.
- New coverage: `tile delta actor and cause` (round trip, the `Server`/0 default,
  every `WorldOpType` mapping, an out-of-range cause refused at both ends,
  truncation at each byte of the two new fields, trailing bytes) and `tile change
  queue and baseline serial` (delta does not bump the serial, FIFO drain removes
  exactly what it copies, the 256-entry bound raises the overflow flag, a
  baseline supersedes the queue and clears the flag).
- The Windows client and dedicated server build and link, covering the four
  changed decompiled translation units.

Known limitations:

- Not verified with a legitimate disc. The arc, the landing sound, and the
  remote timing have never been seen on screen — only the state machinery
  underneath them is covered by the headless suite.
- A drop refused by the server after its arc has already started stays visible
  until the item lands, then snaps to the authoritative value. Killing an
  in-flight drop actor would need a handle into `bg_item`'s drop table that the
  clip does not expose, so the correction waits for the landing rather than
  fighting it.
- The claim table holds 8 tiles. A marked multi-item put larger than that
  degrades to the previous pop-in behaviour for the remainder, which the
  server's 6-tick operation cooldown already paces well below.

## Remote faces, tools and locomotion speed (2026-08-06)

Entirely client-side. No wire format change, no server change, no protocol
version bump — the three values this needed were already replicated and simply
unread by `ac_net_remote_player.c`:

| Value | Already arrived via |
|-|-|
| `now_main_index` | `Net_CapturePlayerTransform` writes it to `transform.action`; carried by the 15 Hz snapshot and the baseline, preserved through `blend()`, exposed as `states[i].transform.action` |
| `now_item_main_index` | the reliable `Player` delta, as `animation_item_state` |
| movement velocity | the snapshot transform, in the game's own units |

- **Faces animate.** `mPlib_Face_Reset`/`mPlib_Face_Step` in `m_player_lib.c`
  reproduce the original's three drivers as a pure stepper over per-remote
  state: the random blink, the per-animation eye/mouth texture tracks
  (`mPlib_Get_Player{Eye,Mouth}TexAnimation_p`, 33 and 35 of 157 animations),
  and the per-state constants. The draw offsets the two face segments by the
  resolved tile instead of pinning both to tile 0, which is why remote players
  previously never blinked or emoted. The 0xE00 face resource already holds all
  8 eye and 6 mouth tiles, so nothing extra is loaded.
  `Player_actor_set_eye_pattern_Talk`/`_Shock` turn out to dispatch on the
  animation, not the state, so they collapse into one animation-keyed case and
  the state table is six entries.
- **Tools render properly.** The held item is drawn under the hand joint's full
  matrix (`Matrix_get(&render->right_hand_mtx)`) at `item_scale`, dispatching on
  whether the item shape is a bare display list or its own skeleton — the same
  split `Player_actor_Item_draw` uses. It replaces a world/shop pickup model
  drawn at a bare position with no orientation. Item models, skeletons and
  animations come from `mPlib_Get_Item_DataPointer`, a table of global symbols,
  so a viewer needs no per-actor DMA.
- **Tools hide when stowed.** Visibility is gated on
  `animation_item_state != mPlayer_ITEM_MAIN_NONE` instead of "the inventory
  hand is non-empty", so a tool no longer renders through menus, interiors that
  forbid tools, or pickup animations.
- **Item animation.** The 13 `Player_actor_SetupItem_Base2` sites pass
  `mPlayer_ANIM_*`/`mPlayer_INDEX_*` constants where `mPlayer_ITEM_DATA_*`/
  `mPlayer_ITEM_MAIN_*` are meant — flagged by the decomp itself at
  `m_player_common.c_inc:3977`. The ordinals were resolved numerically and all
  13 land on the semantically matching pair (swing_net → `ITEM_MAIN_NET_SWING`/
  `ITEM_DATA_NET_SWING`, and so on), which is what confirms the reading. The
  resolved table is in a comment beside `Net_Remote_Player_item_anim`.
  `item_state` and `equipped_item` come from different authorities and can
  disagree, so the pairing is re-validated against the item kind before it can
  reach a skeleton.
- **Umbrella and balloon.** An umbrella is a real `TOOLS_ACTOR` child, given the
  pole matrix exactly as `Player_actor_Item_draw_umbrella` does, and destructed
  when the tool changes or the remote dies. The held balloon turned out **not**
  to be a sub-actor at all — `Player_actor_Item_draw_balloon` draws it from
  `item_keyframe` under its own matrix, and `PLAYER_ACTOR::balloon_actor` is
  only for a *released* balloon — so its movement, sway and lean are transcribed
  against per-remote state and its own hand-position delta.
- **Legs no longer sprint.** `Net_Remote_Player_apply_animation` hardcoded
  `frame_speed` 1.0 while `Player_actor_CulcAnimation_Walk` assigns
  `0.6 * sqrt(actor->speed / 7.5)` (min 0.22) every frame, so every remote walk
  cycle ran between 1.7x and 4.5x too fast. The speed is now derived from the
  replicated velocity: `Actor_position_speed_set` makes `|velocity_xz|` exactly
  `actor->speed`, and `MovementSimulator::tick` stores the client transform
  verbatim, so the replicated velocity is already in the game's units.
  `mPlayer_INDEX_READY_WALK_NET` has its own constants; everything else stays at
  1.0. Replicating the value instead was rejected: it changes every frame while
  walking and so cannot ride the change-triggered reliable delta.

Verification:

- `make test`: 39/39. The commit touches no portable core file, and that is what
  this proves.
- Both changed decompiled translation units and `m_player.c` compile clean under
  `-Wall -Wextra` with the PC build's defines.
- **Not drawn on screen.** No disc is available in the environment this was
  written in, so none of it has been visually verified. That is the real gate.

Known limitations:

- No fishing float or line for a remote. `Player_actor_SetActorUki` claims the
  float with `Actor_info_name_search(&play->actor_info, mAc_PROFILE_UKI,
  ACTOR_PART_BG)` — it is a **scene singleton** placed by the scene spawn
  tables, and the local player writes hand positions straight into it. Two
  players fishing would fight over one actor. Per-player float ownership is a
  gameplay-actor change, deliberately not bundled here.
- No net catch label: what a remote caught is a server-owned encounter outcome,
  not presentation.
- The net bag does not lean. `Player_actor_Item_draw_net_After_dummy_net`
  computes `net_angle` from local collision and keyframe speed; the bag sits at
  its rest angle.
- A remote balloon's lean target is flat: the lean chases
  `-shape_info.rotation.x`, which is terrain pitch, and only yaw is replicated.
- The original's per-tool collision captures (`axe_pos`, `net_pos`, the net
  collision points, the rod tip) are deliberately not reproduced — they feed
  collision and the fishing state machine, and a presentation actor has neither.

## Appearance across a zone transition (2026-08-06)

A player who used a door came back looking like somebody else to everyone who
stayed behind — wrong gender, wrong face, wrong shirt — and stayed that way for
the rest of the session. Reported from two-client testing. **Fixed, and covered
by a regression test.**

An earlier reading of this blamed the gender object bank. That was wrong:
`cKF_bs_r_boy_1` and `cKF_bs_r_grl_1` are ordinary linked data in
`src/data/model/`, and their joint tables point at linked display lists, so no
bank swap is involved and nothing about the body is scene-dependent.

The real cause is on the wire. Appearance is deliberately baseline-owned so the
15 Hz transform snapshot stays under the unfragmented MTU, and as
`town_runtime.cpp:965` states, *"baselines only reach a client on join and zone
transfer"*. Three things then compose into the bug:

1. A client drops a remote's entire `RemoteTrack` — appearance, custom pattern
   *and* presentation — once that account stops appearing in snapshots for half
   a second. Walking into a house does exactly that to everyone left outside.
2. When the traveller comes back, `remotes_[account]` is default-constructed and
   repopulated from the snapshot, which carries transform but no appearance. The
   viewer draws gender 0, face 0, clothing 0.
3. Nothing repairs it. The returning player's appearance has not *changed*, so
   the `appearance_changed` guard means no `AppearanceUpdate` is broadcast; and
   the destination-zone baseline goes only to the traveller, not to the people
   already standing there.

`handle_hello` already re-baselines every viewer when a player joins, with a
comment giving precisely this reason. The zone-transfer path simply never got
the same treatment.

- **Server.** `ZoneReady` now re-baselines every connection standing in the
  destination zone, not just the arriving one. Scoped to the destination:
  occupants of the source zone need nothing, because the departing player simply
  stops appearing in their snapshots, which their client already handles.
- **Client.** Absence is now handled in two stages. At half a second the
  transform history is cleared, which is what actually makes a vanished player
  disappear rather than stand frozen — `TransformHistory::sample` only fails on
  an empty history, so it would otherwise extrapolate forever. The track itself,
  carrying the baseline-owned appearance and presentation, is retained for
  thirty seconds. That makes a brief absence — packet loss as much as a door —
  survivable without a round trip. The unsigned tick subtraction is now guarded
  so an out-of-order snapshot cannot underflow and evict a live player.

Verification:

- New test `appearance survives a zone round trip`: two clients meet, one sets a
  deliberately non-default appearance, walks into a house, comes back, and the
  stayer must still see the real appearance *and* have it in their own baseline.
  The baseline assertion is what covers the server half specifically; client
  retention alone would leave any absence longer than the window still broken.
- With the fix reverted the new test fails with exactly the reported symptom
  (`gender=0 face=0 clothing_index=0`). With the fix, 40/40.

Also fixed here, pre-existing and unrelated to the bug above: `production client
loopback` was flaky at `CHECK(actions_converged)`. Measured on pristine
pre-change netcode it failed roughly 1 run in 5 standalone, and both `make check`
runs attempted before the fix — `make check` loads the machine harder than a bare
`make test`. Alone among the convergence loops in that test it never yielded,
giving real UDP loopback 120 iterations of simulated time but almost no wall time
to deliver in. It now sleeps a millisecond per iteration and gets the same budget
as its neighbours: 8 standalone runs and `make check` all clean. This was a
defective test rather than a defect in the code under test, but a gate that goes
red at random is a gate nobody trusts.

## Hosting a town without an invitation key (2026-08-06)

The server used to refuse to start when `invite_key` was blank, which is what a
packaged `server.ini` ships with. Double-clicked from Explorer that read as a
broken executable: the process printed `an invitation key is required...` to a
console Windows destroyed on exit, so the window only flashed.

- A blank key is now a supported mode. `TownRuntime::initialize` no longer
  rejects it, the `allow_unauthenticated` config field is gone, and
  `--insecure-local` is accepted but does nothing.
- Such a start prints a warning naming the port: no invite proof is demanded and
  no session keys are derived, so anyone who can reach it may join as any
  account over unencrypted traffic. Set `invite_key` to close the town.
- A fatal exit now holds the console open when the process owns the window, so
  the reason stays readable. It never waits when the console is shared with a
  shell or when either standard handle is redirected, which keeps the smoke
  scripts and CI from blocking. `GetConsoleProcessList` is only trusted when it
  reports more than one process; it returns 0 outright under some console hosts.
- `write_default_town_config` now writes `config.invite_key` instead of an empty
  string, so migrating a legacy `config.toml` no longer silently drops the
  operator's key and leaves a server that will not start.

## Items on top of dragged furniture (2026-08-06)

Dragging or spinning furniture that had anything sitting on it deleted whatever
was on top, permanently and server-side. Offline was never affected.

The original game lifts every layer-1 occupant above the furniture out of the
room grid for the duration of the animation and parks it in
`my_room->parent_ftr.fit_ftr_table[]` (`aMR_RequestItemToFitFurniture`,
`ac_my_room_move.c_inc:98`), putting it back only when the keyframe settles
(`aMR_RequestItemToUnFitFurniture`, `:194`). That grid is `Save_t` itself —
`m_field_make.c:850` points `fg2_p` straight at the home's layer arrays — and
`Net_CaptureHouse` reads it. Because the capture deliberately skips its settle
window while a move is playing, the client submitted a room the items had been
deleted from; the server committed it, and the broadcast overwrote the local
restore on the frame the animation ended.

- `Net_CaptureHouse` now captures layer 1 of the active floor from a patched
  copy that reinserts the in-flight items at the units they will land on, so the
  submitted candidate is complete and the hash does not change across the
  settle. `aMR_NetFittedItems` (`ac_my_room.c`) reports them, evaluating the
  parent's *final* position and angle rather than its in-flight ones, and
  predicting the orientation the unfit will give an item of furniture on top. A
  cell the grid already claims is never overwritten.
- `aMR_NetFurnitureMoveActive` now covers the rotate states (`WAIT_LROTATE`
  through `RROTATE`) and any fitted parent. It previously stopped at
  `aFTR_STATE_PULL`, and `mPlib_check_player_actor_main_index_Furniture_Move`
  only matches PUSH/PULL, so a spin suppressed neither the reconcile nor the
  furniture rebuild.
- `aMR_NetReloadFurniture` drops the fit table before destructing the actor
  list. A rebuild ends the animation without ever reaching the unfit, which used
  to leave `parent_ftr.ftrID` pointing into a list that no longer described it.
  The authoritative grid the caller just applied is what the items come back
  from.

No decompiled game logic changed — the fit/unfit code is untouched upstream, so
an `NETCODE_ENABLED=OFF` or offline build behaves exactly as before.

Verified: Windows build (966 targets, client and dedicated server link),
`make test` 39/39. Not yet exercised on screen with a legitimate disc — the
two-client staging under `pc/build64/manual-two-client-test/` has been restaged
with these binaries for that.

## Server-side item pricing and sales (2026-08-06)

An audit of NPC and villager networking found that the NPC-mediated half of the
economy was *implemented on the server and unreachable from the client*.
`m_net_hooks.c` defines `NET_ECONOMY_BUY`, `SELL`, `DONATE`, and `ATTACH_MAIL`;
`net/src/economy.cpp` implements all four against real shop and museum state;
no call site in `src/` issues any of them. Only deposit, withdrawal, debt
payment, and hold-item are actually wired.

That is worse than an absent feature. The wallet is authoritative —
`Net_ApplyAuthoritativeInventory` overwrites the wallet and the pockets from the
server whenever the inventory revision moves — so a purchase that only mutates
locally is *undone* a moment later: the bells return and the item does not.
Selling, donating, and every NPC that hands over an item behave the same way.

Worse still, the server could not have committed a sale even if asked.
`EconomyAuthority::Sell` priced items from a `sell_prices_` map that only the
test suite ever populated, so every sale would have been refused with
`InvalidState`.

This change fixes the server half completely and leaves the client call sites
for the follow-up work listed below.

**Prices now come from the game's own function.** `tools/gen_shop_tables.py`
grew a second dumper that links the real `mSP_ItemNo2ItemPrice` and calls it
once per 16-bit item id, emitting the 5,807 priced items as a sorted sparse
table. The previous generator emitted only the six shop-category price tables
and `shop_item_price()` re-implemented the indexing by hand, which covered
furniture, stationery, clothes, carpet, wallpaper, and diaries and nothing else
— no fish, insects, tools, fossils, shells, or plants, which is most of what a
player actually sells.

Calling the real function rather than porting it matters because pricing runs
every item through `mRmTp_FtrItemNo2Item1ItemNo`, which remaps the furniture
forms of clothes, fish, insects, umbrellas, balloons, diaries, fans, pinwheels,
and tools back to the item they are priced as, using several index tables and
`mNT_FishIdx2FishItemNo`. Hand-porting that would have been a large, silently
driftable body of code. The dumper links the real thing and ignores unresolved
symbols: the pricing call path reaches only the price tables, the fish index
helper, and `common_data`, and never enters the rest of the game.

Two prices are town state rather than table data and are handled explicitly:
the new year's grab bag costs the current year, and fruit costs
`mSP_FOREIGN_FRUIT_PRICE` (2000) everywhere except the town that grows it, where
it costs 400. The sweep runs with no native fruit set, so the table holds the
foreign price and the five native prices are dumped alongside it.

- The generated item pool and list spans are **byte-identical** to before, so
  daily stock rolling is unaffected; only the price representation changed.
- `python3 tools/gen_shop_tables.py --check` still enforces freshness. It is not
  yet wired into `make check` or CI — worth doing.

**The town's fruit reaches the server.** `TownBootstrap` carries a
`native_fruit` field (protocol v16 after the merge below), captured from `Save_Get(fruit)` in
`Net_SubmitInitialTown` and persisted in town state v9. A client that cannot
report one sends zero and the server keeps what it has; a server never told
prices all fruit as foreign, which is the same state a new town starts in.

**Selling is committable.** `EconomyAuthority` gained
`set_sell_price_resolver()`, consulted whenever an item has no explicit
override. `TownRuntime` installs a resolver over `shop_sell_price(item,
native_fruit_, town_year())`. The authority itself stays free of the game's
tables, so a test can still price a handful of items by hand. An item nothing
prices is now refused rather than sold for nothing, matching the original's
`mSP_ItemNo2ItemPrice(item) / SELL_BUY_RATIO == 0` refusal.

Verified with `make check` (41/41, exit 0 — chaos and the accelerated 31-day
soak with 5 restarts and torn-write recovery both pass) and `make sanitize`. New
coverage:

- `shop prices match the original` pins the net, axe, shovel, rod, a shellfish
  (the separate 8-entry table), the signboard constant, native and foreign
  fruit, the year-priced grab bag, and unpriced ids; then rolls a full
  Nookington's shelf and asserts every item on it has a price.
- `selling pays the generated price` walks a sale with no resolver installed
  (refused, the old behaviour), then with one: a rod at 125, a foreign apple at
  500, the town's own cherry at 100, an unpriced item refused rather than given
  away, and an explicit override still winning.
- `native_fruit` added to the bootstrap codec round trip.

Not done, and the reason each is not a one-line follow-up:

1. **Selling from the shop UI.** `ac_npc_shop_common.c:2247` is the commit
   point, but the flow sells several items at once, handles paper stacks
   separately, and breaks the wallet into 30,000-bell bags at
   `mPr_WALLET_MAX`. The server transaction is one slot at a time, so the call
   site needs to issue one request per slot and let the projection produce the
   final wallet — and the bag-breaking has to move server-side or be derived
   from the projected wallet. Getting this half-right is worse than leaving it.
2. **Buying.** Needs the client's shelf to *be* the server's shelf: `Buy` names
   a `shop_index` into `ShopState::stock`, while the game rolls its own list
   into `Save_Get(shop).items`. `ShopState` already rides the zone baseline and
   `net/src/shop.cpp` already reproduces `mSP_MakeRandomGoodsList`, so what is
   missing is an `acnet_client_shop_*` accessor and a projection into
   `Save_Get(shop).items`, the way the inventory is already projected.
3. **Donating.** `aCR_putaway_demo_end_wait_init` in
   `ac_npc_curator_move.c_inc` is a clean single-item commit point, but `Donate`
   quotes the museum revision and the museum is not replicated at all — no
   baseline field, no `ResourceKind::Museum`, no client accessor. Passing zero
   to skip the revision check would defeat the point of the check.

## The shop shelf is replicated, and stopped leaking (2026-08-07)

Continuation of the pricing work above, taking the next step in its own
dependency order: before a purchase can name a row of Nook's shelf, both sides
have to agree on what the shelf is.

**A privacy bug went out with the old delta.** Every accepted economy operation
appended a `ResourceKind::Shop` delta whose payload was the encoded
`EconomyResult` — and for `Buy` and `Sell` it was sent with `target_account = 0`
and `zone = 0`, which `DeltaLog::relevant` treats as *everybody*. That result
carries the acting player's wallet, bank balance, debt, and inventory revision.
No client read it and neither `Buy` nor `Sell` was reachable, so nothing ever
leaked in practice — but wiring the purchase would have made it live. The
account-targeted echoes for the other operations were redundant too: the
requester already receives the same payload directly as a reliable
`InventoryResult`.

That delta is gone. `ResourceKind::Shop` now means the shelf and nothing else.

- `encode_shop_delta` / `decode_shop_delta` carry the whole `ShopState`
  (revision plus up to `kMaximumShopEntries` rows). The list is republished
  whole rather than per-row: it is small, and a shelf assembled from partial
  updates could disagree with the server about which index holds what, which is
  exactly what a purchase names.
- The server publishes it town-wide on a purchase and on the daily roll, so a
  sold-out row and a fresh morning shelf both reach players who are nowhere
  near the shop.
- `Client` replaces `baseline_.shop` wholesale on receipt;
  `acnet_client_shop_stock()` and `acnet_client_shop_revision()` expose it.

**The server's shelf is now the whole shelf.** `roll_shop_stock` previously
reproduced only `mSP_MakeRandomGoodsList` — the rarity-list draws — while the
game appends tools, paint, a signboard, an umbrella, saplings, and flower bags
in `mSP_MakeGoodsList` afterwards. Leaving those out would have made the
server's index disagree with the shelf a player is looking at the moment they
tried to buy a shovel. `mSP_SelectTool` and `mSP_SelectPlant` are now ported,
including Nook's Cranny gating the net, rod, and axe behind
`mSP_{NET,ROD,AXE}_SALES_SUM`, the paint colour rotating one step per roll and
wrapping after twelve, and the cedar sapling appearing only at Nookway and
above.

Two deliberate departures from the original, both commented at the port:

- The original's tool and flower loops retry on a duplicate with no bound. That
  is fine on a console and unacceptable in a server tick, so both are capped;
  the cap only fires on a pathologically unlucky draw and costs the shelf one
  item for the day rather than hanging.
- The Halloween and grab-bag-sale shelf variations are not modelled. The server
  rolls the ordinary shelf on those days. This is a behaviour gap, not a desync,
  because the client is told the shelf rather than rolling its own.

`ShopStockState` gained `sales_sum` and `paint_color`, and the **whole**
structure is now persisted. It never was before: the rarity permutation was
re-derived from the town seed at every start, which happened to work only
because nothing mutated it. Lifetime sales and the paint rotation both
accumulate, so a restart would have reset the tool unlocks and put the paint
back to red. Folded into the same state v9 as the native fruit, which has not
shipped.

Per-tier goods counts are now generated from `l_goods_count_table` rather than
hand-copied into `shop.cpp`, along with the item ids and thresholds the two new
ports need.

Verified with `make check` and `make sanitize` (43/43). New coverage:

- `shop shelf is the whole shelf` asserts Nookington's stocks every tool, a
  paint, a signboard, exactly one umbrella, a cedar sapling with plain ones
  behind it, and five *distinct* flower bags; walks thirteen consecutive rolls
  to see the paint advance and wrap; and checks a new Nook's Cranny sells only
  a shovel until `sales_sum` passes the thresholds.
- `shop shelf replicates town-wide` covers the codec round trip, a sold-out row
  keeping its index, refusal of revision zero, truncation, trailing bytes, an
  oversized shelf, and delivery to a viewer standing in a house on the far side
  of town.

Still not done, and why the chain stops here: projecting the shelf into
`Save_Get(shop).items` needs the spotlight `rare_item` on the client, which
means adding it to `ShopState` and to the baseline, delta, and checkpoint
encodings. That is the next unit of work, followed by the `Buy` call site.
Stopping at a complete, tested layer rather than starting a partial one.

## Nook's counter, the museum, and NPC deltas are reachable (2026-08-07)

The last layer of the NPC-economy chain. Buying, selling, and donating now go
through the server from the actual game UI, and the two replication gaps that
blocked them are closed.

**The shelf is projected into the game.** `ShopState` gained `rare_item` -- the
game keeps the spotlight furniture in its own `Shop_c` field and it cannot be
recovered from the stock list, since nothing there marks which row was the rare
draw. With that on the wire, `Net_ApplyAuthoritativeShopStock` copies the
server's shelf into `Save_Get(shop).items`, driven off a `last_shop_revision`
watermark like the inventory projection, and `mSP_MakeGoodsList` returns early
instead of rolling locally. Rows past the server's count are cleared, so a
shrinking shelf cannot leave yesterday's item buyable off the end. The local
roll stays as the offline path.

**Selling is one atomic transaction.** The first attempt at this was wrong and
worth recording: the counter can sell several items at once, so the obvious
wiring is a request per pocket -- but each would quote the same inventory
revision, and every one after the first would come back `StaleRevision`.
`EconomyRequest` therefore gained a `slot_mask`, one bit per pocket, and the
authority sells the whole selection or none of it. The dialogue's quoted total
matches because both sides price through `mSP_ItemNo2ItemPrice`.

**The wallet cap moved server-side.** The original refuses to let the wallet
exceed `mPr_WALLET_MAX` and peels 30,000 at a time into money bag items in the
pockets, which is most of what the three sale paths in
`ac_npc_shop_common.c` are doing. With the pockets authoritative, that had to be
the server's rule or the bags would vanish on the next projection.
`EconomyConfig` carries the cap, the chunk, and the bag item; the town runtime
fills them from the generated constants, so the authority still hardcodes
nothing. Overflow starts in the slot the sold item vacated, which is what
guarantees somewhere to put the first bag.

**Buying names a row.** `Net_RequestBuyItem` finds the item's index in the
projected shelf -- the server's own list, so the lookup cannot drift -- and
quotes the shop revision. Only the payment is redirected; the item still arrives
through the usual hand-over actor optimistically, the same pattern
`Net_RequestPickup` already uses, with the projection reconciling.

**The museum is replicated.** New `ResourceKind::Museum`, a whole-collection
delta, a baseline field, and `acnet_client_museum_revision()` /
`acnet_client_museum_has()`. `aCR_putaway_demo_end_wait_init` now donates
through the server, which is what makes one town have one collection: a second
player offering a species already on display is refused rather than both
succeeding locally.

**NPC deltas exist at last.** `ResourceKind::Npc` was declared but never
produced, so NPC state only ever moved on a fresh baseline. `encode_npc_delta`
is now published zone-scoped on the hourly schedule job and on conversation
state changes, the client merges by revision rather than arrival order, drops an
NPC that left the viewed zone, and `acnet_client_npcs()` exposes the list.

Verified with `make check` (46/46, fuzz, load, chaos, 31-day soak with 5
restarts and torn-write recovery), `make sanitize`, and the Windows build. New
coverage:

- `selling a selection is atomic` -- three items in one request for one total,
  replay not paying twice, one unsellable pocket voiding the whole sale, a mask
  naming a pocket that does not exist refused as malformed, the wallet cap
  peeling a bag into the vacated slot, and the rule staying inert when
  unconfigured.
- `museum collection replicates` -- codec round trip, an empty collection as a
  valid state, revision zero and truncation refused, town-wide delivery, a
  duplicate species refused with the item left in the donor's pocket, and a
  stale collection revision told to refresh.
- `NPC state replicates` -- round trip, refusal of entity/zone/revision zero and
  a non-finite position, and zone scoping (a viewer outdoors is not told about
  an NPC in the shop).

Two mistakes caught by the tests rather than by review, recorded because both
were plausible: a first draft of the sale test registered no `PlayerView` and
tripped the shop-proximity check, and the wallet-cap case used 99,000 + 125,
which is under the 99,999 cap, so the rule correctly did nothing.

Still not reachable: **conversation leases**. `NpcAuthority` and the C shim are
complete, but a lease names a server entity and the game's NPC actors have no
mapping to the two entities the server owns (shopkeeper 1000, islander 1001).
NPC replication was the prerequisite and is now done; the mapping and the call
site are not.

## A blank town erasing every client's field (2026-08-07)

Reported as "trees and rocks disappear when you get close", and the bulletin
board with them. It was not a rendering fault. The three things named are all
foreground tiles, and the server was deleting them.

**What happens.** `Net_ApplyAuthoritativeState` writes the zone baseline's tile
chunk straight into `Save_t.fg` through `mFI_UtNumtoFGSet_common`. The server
sends a 16x16 chunk centred on the player, so the chunk follows the player
around -- which is why the erasure tracks proximity rather than acre crossings.
If the server's own foreground is empty, every chunk is an eraser, and the tile
is gone for the rest of the session.

Measured on the two-client test town by logging every projection that changed a
cell: 503 changes on one scripted walk, 503 of them to `EMPTY_NO`, including 71
trees, 6 rocks and 22 `NOTICE` (the bulletin board).

**Why the server was empty.** `Net_SubmitInitialTown` is the only thing that
installs a foreground, and its only callers are the two guide-NPC scripts that
run when a resident creates a town from the intro. It captured `Save_t.fg`
without checking that the field existed yet, and the server takes the first
bootstrap as final -- `town_bootstrapped_` closes the door. A bootstrap sent
before the save's field was generated therefore installed 7680 empty tiles
permanently. The test town's checkpoint is state version 9 with the flag set and
a foreground of nothing behind it.

**The fix, both ends.**

- `Net_SubmitInitialTown` counts occupied tiles and refuses to submit a
  foreground of nothing, so the one-shot can no longer be spent on an empty
  save.
- `Net_SubmitTownIfUninitialized` offers the world on any frame where the server
  reports the town uninitialised, throttled to once a second. Previously a
  `--quickstart` login, or any login into a town restored without a foreground,
  had no path to install one at all.
- `TownRuntime::decode_state` treats a town flagged as created whose foreground
  holds fewer occupied tiles than a single acre as never created, so an
  already-damaged town is repaired by the next resident instead of erasing
  everyone forever. A real foreground clears that floor by an order of
  magnitude; a blank town only accumulates a handful of tiles from dropped
  items, which is why the test is a floor and not "any".

**Verified.** Same scripted walk on the same town: 503 wipes to 1, a
`town_bootstrap` audit row now exists, and the client's remaining 40 projected
differences are buried-item representation (`f0xx`/`f1xx` against `58xx`/`a0xx`)
with no tree, rock or sign among them -- a separate, pre-existing discrepancy.
`make check` green including the new `blank town bootstrap repairable` case,
which installs an empty foreground, restarts, and asserts the town reopens for a
real one.

**Known-good saves are unaffected.** The erasure lives in the in-memory field;
the GCI on disk still held ~540 trees and ~36 rocks throughout. A player who
saves while the world is wiped would persist the damage, so the fix wants to
land before any long online session.

## Actor-backed tiles are not projected (2026-08-07)

Follow-up to the blank-town fix: once the server actually had a foreground,
walking around produced heavy hitches and duplicated actors -- a second gyroid
on a house was the visible case.

**Duplicates.** Houses, boards, props and misc actors live in the foreground
grid only as spawn records: `ac_birth_control` reads the name, spawns the
actor, and clears the cell to `RSV_NO` or `EMPTY_NO` until the actor is deleted
(`actor->restore_fg` puts the name back). The server's copy holds the names --
it was bootstrapped from a save -- and no server transaction ever changes them.
The projection wrote them back while the actors were alive, so birth control
spawned duplicates. Enough duplicates exhaust the fixed structure slot pool,
and a failed spawn leaves `setup_actor_flag` set, which reruns the entire
delete-and-spawn scan every frame -- the freeze.

**Hitches.** The baseline chunk follows the player, and the projection set
`mFI_SetFGUpData()` whenever a baseline arrived, rebuilding the draw and
collision tables on every re-baseline even when nothing had changed.

`Net_TileProjectable` now rejects `RSV_NO` in either direction and every
actor-backed name family (`STRUCT`, `PROPS`, `ITEM2`, `ACTOR`) in both the
baseline and delta paths, and both paths only write -- and only set the update
flag -- when the item or the buried bit actually differs from the field.

Verified by the reporter: the freezes are gone on the repaired test town.
`make test` 50/50, Windows client and server build clean.

**Known issue found in the same session:** grabbing your own equipped fishing
rod duplicated it into the inventory. Root-caused and fixed the same day -- see
the next entry.

## The submenu drag hand no longer duplicates tools (2026-08-07)

Grabbing your own equipped fishing rod off the character portrait and dropping
it into the pockets left two rods in the inventory.

While the item submenu's drag hand carries an item, that item exists in
neither the hand slot nor any pocket -- it lives only on the cursor
(`play->submenu.overlay->hand_ovl->info.item`). Two per-frame consumers
misread that transient state:

- `Net_ReconcileEquipment` saw hand `EMPTY` against an authoritative rod,
  found the rod in no local pocket, and hit its "stow into any empty slot"
  fallback -- asking the server to put the rod away before the player had
  chosen where.
- The inventory projection then painted the server's answer back into a
  pocket while the rod was still on the cursor. Dropping the cursor copy into
  another slot made two rods, and nothing corrected it until the next
  revision bump.

Both now wait for the cursor to empty (`Net_InventoryDragActive`). The drag
always settles into a pocket or back onto the portrait, and reconciliation
from that stable state names the slot the player actually chose, so the
server stows the tool where it was dropped rather than into the first free
slot.

The hold-item transaction itself was verified sound -- it is a pure swap and
cannot create an item; the duplicate was purely a client-side projection into
a moving UI state.

**Possible same-shape issue, not investigated:** the drag hand can also carry
a letter (`info.mail`), and the carried-mail projection may repaint a dragged
letter the same way. Worth checking when mail is next touched.

`make test` 50/50, Windows client and server build clean.

## Remote body animations at the game's real speeds (2026-08-07)

Reported from the first four-client visual session: remote idles ran at double
speed, and the suspicion was that most non-walk animations did too. Correct on
both counts, and entirely client-side — no wire or server change.

`Net_Remote_Player_animation_speed` returned 1.0 for every action outside
walk/run/ready-walk-net. But the game's baseline animation speed is 0.5, not
1.0: a survey of every `Player_actor_InitAnimation_Base1/2/3` call site found
131 of 133 passing `0.5f` — wait, talk, pickup, dig, swing, sit, shock, the
demo states, all of it. So every non-locomotion remote animation played at
exactly 2x. The item keyframe had already been through the identical bug and
was fixed to 0.5; the body keyframes kept the 1.0 default.

The fix, all in `Net_Remote_Player_animation_speed`:

- Default is now **0.5**.
- The velocity-derived walk formula now also covers **dash** and the **demo
  walk** — `Player_actor_CulcAnimation_Dash`/`_Run` are wrappers around
  `_Walk`, so all four states share one formula.
- The two genuine 1.0 states get an explicit case: the pitfall climb-out and
  the umbrella twirl.
- `CHANGE_CLOTH` splits on the replicated animation: the dressing-room try-on
  (`MENU_CHANGE1`) is 1.0, the Halloween prank takes the baseline. The local
  `try_on` flag is not replicated, but the animation choice is.
- The `YATTA1`/`YATTA2` `0.6f` literals in the tree are the `VER_GAFU01_00`
  branch of a version guard; this build is `VER_GAFE01_00`, whose branch is
  `0.5f`, so the default covers the cheers with no special case.
- Approximated at the baseline, with a comment saying so: radio exercise and
  the car wash (frames driven externally by the event/minigame after a 0.0
  init) and the snowball push (speed written per frame by the snowball actor).

Verification: `make test` 50/50; full Windows client build clean; the
manual-test clients restaged from `bin/`. Visual confirmation is the next
four-client run.

## Remote tool animations no longer play twice (2026-08-07)

Reported from a four-client session: a peer's fishing motion replayed from the
start on the other clients — body and tool both — for a single action on the
acting client. Two independent causes, both client-side presentation; no wire
or server change.

**A viewer restarts a remote skeleton in exactly two situations:** the
replicated tuple `(body, overlay, looping, reversed)` changes, or the render
data is fresh so `animation_loaded` is false. Resolving the ordinals for a full
fishing cycle (several call sites pass effect/index constants where animation
constants are meant — `eEC_EFFECT_TURI_MIZU` is 70, which really is
`mPlayer_ANIM_TURI_WAIT1`) gives `68 SAO_SWING1` (ready and cast deliberately
share one animation; `setup_main_Cast_rod` never re-inits the body) → `70
TURI_WAIT1` → `69 TURI_HIKI1` → `71 NOT_GET_T1` → `64 GET_T1` → `65 GET_T2` →
`66 PUTAWAY_T1`. No value repeats, so the replay was never the state machine —
it was a rebuild.

**1. The actor was destroyed on a one-frame absence.**
`ClientRuntime::remote_players` omits a player whose interpolation history is
empty (`TransformHistory::sample` fails only on an empty history), and the
history is cleared for transient reasons as well as permanent ones — half a
second without a snapshot on the unreliable channel, an entity mismatch, a zone
edge. `Net_SynchronizeRemoteActors` treated that as departure and deleted the
actor immediately; with snapshots every four ticks it was re-created ~50 ms
later, too fast to read as a disappearance, and the replacement started the
in-flight animation at frame 1. The `missing_frames > 180` grace inside
`Net_Remote_Player_move` never ran: the sync executes first, in
`Net_PreSimulation`, and deleted the actor before the move ever saw a gap.

Now the sync owns the lifetime. Zone change, scene change and a missing sample
list are still immediate; a remote that merely fell out of this frame's list is
held for `NET_REMOTE_ABSENT_FRAMES` (30) with its last pose. The move proc no
longer deletes anything. A departed peer can now linger up to half a second
longer than before — the trade against replaying every long animation.

**2. A rebuild threw the motion away.** Any appearance change — clothing,
pattern, face — destructs and re-constructs both keyframes, and
`apply_animation` then restarted at frame 1. It now captures
`keyframe0/1.frame_control.current_frame` before the destruct and resumes
there, but only when the tuple that comes back matches the one that was
interrupted. The reverse init has no start-frame parameter, so a resumed
reverse animation is placed after the call.

**3. The rod's bend animation looped the whole time a peer was fishing.**
`ITEM_MAIN_ROD_RELAX`/`ROD_VIB` are the only two item states whose keyframe the
original never advances: `Player_actor_Item_main_rod_relax/_vib` call
`Player_actor_Item_SetFrame_forUki_relax/_vib`, which write `current_frame`
outright each frame from the float's geometry, starting from the `180.0f` the
state is entered with — there is no `Item_CulcAnimation_Base` in either. The
viewer was free-running `ROD_SINARI` in REPEAT at 0.5 instead, so the rod
cycled its full bend over and over. `Net_Remote_Player_item_anim` now returns a
start frame as well as a mode, and a start frame other than 1.0 marks the state
as held: the item keyframe is initialised at 180 and not played. The float is
not replicated, so the bend is static rather than modulated.

Verification: `make test` 51/51; full Windows client build clean. Visual
confirmation is the next four-client fishing run.

Known and untouched, same area: holding A to chop or dig re-inits the *same*
animation index on the acting client, so the tuple never changes and the viewer
suppresses swings two onward — the converse of this bug. Fixing it needs a
restart counter on the presentation delta (one byte, protocol bump), because
the frame itself cannot ride a change-triggered reliable message. The take-out
item scale ramp in `Net_Remote_Player_update_item_scale` also reads an
ascending frame from an animation the original plays in reverse
(`InitAnimation_Base3` on `PUTAWAY1`), so a remote's tool snaps to full size,
shrinks away and pops back.

## House gyroids replicate and trade (2026-08-07)

The gyroid in front of each resident house — its four display slots, visitor
message, and held proceeds (`Haniwa_c` at `mHm_hs_c:0x25D4`) — was entirely
client-local: items put up for sale never appeared on other clients, purchases
diverged every save, and the proceeds only existed for whoever sold. It is now
server state end to end. Protocol v16 → **v17**, town state v9 → **v10**.

Server and wire:

- `HouseState` gains a `GyroidState` (own revision, four `GyroidItem`s, the
  128-byte message as opaque bytes like mail text, bells). Baselines carry all
  four slot-indexed after the museum block; `ResourceKind::Gyroid` deltas keep
  them live town-wide, like `Resident` — the gyroids stand in the field.
- One `GyroidRequest`/`GyroidResult` message pair (34/35) with three
  operations in `HousingAuthority::apply_gyroid`, all idempotent, revision-
  quoted, and requiring the actor in the town field zone (the interior zone
  does not authorize the exterior):
  - **Update** (owner): replaces display and message whole; the server diffs
    the display against the old one and balances it through the owner's
    pockets, exactly as the whole-house furniture submit does. Bells never
    travel this way.
  - **Take** (guests only; the owner's path is Update): one displayed item
    into the first empty pocket, price from wallet when for sale, proceeds
    into the gyroid. Display-only slots (`TRADE_1`) and the unused `TRADE_3`
    refuse; two guests racing for one item is a revision conflict.
  - **Collect** (owner): proceeds into the wallet, breaking the cap into
    money bags under the same overflow rule as a Nook sale.
- After an accepted operation the server broadcasts the gyroid delta and
  re-baselines the acting connection — a diffed update or an overflow bag
  cannot be mirrored from the result alone.
- Checkpoints persist the gyroid inside each house record (v10, version-gated
  read, older checkpoints load with virgin gyroids).

Client:

- `Net_ApplyAuthoritativeGyroids` projects all four into
  `Save_Get(homes[slot]).haniwa`, where the gyroid actor, the tag overlay and
  the message board already read — so guest browsing, the price messages, and
  the "your gyroid is holding bells" hint all work unmodified. Skipped while a
  submenu is open, the same hazard the inventory projection avoids. First
  contact with a virgin server gyroid keeps the local block instead, so a
  blank server state cannot erase the game's default greeting.
- Owner edits need no overlay hooks at all: a hash watch over the owner's own
  block (items and message only — bells are excluded) submits it whole when
  the submenu closes and the block matches neither what was projected nor what
  was last sent. The tag overlay, drag hand, and message editor all settle
  into the save first, so one falling-edge watch covers every mutation path.
- Two decomp call sites: `mTG_get_proc` (`m_tag_ovl.c`) sends the take before
  its optimistic local copy, and `aHNW_check_proceeds`
  (`ac_haniwa_move.c_inc`) sends the collect. Both force the inventory and
  gyroid projections so the server's verdict lands either way.

Verification: `make check` (51/51 unit/integration, fuzz, load, chaos,
month-soak with v10 checkpoints, smoke) and `make sanitize` (51/51) pass; the
new "gyroid replication and trade" case covers the codecs (including malformed
terms), the owner diff, guest purchase, free take, display-only refusal,
owner/guest authorization, stale revisions, replay, and wallet-cap overflow
into bags. Full Windows client build clean; **not yet drawn on screen** — the
four-client launcher run is the visual gate, same as the economy path.

Known limitations:

- Turnips displayed on a gyroid no longer spoil online: `mAGrw_SpoilKabu`
  runs client-locally at boot and the projection reverts it. Spoilage needs a
  server-side daily job to be correct; today's behavior is "the server never
  spoils".
- A guest can only pay from the wallet. The original breaks pocket money bags
  into the wallet mid-purchase; online that local break is reverted by the
  projection and the server refuses a short wallet — the same limitation the
  Nook counter has.
- An owner edit refused as `InvalidState` (a pocket/display imbalance the
  diff cannot settle) stays visible locally until the next gyroid delta
  repaints it; the pockets themselves are already reverted by the inventory
  projection.

## Shared surfaces, the stalk market, and the rest of the face (2026-08-07)

Four fixes from a fresh sweep for state that is required for gameplay or
visuals and was still simulated per client. Protocol went 17 -> 20 across three
of them; checkpoints went 10 -> 12.

### Remote players cast no shadow

`ac_net_remote_player.c` never called `Shape_Info_init`, so `shadow_proc` kept
`Actor_info_make_actor`'s NULL default and `Actor_draw` skipped the shadow
entirely. Remote players floated over unshaded ground in every zone.
`VISUAL_REPLICATION_AUDIT.md` had this backwards -- it said remotes *always*
draw a shadow -- so the entry has been corrected.

They now build the same `mAc_ActorShadowCircle` the local player does, and
`draw_shadow` follows `Player_actor_SetupShadow`'s per-main-index table from the
replicated action. Only the non-NORMAL entries are named rather than copying the
121-entry array; every one of them is a state where the player is inside or
underneath something, which is what confirms the reading. The shadow starts
disabled and is enabled from the branch that confirms the skeleton is built,
because `Actor_draw` runs `shadow_proc` whether or not the draw proc bailed out.

The same commit added the running lean. `Player_actor_set_lean_angle` derives
its pitch from the body animation's playback *speed*, not from the ground normal
as the audit claimed, so the viewer that already reproduces that speed gets it
for free.

### A house's surfaces are shared (protocol v18)

`AcNetHouseState` carried the furniture standing on a room's surfaces but never
the surfaces: two players in the same room each saw their own wallpaper and
carpet, and repainting a house or hanging a design on its door was invisible to
everyone but the owner. `HouseSurfaces` adds, per floor, a wallpaper index, a
flooring index and the two `mHm_fllot_bit_c` flags, plus the exterior palette,
its two pending values, and the door design. It rides the existing whole-room
submit, so contested edits resolve exactly as furniture does.

Indices are carried opaquely -- they address game tables the server has no
reason to know the size of, and `aMI_CheckFloorWallIndex` already clamps a bad
one as the room loads. The pattern byte is different: the original defines two
bits and anything else is rejected at the decoder rather than masked. The
candidate hash covers the block, because repainting a wall moves nothing in the
furniture grid and the submit would otherwise never fire.

### Turnips could not be sold at all (protocol v19)

The original prices turnips from the town's weekly schedule rather than from
`mSP_ItemNo2ItemPrice`, so they are absent from the generated price tables, so
`shop_sell_price` returned 0 -- and `EconomyAuthority` reads a zero price as
"unsellable" and refuses the whole transaction. Nook would not take them.
Underneath that, every client rolled its own week, so no two players were quoted
the same price and none of those prices matched what the server would pay.

`TurnipMarket` is now town state, carried in the baseline and kept live by a
town-wide `Turnip` delta. The daily job rolls a fresh week when the town date
lands on a Sunday, reproducing `Kabu_decide_price_schedule` including the random
walk's inverted-looking clamp. The sell resolver consults it before the static
tables, multiplying by `{10, 50, 100, 0}` and deliberately not dividing by the
sell/buy ratio. Client-side the schedule is projected into `Save_t`, so
`Kabu_get_price` and the counter dialogue quote the authoritative number with no
further plumbing, and both sides multiply the same per-turnip value by the same
bundle size. `Kabu_manager` returns early while connected.

`TownDate` gained a weekday, floored rather than truncated so it stays correct
before the epoch.

### The face, umbrella and hand a viewer could not draw (protocol v20)

Category C of `VISUAL_REPLICATION_AUDIT.md`, batched into one bump as that page
recommended. `PlayerAppearanceBits` is six bytes -- bee swell, decoy and colour
flash in one flag byte, plus the sunburn rank, the umbrella action, and the item
held mid-pickup -- riding `InputCommand` up and the presentation delta down.
They are kept out of `AppearanceUpdate` on purpose: its 1/s rate bucket and
journal are the wrong shape for a face that swells and subsides. That was the
open question the audit left.

`mPlib_Load_PlayerFaceTexAndPalletEx` is the remote-facing form of the two
resource pickers that previously only worked for the local player, including the
tanned-palette branch and its suppression under the decoy face.

A replicated `aTOL_ACTION_DESTRUCT` is never forwarded to the umbrella -- its
lifetime belongs to the tool-change path and the actor's `dt`. The animation
*phase* is still deliberately absent: a bare start frame is not enough, because
by the time the delta lands the animation has moved on.

### Verification and the growing debt

`make check` and `make sanitize` are green at every commit -- 52/52 tests, fuzz,
load, chaos, and the 31-day soak, which crosses several Sundays and so exercises
the turnip reroll and its checkpointing. New cases cover the house-surface round
trip and its rejected pattern byte, an end-to-end assertion that the *second*
client receives the surfaces, 400 weeks of turnip rolls across all three trends
asserting no zero price ever appears, and a decoder case for every bound the
appearance bits added.

**Seen on screen 2026-08-08.** The maintainer ran the four-client launcher
against a real disc and reported the visuals correct. That clears the backlog
this page had been accumulating: the shadow, the run lean, the shared room
surfaces, the face resource bits and the umbrella, plus the remote presentation
work from 2026-08-06 that had been sitting unverified underneath them. The
report was a blanket "it all looked right" rather than a per-feature checklist,
so treat it as "nothing was visibly broken in a live four-client session"
rather than as targeted confirmation of each item.

Not covered by that pass, because they need a scenario rather than a look: a
bee-stung face, a tanned face, an umbrella opening, and a mid-pickup item are
all transient states nobody was necessarily in.

## Three quick wins: the gate, Nook's level, the music box (2026-08-08)

Protocol 20 -> 22, checkpoints 12 -> 13.

### `make check` now catches a client/server link split

The root Makefile builds one flat list of every net/ and server/ source, so a
symbol only server code defines still resolves and the whole gate passes. The
shipped client does not link that list -- `pc/CMakeLists.txt` gives
`acnet_client` a strict subset -- so a call from `c_api.cpp` into a server-only
translation unit built clean under `make check` and failed only in
`build_pc.bat`. That is exactly how `turnip_sell_price` got through.

`make client-link` links the object files CMake's client target is made of and
nothing else. Linking them directly rather than through an archive is what makes
it strict: an archive member is only pulled in if something already references
it, whereas a direct link must resolve every reference in every object. The
source list is parsed out of `pc/CMakeLists.txt` rather than copied, so it
cannot drift from the target it checks, and a source the client needs that the
Makefile never compiles is reported separately -- that case is worse, since
`make check` would not be building it at all.

Verified by reintroducing the bug: dropping `turnip.cpp` from the target
reproduces the same undefined reference the Windows link produced. It reuses
objects the suite already compiled, so it costs a fraction of a second.

### Nook's upgrade level is server-owned (protocol v21)

`tier` and `sales_sum` were persisted and drove the shelf, but nothing advanced
them: `mSP_PlusSales` was a local call, so online the store never upgraded and
Nook's Cranny never unlocked the net, rod or axe. Each client also accumulated
its own total, which would have upgraded the store for that player alone.

The server now adds to the total on every accepted `Buy` and `Sell` -- full
price for a purchase, half the payout for a sale, the two `mSP_PlusSales` call
sites -- and clamps at the next tier's threshold, which is what stops one large
transaction skipping a tier. Nookington's additionally needs `visitor_shopped`,
this town's equivalent of the original's `visitor_flag`: an account holding no
resident slot has shopped. Both fields ride the shelf, so a viewer learns about
an upgrade; a `Sell` republishes the shelf only when the tier actually moved.

Client-side `mSP_PlusSales` returns early while connected and the level is
projected into `Save_t`. `mSP_RenewShopLevel` is deliberately not called: it
recomputes the level from the local total, which is the derivation the server
now owns.

### Each house's music box (protocol v22)

The last field missing from `AcNetHouseState`. It rides the whole-room submit
like the furniture and the surfaces; the island cabin keeps its own.

The finding's other outstanding item, the mailbox, was stale when written -- a
house mailbox's letters have been server-owned through the mail family and their
own `MailboxState` revision since that work landed. Corrected in the audit
rather than left to send the next reader hunting a bug that is not there.

### Verification

`make check` and `make sanitize` green at every commit, and `make check` was
re-run from a clean tree, a forced full rebuild, and an incremental one to be
sure the new link step behaves in all three. 53/53 tests, including new cases
for the tier ladder (the clamp blocking a tier skip, Nookington's visitor gate,
saturation instead of wraparound, and an undefined tier rejected at the decoder)
and the music box crossing the wire to a second client.

Not yet seen on screen: the shop upgrade and the music box. Neither was
reachable by an operator, which is why the same day added
`--set-shop-sales AMOUNT [--shop-visitor]` and `--grant-song SLOT=SONG`.
`--grant-bells` deliberately does not move the shop level -- lifetime sales
accrue only from committed transactions, and bells sitting in a bank have not
been spent -- so without the new command, reaching Nookington's meant pushing
240,000 bells through a five-row shelf. K.K. Slider is not modelled at all, so
a song had no path.

Both journal before reporting success and write an audit row. `set_shop_sales`
does not call `commit_transaction`, because a transactions row is keyed by
account and one town has one store. Finding that out was itself a small fix:
`record_transaction` rejected account 0 with a bare `return false`, so the
first version of the command failed with an empty message. It now says what
was wrong. The console reports the store as well, since a level nothing
displays is a level nobody can check.

## The town tune, and why regeneration is blocked (2026-08-08)

### The town tune is town state (protocol v23)

`Save_t::melody` was local, so every player heard a different town on the hour
and at the gate, and retuning at the town hall reached nobody. It now rides the
baseline and a town-wide delta, with the same contested-edit treatment a house
or gyroid gets: the request quotes the revision it saw, a stale one is refused
with the current tune returned, and a replayed key returns the original result.
`mMld_SetSaveMelody` sends the request rather than writing the save.

### Local daily regeneration no longer runs online

`mAGrw_RenewalFgItem` is skipped while connected. This is not a feature being
removed -- it is dead work being stopped. Every effect it has lands either in
`Save_Get(fg[][])`, which the authoritative projection rewrites as soon as the
player walks two tiles (`refresh_interest_chunk`) and wholesale after the
server's daily job clears `has_exterior_chunk`, or in the pockets, which the
inventory projection rewrites just as fast. Online it never survived; it only
produced weeds and fossils that appeared and then vanished.

The timestamp still advances. Leaving `all_grow_renew_time` stale would tell the
game no renewal had happened since the town was founded, so the first offline
boot after a long online session would try to catch up months at once, and
`mAGrw_CheckKabuPeddler` would misjudge the turnip seller meanwhile.

### The noticeboard is town state (protocol v24)

The board exists so townmates can leave each other notes, which a local copy
cannot do -- every player was writing to their own fifteen slots and reading
nobody else's. The server holds the posts and owns the eviction, which is the
contended part: at fifteen posts the oldest drops, and done on each client they
would drop different posts and disagree afterwards.

The authoritative list is dense and oldest-first rather than fifteen fixed
slots, so the client fills the tail with the game's clear-code sentinel as it
projects and the server never has to know what that sentinel is.

### Why the server cannot yet do the regeneration itself -- read before trying

This is the blocking finding, and it is worth stating precisely because the
obvious plan does not work.

Porting `m_all_grow_ovl.c` (3158 lines: weeds, flowers, trees, fossils, gyroid
burial, money rock, the dump, snowmen, turnip spoiling) to the server fails on
its first input, not its last. Weed placement calls
`mCoBG_Attribute2CheckPlant(col->data.unit_attribute, &wpos)` and fossil and
money-rock placement call the equivalent flat-ground tests. **These read the
town's per-tile collision attributes, and the server has no copy of them** --
`grep unit_attribute net/ server/` returns nothing, and it cannot get them from
assets it is forbidden to hold. `TileState` carries item, condition, terrain,
buried and placed_furniture; none of that says whether a cell is plantable or
diggable.

So server-side regeneration needs a decision first: extend the town bootstrap
to carry a per-tile attribute mask (the client already sends the tile grid and
the buried flags, so this is the natural place), and settle how that mask stays
correct when terrain is edited. Only then is the port itself worth starting.

Until that lands the town does not regenerate online -- no new weeds, fossils,
gyroids or money rock -- which is a real missing feature, but a visible and
consistent one rather than the silent per-client drift it replaced.

**The lost & found is blocked behind it.** `PoliceBox_c` is a twenty-item array
and would be easy to replicate, but `mPB_force_set_keep_item` is its only
filler and that lives inside the regeneration path. Replicating it today would
replicate a permanently empty box.

### The remaining backlog is two decisions, not a list of tasks

Everything cheaply finishable has been done. What is left divides into:

**1. A directly granted item still vanishes -- and fixing it is a decision.**
`Net_ApplyAuthoritativeState` rewrites all fifteen pockets when the server's
inventory revision changes, and *only* then. So when a villager hands a player
an item, `mPr_SetPossessionItem` writes the pocket, the server never hears
about it, and the item sits there working until the player's next pickup,
purchase or catch bumps the revision -- at which point it silently disappears.
Works until it doesn't, which is the worst shape a bug can have.

The fix is a grant transaction, and that is where it stops being a coding
question. The server does not model villagers, so it has nothing to validate a
grant against: the request would reduce to "the client says give me this item",
which is an item-duplication cheat wearing a transaction's clothes and runs
straight into `MASTER_PLAN.md`'s rejected-approaches list. The alternatives are
to accept that in a private invite-key town, to refuse gifts outright until
villagers are server-side (visible and honest, but removes gameplay), or to
leave it. **This wants the maintainer's call, not a unilateral one.**

**2. Villagers.** Unchanged, and still the roadmap phase everything else waits
behind: conversation leases are built but cannot be reached without an NPC
identity mapping, and villager gifts, favours and friendship are the reason
finding 2 above matters at all.

## Villagers are server-owned, phase one (2026-08-08)

`Save_t.animals[]` was the largest remaining source of permanent divergence, and
it hid well: town generation seeds its roll from the town seed, so a *fresh*
save on every machine produced the same fifteen villagers. From the second boot
onward each client evolved its own copy alone -- `mNpc_Grow` moved somebody in
behind a local `RANDOM(100)`, move-outs came from local dialogue, and clothes,
mood and catchphrases drifted apart until two players no longer agreed about who
lived in the town.

### What landed (protocol v25)

The **roster** is town state: fifteen slots, each carrying the identity
(`npc_id`, origin town, `name_id`, personality), the house acre and unit, the
catchphrase, shirt and pending shirt, pattern and umbrella ids, mood, the
`is_home`/`moved_in`/`removing` flags, previous town, the spawning player's
name, and the fourteen inter-villager relations. `mNpc_Grow` returns early while
connected.

**The server does not invent villagers.** It holds no name, species or
personality tables and is not allowed to, so the roster rides the
`TownBootstrap`: the first resident's generation is the source, through the same
codec the baseline uses so the two cannot disagree about its shape. Once adopted
it is the server's and a later bootstrap cannot overwrite it. Because clients
send a bootstrap on *every* login (that is also how appearance is saved), a town
whose checkpoint predates this adopts its roster on the next login rather than
needing to be recreated.

Each occupied slot is now a real server NPC entity at `kVillagerEntityBase +
slot`, derived rather than looked up so both ends compute the same identity from
the same roster. That is re-synced after a checkpoint load as well, since a
restart restores the roster but not the entities derived from it. The console
consequently counts the town's actual neighbours instead of the two placeholder
service NPCs, so a fresh town reads 0 rather than a misleading 1.

`Animal_c::memories` is deliberately **not** carried. It is the per-player
relationship record -- seven eighths of the 0x988-byte struct -- and it is
account-scoped rather than town-scoped; projecting it from a town-wide roster
would hand every player the same friendships. Vacating a slot therefore clears
the identity and nothing else.

### Phase two: the turnover (protocol v26)

Phase one froze the town -- nobody moved in or out. That is now server-decided,
along the split the bootstrap already established: the server owns what must be
single-valued, the client owns what needs the game's tables.

The server owns *when* a move-in is due (one per day, into a real vacancy) and
*which slot*. It publishes the opening with a seed; a client runs `mNpc_Grow`'s
own roll against that seed and offers the result. Every client may offer, the
first accepted closes the opening, and because they all seed from the same value
they are offering the same villager -- so the race has no visible outcome and
needs no election. A move-in naming a character already in town is refused,
since the roster is keyed by who they are.

The two conditions the original checks that the server cannot -- the player
belonging to this town, and having spoken to every current villager -- stay on
the client where the data is. `mNpc_Grow`'s selection half was split into
`mNpc_GrowSelectAndPlace` so the networked offer runs the same roll rather than
a reimplementation of it.

Move-out is the mirror: the original decides a departure from dialogue, which is
client-side, so the client reports it and the server owns what follows -- the
slot empties at the next daily turnover, not on announcement. The client detects
it by **diffing** the local `removing` flag against the authoritative one rather
than hooking the dialogue, which catches the town-transfer path and the
conversation path without having to find each one.

**`make check` earned its keep here.** The roster is stored in checkpoints
through the wire codec, so the two cannot disagree about its shape -- worth
having, but it means changing the wire invalidates stored data. The smoke step
refused to start on a checkpoint the *previous build* had written, which is
exactly the failure that step exists to catch. The blob is length-prefixed so
the case is recoverable: a version-16 roster is skipped rather than rejected and
the next bootstrap re-adopts one. Checkpoints are at 17.
### Phase three: conversation leases (protocol v27), and a bug from phase one

**A refilled slot inherited the previous villager's memories.** The roster
projection cleared only the identity when a slot changed hands, so a player's
relationship with whoever moved out -- along with their contest quest and stored
mail -- was silently reattributed to whoever moved in. The original calls
`mNpc_ClearAnimalInfo` for exactly this and the projection now does too, but
only when the character actually changes: doing it every projection would wipe
every player's relationships whenever any villager anywhere altered the roster.

**Leases are wired.** The earlier entry here said this needed a wait state
inside the talk machine and wanted a disc first. That judgement was based on
`aNPC_act_talk_init_proc` being the only hook; the actual gate is
`aNPC_normal_talk_request`, which already returns a boolean its callers treat as
"not now" and which runs *before* anything is started. Refusing there needs no
wait state and has nothing to unwind, which is what made it safe to do.

`NpcState::conversation_owner` is replicated so the check costs no round trip --
it happens the instant a player presses A. Taking and releasing stay optimistic:
two players who press within one round trip both still get a conversation, which
is what happened before any of this existed. What the gate removes is the common
case of walking up to somebody already mid-conversation.

Worth recording: the first version of the release passed lease id 0, which
`release_conversation` matches exactly and would never have accepted -- the
villager would have stayed busy until timeout. Reading the authority caught it;
no test would have.
### Phase four: villager positions (protocol v28)

The last visible villager gap. Every client ran the NPC AI itself, so two
players standing together watched the same villager walk in different
directions.

Villagers cannot be simulated server-side -- pathfinding, collision and the
schedule tables are all missing there -- so the server **designates one
connection to simulate** and relays what it reports, the same shape the roster
bootstrap and move-ins already use. The host is the lowest connected account, so
every client computes the same answer and a re-election is stable; it is
re-elected from the tick rather than from join and leave separately, so no path
can forget.

Poses ride the Snapshots channel, bounded at the roster size and rate-limited --
a walking villager a few frames stale is indistinguishable. Followers **ease**
toward the reported pose rather than snapping: the local AI is still running and
still writing the actor's position, because suppressing it would mean reaching
into the NPC state machine, so each frame it pulls one way and the follower
pulls back. Easing keeps that from reading as a stutter and the authoritative
position wins over a few frames. A jump past a threshold is taken directly,
since that is a scene change rather than a walk.

**This is the piece most likely to need tuning on screen.** The ease factor and
the send rate were chosen by reasoning about the tug between local AI and
authoritative pose, not by watching it. If villagers look jittery or rubber-band
on a follower, those two numbers are the first thing to try.

### Phases five and six: memories and the special visitor (v29, v30)

**Villager memories are server-owned.** An earlier entry here argued they could
stay local, because each player having their own friendship with each villager
is what the original does. That reasoning was right about the mechanism and
wrong about the destination: the extended-residents plan boots the client from a
server baseline with *no local save at all*, and an unreplicated memory is then
a relationship that silently resets every login. It is the only record that a
player and a villager have a history.

`Anmmem_c` rides as its own 312 bytes, opaque. Account-scoped rather than
town-wide, so it sits in that account's baseline beside the inventory. The
account is taken from the connection, never the request: an account may only
write its own memories. The client submits on content change and only submits a
memory that already exists -- allocating one would tell the town this player had
met somebody they had not.

**The special visitor is town state.** Redd, Saharah, Katrina, the designer, the
artist and the sale were rolled per client, so their contents diverged even
where the date did not -- two players were offered different paintings, and the
"already bought" flags were private to each machine. The schedule dates come
from town-seeded common data so the events lined up, which is why this went
unnoticed. `kind` is validated because the game indexes a table with it; the
rest is opaque POD whose size is asserted with `_Static_assert`, so a decomp
change to any event struct fails the build rather than silently truncating.

**A persistence bug caught while writing it:** the event block first landed
between the villager-memory count and its records, so a checkpoint would have
written one order and read another. Found by re-reading the save/load pair, not
by a test -- the month soak would only have caught it on a restart that happened
to carry memories.

### Phases seven and eight: NPC gifts and the event flags (v31, v32)

**NPC gifts are a server transaction.** This is the item-loss bug that had been
flagged as needing a maintainer decision. Villagers being server-side did not
unblock it the way expected -- the NPCs handing these out are not all villagers,
so a conversation lease cannot gate it -- but the decision resolved itself once
the alternative was written down: leaving it alone was *actively losing items*.
A gift written locally sat in the pocket working until the player's next pickup
or purchase, then silently vanished.

So `EconomyOpType::Grant` is client-trusted, and the only one here. The server
checks a real item and a free pocket; everything else is bounded by the
per-message rate limit, the journal and an `audit_log` row, so abuse is visible
after the fact even if it cannot be prevented in front. That is the trust
already extended for player movement, and the town is invite-keyed.

Nine deliberate call sites, not one hook on `mPr_SetFreePossessionItem`: the
gyroid proceeds peel bells into money bags through that same function and
already have their own transaction, so a blanket hook would have granted twice.
Each site falls back to the original local write offline.

**The event flags ride with the special visitor.** `mEv_CheckFirstJob` and the
Halloween status are read from them and gate what villagers do and which
dialogue runs, so leaving them local kept towns diverging in a way the visitor
alone did not explain.

**Villager schedules need no replication of their own** -- worth recording,
because it looked like the last gap. `mNPS_schedule_c` lives in common data
rather than the save: it is *derived* from the roster, the town clock and these
event flags, all three now shared, and positions come from the simulation host
regardless.

### The persistence rule this cycle established

Twice now a wire change has invalidated a stored blob, because the roster and
the event block are both persisted *through the wire codec* -- which is worth
having, since the checkpoint and the baseline then cannot disagree about shape.
The rule is now written beside the version constant: anything persisted that way
needs a version bump **and** a tolerant read of the old version. Both are
recoverable because the blobs are length-prefixed. `make check`'s smoke step is
what catches it, by starting on a checkpoint the previous build wrote -- it has
now done so twice.

### Still open on villagers
Nothing on villagers is outstanding that is not bug-testing. The roster,
turnover, leases, positions, memories, the special visitor, the event flags and
NPC gifts are all server-owned; schedules are derived from state that already
is. What remains is watching it run -- see the tuning note on positions above,
which is the piece most likely to need adjustment on screen.

## Fish, insects and the fishing float (2026-08-08)

### Delivered: the seasonal offsets (protocol v33)

`gyoei_term_transition_offset` and `insect_term_transition_offset` shift by up
to five days when a species stops or starts appearing, and every client rolled
its own. Two players could disagree about whether it was still bass season --
which decides *which fish and insects exist at all*, a stranger inconsistency
than any individual spawn. They ride the town event payload now, since they are
the same kind of thing: a value the town rolls once and everyone must share.

### Not delivered: individual spawns, and why

Fish and insects are rolled per client from `fqrand()` (`ac_set_ovl_gyoei.c`,
`ac_set_ovl_insect.c`), so two players at the same pond see different fish.
This is real and visible, and it is **not a quick seed fix**:

- Seeding the roll from a town value does not work. `fqrand()` reads one global
  stream, and each client's stream diverges immediately because they make
  different numbers of calls. Making it deterministic would mean deriving a seed
  at every decision point from (town, acre, time), which changes the spawner
  from "re-rolls as you walk in and out" to "fixed for a period" -- a gameplay
  change, not just a consistency one.
- The server cannot own it. Spawn placement needs the water and terrain data it
  has no copy of -- the same blocker as daily regeneration.
- The host-simulates pattern from villager positions *would* fit, and the
  transport already exists. What does not exist is the hard half: a follower
  must stop its own set manager spawning and instead create and destroy actors
  from replication. Villagers are fifteen long-lived actors; fish and insects
  are created and destroyed constantly as you walk, so this is materially more
  invasive than the villager work was.

### Not delivered: the remote fishing float

The float is a **scene singleton**. `Player_actor_SetActorUki` claims it with
`Actor_info_name_search(mAc_PROFILE_UKI)`, and so does the fish AI
(`ac_gyo_test.c`, `ac_gyo_kaseki.c`). Spawning a second one for a remote would
make the fish target whichever the search happened to find first -- so the
obvious approach risks **breaking local fishing**, which is far worse than
remotes having no visible float.

Doing it properly needs two things: a replicated cast position (the float is out
in the water, not at the hand, so it cannot be derived like the held tool), and a
rendering path that is not the shared UKI actor -- drawn as part of the remote's
presentation, the way the umbrella and balloon already are.

## Compatibility note for the protocol version

**Protocol v16, town state v9.** Two independent lines of work both landed as
"v15" before they met: the item-drop presentation change grew the `Tile` delta
with an acting account and a `TileChangeCause`, and the shop work added
`native_fruit` to `TownBootstrap`, `rare_item` to `ShopState`, and a `slot_mask`
to `EconomyRequest`. Each was v15 on its own branch, and the merged wire format
is neither of them, so it is v16 rather than a third meaning for the same
number. Negotiation is strict: client and server must be updated together.

The town directory is forward compatible — v4 through v8 state files still load,
with older records receiving migration defaults, a v7 held item migrated out of
the appearance into the inventory, and a pre-v9 town starting with no recorded
native fruit, no spotlight rare item, and the seed-derived shelf state until its
next bootstrap and daily roll supply them. `TileState` on disk did not change;
only its wire delta grew. Once a v9 checkpoint is written an older server can no
longer read the directory, so back up the town folder before upgrading.

The invitation-key and dragged-furniture changes above carry no wire or state
version of their own.

The preceding v14 work also moved town state to v8. The town directory is
forward compatible — v4 through v7 state files still load, with older records
receiving migration defaults, and a v7 held item is migrated out of the
appearance into the inventory — but once a v8 checkpoint is written an older
server can no longer read the directory. Back up the town folder before
upgrading.

## Verified release candidate

The 2026-08-06 resident-roster work was verified with:

- `make check`: 37/37 tests, 50,000-input protocol fuzz, eight-client encrypted
  load, loss/jitter/duplication/reordering/reconnect chaos, the accelerated
  31-day soak (5 restarts, torn-write recovery), and headless server smoke.
- `make sanitize`: the same 37/37 under ASan and UBSan.
- New coverage: `resident roster replication` (codec round trip, all-vacant
  roster, refusal of an identity attached to a vacant slot, out-of-range gender,
  occupied-with-no-account, truncation, and town-wide delta delivery to a viewer
  in another zone), roster assertions inside the baseline round trip, and an
  end-to-end assertion in `production client loopback` that both connected
  clients name the owner of both occupied houses — including the other player's
  updated name after an appearance change.
- The Windows client and dedicated server both build clean via MinGW-w64 CMake
  plus Ninja (`pc/build64`, `NETCODE_ENABLED=ON`), covering the touched
  decompiled translation units (`m_map_ovl.c`, `m_net_hooks.c`) and the new
  `acnet_client_residents` C boundary.
- Not yet run for this change: `scripts/smoke_windows.ps1` and
  `scripts/smoke_online_windows.ps1` with a legitimate disc. The map labels are
  a visual change and have not been seen on screen.

The preceding mail/banking authority and operator-gift work was verified with
`make check` (36/36), `make sanitize`, and the `AnimalCrossingServer` admin CLI
end to end against a real town directory (`--list-accounts`, `--grant-bells`,
`--send-mail` with attachment and body, persistence across restarts, `audit_log`
rows, and an eleventh letter refused with a non-zero exit status).

The earlier 2026-08-06 release gate completed successfully:

- `make check`: 31/31 tests, 50,000-input protocol fuzz run, eight-client load,
  loss/jitter/duplication/reordering/reconnect chaos, accelerated 31-day soak,
  torn-write recovery, and headless server smoke all passed.
- `make sanitize`: the same 31/31 tests passed under ASan and UBSan.
- Native Windows CMake/Ninja produced only PE32+ x86-64 client and server
  executables. Passive graphical smoke kept offline and authenticated online
  clients alive using a user-supplied CISO; a second passive smoke connected
  two real clients simultaneously and observed 2/16 on the server dashboard.
- The process-owned Windows operator dashboard was manually confirmed visible;
  its separate redirected status/log stream also passed automation.
- `dist/ACGC-PC-Port-dedicated-town-Windows-x86_64.zip` passed its internal
  SHA-256 manifest and forbidden-asset scan. Archive SHA-256:
  `da0346e24b83fb93d51146a1d7287de7b6ecf5ebf0e6dede16d4b8193d595e09`.

## Next recommended task

*(The client/server link-split gap is closed -- `make client-link` is in
`make check` as of 2026-08-08. See "Delivered systems".)*


Nook's counter, the museum, and the shelf are wired end to end. What is left is
proving it on screen, and the one transaction still out of reach:

1. **Verify against a real client.** This is now the blocking item, not a
   footnote. Buying, selling, donating, and the projected shelf have been built
   and unit-tested but never drawn on screen -- this machine has no SDL2. Nook's
   counter (single and multi-item sales, a sale that breaks the wallet cap into
   bags, a purchase someone else just took), the museum, the ABD, the loan
   counter, and the mailbox all need `scripts/smoke_online_windows.ps1` and a
   disc. Expect the *dialogue* to be where problems surface: the message flow
   assumes the local mutation already happened, and it now happens a few frames
   later when the result lands.
2. **Conversation leases.** The one built-but-unreachable transaction left. It
   needs a mapping from the game's NPC actors to server entities: either the
   server naming its NPCs in a way the client can match, or the client matching
   `acnet_client_npcs()` by zone and position. Then `Net_RequestConversation` at
   the talk entry point, so two players cannot hold Nook at once.
3. **Shop tier and lifetime sales.** `ShopStockState::tier` and `sales_sum` are
   persisted and drive the shelf, but nothing advances them -- `mSP_PlusSales`
   is still a local call, so the store never upgrades online and Nook's Cranny
   never unlocks its later tools.
4. **Wire `gen_shop_tables.py --check` into `make check`.** The generated tables
   are load-bearing for pricing now, and nothing enforces their freshness
   automatically.

Still outstanding from the previous cycle, and unaffected by this change:

- **Carried-letter handling.** Rearranging letters and storing them on a card
  still go through the generic drag hand, which is refused online. That needs
  either a server-side letter reorder operation or a rule that the carried
  order is presentation-only.
- **Posting a letter.** The post office addresses a recipient by name and the
  server by account, so a name-to-account lookup has to be exposed before
  `AttachMail` can be reached from the counter.

### Beyond that: villagers

The same audit found the villager gaps below. They are a roadmap phase, not a
follow-up, and are listed here so the next cycle starts from a real inventory
rather than re-deriving it.

- **`ResourceKind::Npc` is declared and never produced.** No NPC delta is ever
  appended, so even the two server NPCs (shopkeeper 1000, islander 1001) are
  frozen between baselines. There is also no client accessor for the NPC list —
  `c_api.h` has `acnet_client_remote_players()` and nothing equivalent — so the
  decoded `ZoneBaseline::npcs` is unreachable.
- **Conversation leases are unreachable.** `acnet_client_request_conversation`
  exists and `NpcAuthority` is tested, but no `Net_RequestConversation` hook
  exists and nothing in `src/` calls it. Two players can hold the same villager
  in conversation at once.
- **The hourly `npc-schedules` job writes state nothing reads.**
- **Villagers diverge permanently after the first boot.**
  `mSDI_OnlineTownGenerationBegin` seeds `sqrand()` from the server's town seed,
  so a fresh save generates the same roster — after which each client owns a
  private `Save_t.animals[]` that evolves alone. `mNpc_Grow()` runs at boot
  (`m_start_data_init.c:577`) behind a local `RANDOM(100)` roll, a per-save
  `last_grow_time`, and a "has talked to every villager" gate, so move-ins
  differ per client; move-outs are chosen locally from dialogue. Friendship,
  memories, mood, clothes, catchphrase, `animal_relations[]`, and home acre are
  all client-local and unpersisted.
- **`ANIMAL_MEMORY_NUM` is 7** against a 16-player capacity, so the per-villager
  memory table cannot hold a full town.
- **Villagers cannot perceive remote players.** Remote players are deliberately
  not `PLAYER_ACTOR`s; villager AI targets the local player exclusively
  (`ac_npc2_action.c_inc:96`, `ac_npc2_think.c_inc:71`), and 72 of the 230 files
  under `src/actor/npc/` reference `GET_PLAYER_ACTOR` or `Now_Private`. This one
  runs straight into the sole-player audit in `ARCHITECTURE_AUDIT.md` and must
  not be mechanically rewritten.
- **Special and event NPCs are unmodelled.** Redd, Gulliver, Wisp, K.K.,
  Katrina, Saharah, and the holiday set all spawn from local date and local RNG.
  `ac_halloween_npc_talk.c_inc:36` writes straight into `Now_Private->inventory`,
  which the authoritative projection then reverts — the same defect class as the
  shop.

## Scope boundaries

- The source tree contains no Nintendo assets. Graphical smoke tests require a
  legitimate USA Rev 0 disc image supplied outside the repository.
- Four permanent residents intentionally preserve the original GCI slots;
  extra connections are visitors stored in server metadata.
- One server process owns one town. Running additional towns means running
  additional processes with distinct data directories and UDP ports.
- Discord Rich Presence is Windows-only. Other platforms build no-op stubs;
  the wording layer (`pc/src/pc_discord_text.c`) is portable, so a Unix-socket
  IPC backend would not need to duplicate it.

## Release gate

Run `make check`, `make sanitize`, the Windows build/server smoke, and
`scripts/smoke_windows.ps1` with a legitimate disc before publishing. Package
with `package_release.bat -Version VERSION`; never add the disc, GCI, invitation
key, or live town directory to the release archive.
