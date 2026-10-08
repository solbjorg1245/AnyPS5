#!/usr/bin/env bash
# Demon's Souls (PPSA01341) launcher for AnyPS5 on Linux.
#
# First start: ./DemonsSouls.sh --dump /path/to/PPSA01341-app0
#   1. runs tools/relinker on <dump>/eboot.bin and writes DemonsSouls.elf here (the relinker also
#      writes the converted game modules into app0/sce_module),
#   2. patches DemonsSouls.elf with tools/ds_patch.py (python3; refuses an unsupported game version;
#      skipped with a warning when python3 is missing),
#   3. symlinks every folder of the dump into app0/ (nothing in the dump is copied or modified; the
#      few small top-level files are copied),
#   4. starts the game.
# Later starts: ./DemonsSouls.sh   (extra arguments after -- go to the game)
# Options: --dump <dir>, --setup-only, --no-patch
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

while [ $# -gt 0 ]; do
    case "$1" in
        --dump) [ $# -ge 2 ] || { echo "--dump needs a directory" >&2; exit 1; }; dump="$2"; shift 2 ;;
        --setup-only) setup_only=1; shift ;;
        --no-patch) no_patch=1; shift ;;
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

    echo "== relinking $eboot"
    mkdir -p "$app0"
    "$here/tools/relinker" "$eboot" "$exe"
    chmod +x "$exe"

    if [ "$no_patch" = 1 ]; then
        echo "warning: skipping the executable patch (--no-patch): job workers will spin and starve the game threads" >&2
    elif command -v python3 >/dev/null 2>&1; then
        echo "== patching DemonsSouls.elf"
        if ! python3 -I "$here/tools/ds_patch.py" "$eboot" "$exe"; then
            rm -f "$exe"
            echo "ds_patch.py refused this dump; DemonsSouls.elf was removed" >&2
            exit 1
        fi
    else
        echo "warning: python3 not found: DemonsSouls.elf is NOT patched and will run much slower" >&2
    fi

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
    echo "DemonsSouls.elf is not set up yet. Run once with your own decrypted dump:"
    echo "  ./DemonsSouls.sh --dump /path/to/PPSA01341-app0"
    exit 1
fi
[ "$setup_only" = 0 ] || exit 0

ram_kib="$(awk '/^MemTotal:/ { print $2 }' /proc/meminfo)"
swap_kib="$(awk '/^SwapTotal:/ { print $2 }' /proc/meminfo)"
ram_gib=$(( ram_kib / 1048576 ))
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
