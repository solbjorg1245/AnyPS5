#!/usr/bin/env bash
# Demon's Souls (PPSA01341) launcher for AnyPS5 on Linux.
#
# First start: ./DemonsSouls.sh --dump /path/to/PPSA01341-app0
#   1. runs tools/relinker on <dump>/eboot.bin and writes DemonsSouls.elf here (the relinker also
#      writes the converted game modules into app0/sce_module),
#   2. patches DemonsSouls.elf with tools/ds_patch.py (python3 3.8+; an unsupported game version is
#      refused before anything is relinked; skipped with a warning when python3 is missing),
#   3. symlinks every folder of the dump into app0/ (nothing in the dump is copied or modified; the
#      few small top-level files are copied),
#   4. starts the game.
# Steps 1 and 2 run in a staging folder (setup.tmp): a failed setup leaves an earlier one as it was.
# Later starts: ./DemonsSouls.sh   (extra arguments after -- go to the game; ./DemonsSouls.sh --launch
#   skips the settings window, also as Steam launch options: %command% --launch)
# Options: --dump <dir>, --setup-only, --no-patch, --to-intel (relink for an Intel CPU; untested)
# The settings window (anyps5-launcher) runs this setup itself (--dump <its game folder> --setup-only) when
# the game folder is chosen there; without DemonsSouls.elf and without --dump this script opens that window.
#
# Runtime settings (an already exported variable always wins):
#   APS5_HOST_IMPORT_MIB  guest memory the GPU may read in place; chosen from installed RAM
#   APS5_BINDLESS_SLOTS   48 (textures one bindless material table may reference)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exe="$here/DemonsSouls.elf"
app0="$here/app0"
supported_build="2025-10-15.877562"
dump=""
setup_only=0
no_patch=0
to_intel=0

while [ $# -gt 0 ]; do
    case "$1" in
        --dump) [ $# -ge 2 ] || { echo "--dump needs a directory" >&2; exit 1; }; dump="$2"; shift 2 ;;
        --setup-only) setup_only=1; shift ;;
        --no-patch) no_patch=1; shift ;;
        --to-intel) to_intel=1; shift ;;
        --) shift; break ;;
        *) break ;;
    esac
done

