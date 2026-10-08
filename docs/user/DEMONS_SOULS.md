# Demon's Souls (PPSA01341) on AnyPS5

This archive runs Demon's Souls (PS5, 2020) on a PC through AnyPS5. **It contains no game code, no
Sony firmware and no shader cache.** Everything game-related comes from your own dump: on first
start the launcher converts your `eboot.bin` into a PC executable on your machine.

The port is a work in progress. Read [Known issues](#known-issues) before you start.

## Requirements

- **Your own, legally dumped and decrypted copy of the game**, PPSA01341 (the `app0` folder of the
  installed game: `eboot.bin`, `sce_module/`, `sce_sys/` and the data folders, about 82 GB).
  Supported game version: `gameversion.txt` says `BuildVersion=2025-10-15.877562`. Other versions are
  refused by the patch step. Encrypted files (SELF) do not work.
- **OS:** Windows 10/11 x64, or x86-64 Linux. Linux has not been tested yet.
- **GPU:** Vulkan 1.1 or newer with `shaderInt64`; `VK_EXT_external_memory_host` is used when the
  driver offers it. So far only tested on an NVIDIA RTX 5080 with a current driver.
- **Memory:** the game reserves and touches about 60 GB. RAM plus page file (Windows) or RAM plus swap
  (Linux) must be at least **64 GB**. 32 GB of RAM or more is recommended. On Windows set the page file
  to "System managed" on a drive with enough free space, or set a custom size of at least
  64 GB minus your RAM (System > About > Advanced system settings > Performance > Settings >
  Advanced > Virtual memory). The launcher warns when the total is lower.
- **Disk:** about 1 GB for AnyPS5 plus the shader cache, which grows to several GB. The dump is linked,
  not copied.
- **Python 3.8 or newer** for the one-time patch step (standard library only). Windows:
  [python.org](https://www.python.org/downloads/) or `winget install Python.Python.3.12`. Without Python the
  game still starts, but much slower.

## Install

1. Unpack the archive into its own folder, for example `C:\Games\AnyPS5-DemonsSouls`. Do not
   unpack it into the dump folder. On Windows, keep the folder and the dump on local NTFS drives,
   because the launcher links the dump with directory junctions.
2. First start, with the path to your dump:
   - Windows: open PowerShell in the folder and run
     `.\DemonsSouls.ps1 -Dump "D:\Games\PPSA01341-app0"`, or run
     `DemonsSouls.bat -Dump "D:\Games\PPSA01341-app0"` from a command prompt.
   - Linux: `./DemonsSouls.sh --dump /path/to/PPSA01341-app0`
   The launcher then
   1. runs `tools/relinker` on your `eboot.bin` and writes `DemonsSouls.exe` (Windows) or
      `DemonsSouls.elf` (Linux). The relinker also writes converted game modules into
      `app0/sce_module`,
   2. applies the port's executable patch with `tools/ds_patch.py` (it checks the game version and
      writes nothing if the version does not match),
   3. links every folder of your dump into `app0/`. Your dump is never modified,
   4. starts the game.
3. Later starts: `DemonsSouls.bat` / `.\DemonsSouls.ps1` on Windows, `./DemonsSouls.sh` on Linux.
   Run with `-Dump` / `--dump` again after updating AnyPS5.

Options: `-SetupOnly` / `--setup-only` prepares without starting; `-NoPatch` / `--no-patch` skips the
patch (slow; for troubleshooting only). On an Intel CPU, `-ToIntel` / `--to-intel` (together with
`-Dump` / `--dump`) relinks with the relinker's `--to-intel` conversion of AMD-only instructions;
it is untested with this game, so use it only if the game crashes with an illegal instruction.

The first launch, and every new area, compiles shaders into `shader_cache/`. This makes the first
minutes stutter heavily. Saves are stored in `_sd/` in the install folder; back them up from there.

## Settings

The launcher sets these unless you already set them yourself:

| Variable | Value | Meaning |
|---|---|---|
| `APS5_HOST_IMPORT_MIB` | 16384 with 48 GB RAM or more, 8192 with 24 GB or more, else 4096 | guest memory the GPU reads in place. Lower it if you run out of memory. |
| `APS5_BINDLESS_SLOTS` | 48 | textures one material table may reference (the maximum) |

Other user settings: `ANYPS5_INPUT_CONFIG` (input file path), `ANYPS5_SHADER_CACHE_DIR` (move the shader
cache), `ANYPS5_NO_SHADER_CACHE=1` (no disk cache).

## Controls

Controllers work through SDL: Xbox layout A/B/X/Y = Cross/Circle/Square/Triangle, LB/RB = L1/R1,
LT/RT = L2/R2, Start = Options. PlayStation controllers use their own layout.

Built-in keyboard and mouse bindings:

| PS5 | Key |
|---|---|
| Cross | Enter, Space |
| Circle | C |
| Triangle | I |
| Square | left mouse button |
| L1 / R1 | Q / E, Alt |
| R2 | right mouse button |
| L3 / R3 | Shift / Ctrl |
| Options | Esc |
| D-pad | arrow keys, mouse wheel up/down |
| Left stick | W A S D |
| Right stick | T F G H |
| Touchpad left / right | Backspace / Tab |
| Toggle mouse capture | middle mouse button |
| Fullscreen | F11 |

To change keys, rename `anyps5-input.ini.example` to `anyps5-input.ini` and edit it. All actions are
listed in `INPUT_MAPPING.md`.

## Known issues

- **Speed:** about 4 FPS in Boletaria 1-1 on an RTX 5080. The game is not yet playable at normal speed.
- **Graphics:** bright colour flashes, pixelated blocks and a blocky floor after loading a save. Some
  draws are still dropped.
- **Crashes while loading:** about 1 in 4 launches dies while loading. Start the game again.
- **Tested so far:** boot, menus, character creation, saving and loading in Boletaria 1-1. Combat,
  bosses and the other worlds are untested. Audio works but has not been checked closely.
- **Memory:** with too little page file or swap the game crashes with out-of-memory errors (see
  Requirements).
- **Linux:** never run so far. Expect problems and report them.
- **Shader compiles:** heavy stutter the first time an area or effect is seen.

## Legal

AnyPS5 is free software under the GNU GPL version 2 (`LICENSE`). Third-party licenses are in
`licenses/`. This release contains no code or data from Sony Interactive Entertainment, Bluepoint
Games or FromSoftware. You need your own copy of the game, dumped from a console you own.
Demon's Souls is a trademark of its owners. This project is not affiliated with or endorsed by them.
