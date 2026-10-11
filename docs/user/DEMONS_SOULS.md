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
   because the setup links the dump with directory junctions.
2. Start **`anyps5-launcher.exe`** in that folder (Linux: `./anyps5-launcher`; on a fresh install
   `DemonsSouls.bat` and `DemonsSouls.sh` open it too). In **Game folder**, type or paste the path of
   your dump's `app0` folder, or press **Browse...** (the system's folder picker; on a Steam Deck the
   window's own browser: D-pad moves, A opens a folder, B goes up, Y takes the folder shown). The
   window checks the folder at once and says what is wrong: `eboot.bin`, `sce_sys` and `sce_module`
   must be there, `eboot.bin` must be decrypted (an encrypted SELF does not work), the folder must be on
   a local drive, and it names the game version (a warning for an unknown one). Choosing the folder that
   contains `app0` works too. **Play** stays disabled until the folder is valid.
3. Press **Play**. The first time, and after you choose another folder, update AnyPS5 or install
   Python, the window runs the one-time setup and shows its progress (it calls `DemonsSouls.ps1 -Dump
   <folder> -SetupOnly` / `DemonsSouls.sh --dump <folder> --setup-only`, hidden):
   1. `tools/relinker` turns your `eboot.bin` into `DemonsSouls.exe` (Windows) or `DemonsSouls.elf`
      (Linux) and writes converted game modules into `app0/sce_module`,
   2. `tools/ds_patch.py` applies the port's executable patch (it checks the game version and writes
      nothing if the version does not match; skipped with a warning when Python is missing),
   3. every folder of your dump is linked into `app0/`. Your dump is never modified.

   Then the game starts. The setup is recorded in `anyps5-setup.txt` (delete it to run the setup again).
   Later, **Play** starts the game at once.

### Steam

Add `anyps5-launcher.exe` (Linux and Steam Deck: `anyps5-launcher`) as a non-Steam game. Without launch
options the settings window opens on every start. With the launch options **`%command% --launch`** the
game starts straight away once the game folder is set up (otherwise the window opens). On a Steam Deck,
unpack the archive and add the launcher in Desktop Mode, then play from Game Mode.

### Without the window

`.\DemonsSouls.ps1 -Dump "D:\Games\PPSA01341-app0"` (PowerShell), `DemonsSouls.bat -Dump
"D:\Games\PPSA01341-app0"` (command prompt) or `./DemonsSouls.sh --dump /path/to/PPSA01341-app0` (Linux)
runs the same setup in a console and starts the game. Later starts: `DemonsSouls.bat` /
`.\DemonsSouls.ps1` / `./DemonsSouls.sh`; run with `-Dump` / `--dump` again after updating AnyPS5.

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

### Settings window

`anyps5-launcher` is the settings window: game folder (see [Install](#install)), resolution (the
presets, your desktop's size or auto), window mode, FPS limit, timestep and native aspect. Use the mouse,
the keyboard (arrow keys, Enter) or a controller (D-pad, A; Start plays). **Play** saves
`anyps5-settings.ini`, runs the setup when it is due and starts the game; **Quit** or closing the window
exits. Your comments and other keys in the file are kept. Upscaler and frame generation are shown greyed
out: they come later.

Starting `DemonsSouls.exe` itself (or `DemonsSouls.bat` / `.sh` after the setup) opens the same window
first; there the game folder is shown but not changed. Add `--launch` to start straight away with the
saved settings:
- Steam (a non-Steam game pointing at `anyps5-launcher.exe`, `DemonsSouls.exe`, `DemonsSouls.bat` or
  `DemonsSouls.sh`): launch options `%command% --launch`
- Windows: `DemonsSouls.bat --launch` or `.\DemonsSouls.ps1 --launch`; Linux: `./DemonsSouls.sh --launch`

`APS5_NO_LAUNCHER=1` skips the window when the game starts on its own. If `anyps5-launcher.exe` is
missing (next to the game or in `libs/`) or cannot open, the game starts with the file as it is.

### Settings file

Display settings live in `anyps5-settings.ini` next to the game: the settings window writes it, or rename
`anyps5-settings.ini.example` and edit it. The game's own performance and cinematic modes are replaced
by these keys:

| Key | Variable | Values |
|---|---|---|
| `Resolution` | `APS5_RESOLUTION` | `auto` or `WIDTHxHEIGHT`, e.g. `1280x800` (Steam Deck), `1920x1080`, `2560x1440`; sets the window size and the game's render resolution |
| `FpsLimit` | `APS5_FPS_LIMIT` | `auto`, `30`, `60`; other values (20 to 240) need `Timestep = variable` |
| `Timestep` | `APS5_TIMESTEP` | `fixed` (the game's own: slow motion below the limit) or `variable` (experimental: the game keeps its speed at any frame rate) |
| `WindowMode` | `APS5_WINDOW_MODE` | `windowed` or `fullscreen` |
| `NativeAspect` | `APS5_NATIVE_ASPECT` | `off` or `on` (experimental: renders at `Resolution`'s own aspect, e.g. 21:9, instead of letterboxed 16:9) |

The log shows the result in its `[settings]` lines.

A `Resolution` up to 1080p (or `RenderResolution` 900 or 1080) renders its tier's size (1600x900 at
1280x720 or 1280x800, 1920x1080 at 1920x1080) inside the game's WantsHigh tier, so the HUD and menus are
laid out on its 2160p display buffer and scaled down to the window: the game's 1080p tiers draw the HUD's
item diamonds and the equipment menu's slot frames too large. `APS5_NO_RESOLUTION_4K_UI=1` brings back
the 1080p tier.

`NativeAspect = on` widens the camera and presents the 16:9 render targets at the window's shape.
The HUD, menus and movies stay 16:9 in a centred region; beside them the 3D shows (black bars beside
a movie). `APS5_NO_NATIVE_ASPECT_UI_RECT=1` stretches the UI across the window with the 3D instead,
and `APS5_NATIVE_ASPECT_UI_EDGE_STRETCH=1` extends the UI's outermost columns over the side margins
(menu dims reach the window's edges, but movies smear into streaks there).
The UI is drawn on the title's 2160p display buffer at every `Resolution` (a smaller window, e.g.
1280x800, renders its own size inside the WantsHigh tier): the title's 1080p tiers draw the HUD's item
diamonds and the equipment menu's slot frames too large. `APS5_NO_NATIVE_ASPECT_4K_UI=1` brings back the
smallest tier.
Two environment-only switches go further and are experimental: `APS5_NATIVE_ASPECT_RENDER=1` sizes
the shared render targets at `Resolution` itself (the post effects break: bloom blobs and ghosting),
and `APS5_NATIVE_ASPECT_UI=1` gives the display buffer the window's shape (the bottom HUD is cut off
when the window is wider than 16:9).

Troubleshooting: GPU-to-guest copy-backs are coalesced and narrowed (only changed dwords are written
back) by default. `APS5_NO_COALESCE_COPY_BACKS=1` and `APS5_NO_NARROW_COPY_BACKS=1` turn these off
if you see stale or flickering geometry.

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
