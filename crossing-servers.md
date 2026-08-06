# Crossing Servers

This fork includes a persistent Animal Crossing town server. The original game
client owns movement/collision and sends bounded transforms; the server commits
all persistent and contested state, and clients send semantic requests rather
than persistent outcomes.

## Start a Windows town

Build with `build_pc.bat`, then run from `pc\build64\bin`:

```bat
AnimalCrossingServer.exe --config server.ini
```

The host edits `server.ini`. Each client puts their legitimate USA Rev 0 disc
image in `rom\`, edits `network.ini`, and then launches normally:

```bat
AnimalCrossing.exe
```

Use a different stable nonzero account ID for every player. The first four
resident accounts map to the original save slots; later accounts are visitors.
The server relays movement and owns inventory/economy, foreground changes,
tools/catches, NPC leases, zones/houses, clock/weather, and persistence.

## Operate and verify

- Configure the host in `server.ini`; clients point to it with `network.ini`.
  Command-line values win when a temporary override is useful.
- Stop with Ctrl+C for an orderly checkpoint and back up the whole town folder.
- Use `--checkpoint-now`, `--ban`, `--unban`, `--import-gci`, or `--export-gci`
  as one-shot commands while the main process is stopped.
- Run `make check` and `make sanitize` for the automated suite.
- Build an asset-free release with `package_release.bat -Version VERSION`.

Never distribute a disc image, save, live server database, journal, invitation
key, or extracted Nintendo asset.
