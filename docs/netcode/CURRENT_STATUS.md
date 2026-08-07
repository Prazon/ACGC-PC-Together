# Dedicated Town Netcode Status

Last updated: 2026-08-06

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

- Explicit protocol v15 codecs, selective reliability, sequencing,
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

## Compatibility note for the protocol version

Protocol v15 (the item-drop presentation work above) is not backwards compatible
with an older client: negotiation is strict, so client and server must be
updated together. Town state is unchanged at v8 — `TileState` on disk did not
change, only the wire delta grew, so a v8 town directory needs no migration for
it. The invitation-key and dragged-furniture changes above carry no wire or
state version of their own.

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

Finish the mail UI surface, then post letters player to player.

1. **Carried-letter handling.** Rearranging letters and storing them on a card
   still go through the generic drag hand, which is refused online. That needs
   either a server-side letter reorder operation or a rule that the carried
   order is presentation-only.
2. **Posting a letter.** The post office addresses a recipient by name and the
   server by account, so a name-to-account lookup has to be exposed before
   `AttachMail` can be reached from the counter. The letter-writing UI then
   fills the content fields that already exist on the wire.
3. **Verify against a real client.** None of the mail or banking UI work has
   been run in the graphical client -- this machine has no SDL2. The ABD, the
   loan counter, the mailbox flag, reading a letter, and taking a present out
   all need a pass with `scripts/smoke_online_windows.ps1` and a disc.

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