install_game() {
    local dump_dir eboot version item name target
    dump_dir="$(cd "$1" && pwd)"
    eboot="$dump_dir/eboot.bin"
    for required in eboot.bin sce_module sce_sys; do
        [ -e "$dump_dir/$required" ] || { echo "$dump_dir does not look like a PPSA01341 app0 dump: $required is missing" >&2; exit 1; }
    done
    [ "$dump_dir" != "$here" ] || { echo "Install AnyPS5 into its own folder, not into the dump" >&2; exit 1; }
    if [ -f "$dump_dir/gameversion.txt" ]; then
        version="$(sed -n 's/^+\{0,1\}BuildVersion=\([^[:space:]]*\).*/\1/p' "$dump_dir/gameversion.txt" | head -n 1)"
        [ "$version" = "$supported_build" ] || echo "warning: dump BuildVersion is '$version'; this release supports $supported_build" >&2
    fi

    python=""
    if [ "$no_patch" = 0 ] && command -v python3 >/dev/null 2>&1 && python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' 2>/dev/null; then
        python=python3
        # The version check first: an unsupported dump is refused before anything is relinked or replaced.
        "$python" -I "$here/tools/ds_patch.py" --check "$eboot" || { echo "ds_patch.py refused this dump; nothing was changed" >&2; exit 1; }
    fi

    # Relinked and patched in a staging folder, then swapped in: a failed setup leaves an earlier one working.
    stage="$here/setup.tmp"
    rm -rf "$stage"
    mkdir -p "$stage"
    trap 'rm -rf "$stage"' EXIT  # also when the relinker or the patch fails
    staged_exe="$stage/DemonsSouls.elf"

    echo "== relinking $eboot"
    relink_args=()
    if [ "$to_intel" = 1 ]; then
        relink_args+=(--to-intel)
    elif grep -q -m 1 '^vendor_id[[:space:]]*:[[:space:]]*GenuineIntel' /proc/cpuinfo 2>/dev/null; then
        echo "warning: Intel CPU: the game uses AMD-only instructions; if it dies with SIGILL, run again with --dump ... --to-intel" >&2
    fi
    "$here/tools/relinker" ${relink_args[@]+"${relink_args[@]}"} "$eboot" "$staged_exe"

    if [ "$no_patch" = 1 ]; then
        echo "warning: skipping the executable patch (--no-patch): job workers will spin and starve the game threads" >&2
    elif [ -n "$python" ]; then
        echo "== patching DemonsSouls.elf"
        "$python" -I "$here/tools/ds_patch.py" "$eboot" "$staged_exe" || { echo "ds_patch.py refused this dump; an earlier setup is unchanged" >&2; exit 1; }
    else
        echo "warning: python3 3.8+ not found: DemonsSouls.elf is NOT patched and will run much slower" >&2
    fi
    chmod +x "$staged_exe"

    # The new executable, then the modules (a link goes as a link, never its target).
    mkdir -p "$app0"
    mv -f "$staged_exe" "$exe"
    if [ -d "$stage/app0/sce_module" ]; then
        if [ -L "$app0/sce_module" ]; then rm -f "$app0/sce_module"; else rm -rf "$app0/sce_module"; fi
        mv "$stage/app0/sce_module" "$app0/sce_module"
    fi
    rm -rf "$stage"
    trap - EXIT

    echo "== linking the dump into app0"
    for item in "$dump_dir"/*; do
        [ -e "$item" ] || continue
        name="$(basename "$item")"
        # sce_module: the relinker writes the converted modules there. logs: the game writes there.
        case "$name" in eboot.bin|sce_module) continue ;; logs) mkdir -p "$app0/logs"; continue ;; esac
        target="$app0/$name"
        if [ -L "$target" ]; then
            rm -f "$target"
        elif [ -d "$item" ] && [ -e "$target" ]; then
            continue
        fi
        if [ -d "$item" ]; then
            ln -s "$item" "$target"
        else
            cp -f "$item" "$target"
        fi
    done
    echo "== setup done"
}

if [ -n "$dump" ]; then
    install_game "$dump"
elif [ ! -x "$exe" ]; then
    # A fresh install: the settings window asks for the dump, sets up and starts the game.
    [ ! -x "$here/anyps5-launcher" ] || exec "$here/anyps5-launcher" "$@"
    echo "DemonsSouls.elf is not set up yet. Run once with your own decrypted dump:"
    echo "  ./DemonsSouls.sh --dump /path/to/PPSA01341-app0"
    exit 1
fi
[ "$setup_only" = 0 ] || exit 0

ram_kib="$(awk '/^MemTotal:/ { print $2 }' /proc/meminfo)"
swap_kib="$(awk '/^SwapTotal:/ { print $2 }' /proc/meminfo)"
ram_gib=$(( (ram_kib + 524288) / 1048576 ))  # rounded like the Windows launcher
if [ $(( (ram_kib + swap_kib) / 1048576 )) -lt 64 ]; then
    echo "warning: RAM + swap is below 64 GiB; the game reserves and touches about 60 GiB. Add swap if it dies with out-of-memory errors." >&2
fi
if [ -z "${APS5_HOST_IMPORT_MIB:-}" ]; then
    if [ "$ram_gib" -ge 48 ]; then export APS5_HOST_IMPORT_MIB=16384
    elif [ "$ram_gib" -ge 24 ]; then export APS5_HOST_IMPORT_MIB=8192
    else export APS5_HOST_IMPORT_MIB=4096
    fi
fi
export APS5_BINDLESS_SLOTS="${APS5_BINDLESS_SLOTS:-48}"

cd "$here"
exec "$exe" "$@"
