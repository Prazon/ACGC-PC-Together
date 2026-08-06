# Animal Crossing PC Port

A native PC port of Animal Crossing (GameCube) built on top of the [ac-decomp](https://github.com/ACreTeam/ac-decomp) decompilation project.

The game's original C code runs natively on PC, with a custom translation layer replacing the GameCube's GX graphics API with OpenGL 3.3.

This repository does not contain any game assets or assembly whatsoever. An existing copy of the game is required.

Supported versions: GAFE01_00: Rev 0 (USA)

## Quick Start (Pre-built Release)

Pre-built releases are available on the [Releases](https://github.com/flyngmt/ACGC-PC-Port/releases) page. No build tools required.

1. Download and extract the latest release zip
2. Place your disc image in the `rom/` folder
3. Run `AnimalCrossing.exe`

The game reads all assets directly from the disc image at startup. No extraction or preprocessing step is needed.

## Building from Source

Only needed if you want to modify the code. Otherwise, use the [pre-built release](https://github.com/flyngmt/ACGC-PC-Port/releases) above.

### Requirements

- **Animal Crossing (USA) disc image** (ISO, GCM, or CISO format)
- **CMake** 3.16+
- **SDL2** development libraries
- **64-bit GCC** (the only supported ABI)
- **Ninja**

### Build Steps

### Windows x86-64 (MSYS2)

1. Install **MSYS2** (https://www.msys2.org/)

2. Install the native x86-64 dependencies:
   ```bash
   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-SDL2 mingw-w64-x86_64-sqlite3
   ```

3. Clone and build:
   ```bash
   git clone https://github.com/flyngmt/ACGC-PC-Port.git
   cd ACGC-PC-Port
   build_pc.bat
   ```

4. Place your disc image in `pc/build64/bin/rom/` and run:
   ```bash
   pc\build64\bin\AnimalCrossing.exe
   ```

### macOS (Apple Silicon & Intel)

1. Install dependencies:
   ```bash
   brew install gcc sdl2 cmake ninja
   ```

2. Clone and build:
   ```bash
   git clone https://github.com/flyngmt/ACGC-PC-Port.git
   cd ACGC-PC-Port
   cmake -S pc -B pc/build64 -G Ninja -DCMAKE_C_COMPILER=gcc-15 -DCMAKE_CXX_COMPILER=g++-15
   cmake --build pc/build64 --parallel
   ```
   > Adjust `gcc-15`/`g++-15` to match your installed GCC version (`ls /opt/homebrew/bin/gcc-*`).

3. Place your disc image in `pc/build64/bin/rom/` and run:
   ```bash
   pc/build64/bin/AnimalCrossing
   ```

### Linux (x86_64 / ARM64)

1. Install dependencies:
   ```bash
   # Arch/CachyOS/Manjaro
   sudo pacman -S gcc cmake ninja sdl2 sqlite

   # Debian/Ubuntu
   sudo apt install gcc g++ cmake ninja-build libsdl2-dev libsqlite3-0

   # Fedora
   sudo dnf install gcc gcc-c++ cmake ninja-build SDL2-devel sqlite-libs
   ```

2. Clone and build:
   ```bash
   git clone https://github.com/flyngmt/ACGC-PC-Port.git
   cd ACGC-PC-Port
   cmake -S pc -B pc/build64 -G Ninja -DNETCODE_ENABLED=ON
   cmake --build pc/build64 --parallel
   ```

3. Place your disc image in `build/bin/rom/` and run:
   ```bash
   pc/build64/bin/AnimalCrossing
   ```

### Disc Image

The game reads all assets directly from the disc image at startup. No extraction or preprocessing step is needed. Place your disc image (`.iso`, `.gcm`, or `.ciso`) in the `rom/` folder next to the executable — the file can be named anything. The game also checks the `orig/` folder and the current directory.

## Controls

Keyboard bindings are customizable via `keybindings.ini` (next to the executable). Mouse buttons (Mouse1/Mouse2/Mouse3) can also be assigned.

### Keyboard (defaults)

| Key | Action |
|-----|--------|
| WASD | Move (left stick) |
| Arrow Keys | Camera (C-stick) |
| Space | A button |
| Left Shift | B button |
| Enter | Start |
| X | X button |
| Y | Y button |
| Q / E | L / R triggers |
| Z | Z trigger |
| I / J / K / L | D-pad (up/left/down/right) |

### Gamepad

SDL2 game controllers are supported with automatic hotplug detection. Button mapping follows the standard GameCube layout.

## Command Line Options

| Flag | Description |
|------|-------------|
| `--verbose` | Enable diagnostic logging |
| `--no-framelimit` | Disable frame limiter (unlocked FPS) |
| `--model-viewer [index]` | Launch debug model viewer (structures, NPCs, fish) |
| `--time HOUR` | Override in-game hour (0-23) |
| `--online HOST:PORT` | Connect to a dedicated town |
| `--town ID` | Select the nonzero dedicated-town ID |
| `--account ID` | Use a stable nonzero account ID |
| `--invite-key KEY` | Authenticate to an invitation-only town |

## Dedicated towns

The build also produces `AnimalCrossingServer`. The original game client owns
movement and collision, while the server relays bounded transforms and remains
authoritative for inventories, ground state, tools/catches, shops, trades, NPC
leases, zones, housing, time/weather, mail, museum state, and persistence.
The first online resident establishes the server's deterministic town; later
players see it as an existing town. Online character creation keeps player
name/gender, gives a randomized face/distinct starter shirt and assigned house, and skips
local town naming, clock correction, and Tom Nook's initial job sequence.

Run `make check` for the automated host suite, or see
[`crossing-servers.md`](crossing-servers.md) for server operation, backups,
online client arguments, and release packaging.

## Settings

Graphics settings are stored in `settings.ini` (next to the executable) and can be edited manually or through the in-game options menu:

- Resolution (up to 4K)
- Fullscreen toggle
- VSync
- MSAA (anti-aliasing)
- Texture Loading/Caching (No need to enable if you aren't using a texture pack)

## Discord Rich Presence

Optional, Windows builds only for now. To show your current town and location (e.g. "In the town of Foo" / "Inside Nook's Cranny") on your Discord profile, create a free Application at the [Discord Developer Portal](https://discord.com/developers/applications) and paste its Client ID into `discord_client_id` in `settings.ini`. Leave it blank to disable (default).

On a dedicated town it reads "Online in the town of Foo", plus how many other players are nearby. Nothing that identifies the server, your account, or an invitation key is ever sent to Discord.

## Texture Packs

Custom textures can be placed in the `texture_pack/` folder next to the executable. Dolphin-compatible format (XXHash64, DDS).

I highly recommend the following texture pack from the talented artists of Animal Crossing community.

[HD Texture Pack](https://forums.dolphin-emu.org/Thread-animal-crossing-hd-texture-pack-version-23-feb-22nd-2026)

## Save Data

Save files are stored in the `save/` folder next to the executable, using the standard GCI format. Compatible with Dolphin emulator saves — place a Dolphin GCI export in the save directory to import an existing save.

## Credits

This project would not be possible without the work of the [ACreTeam](https://github.com/ACreTeam) decompilation team. Their complete C decompilation of Animal Crossing is the foundation this port is built on.

## AI Notice

AI tools such as Claude were used in this project (PC port code only).

## FAQ

See [FAQ](FAQ.md) for more info.
