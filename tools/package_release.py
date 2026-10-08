"""Release packaging.

Default mode (used by .github/workflows/release.yml, unchanged output names):
    package_release.py --platform windows|linux --build <dir> --version <tag> --output <dir>
        -> prx-<platform>-<tag>.zip / .tar.gz (libs/*.prx) and relinker-<tag>[.exe]
    package_release.py --docs docs/user --output <dir>

Player archive for Demon's Souls (--game ds, in addition to the files above):
        -> AnyPS5-DemonsSouls-<tag>-<platform>-x64.zip (Windows) / .tar.gz (Linux)
    The archive holds only files built from this source (relinker, libs/*.prx), the MinGW runtime
    DLLs, launchers, the stdlib-only patch tool, docs and licenses. The player relinks their own
    dump on first start; no game code, firmware or shader cache is ever packaged.

Every file comes from an explicit allowlist and is then checked against a denylist (game files,
Sony firmware, shader caches, NVIDIA SDKs, development port folders); a hit aborts packaging.
--dry-run lists what would be packaged and runs every check without writing anything.
"""
import argparse
import os
import re
import shutil
import struct
import sys
import tarfile
import zipfile
from pathlib import Path

SOURCE_ROOT = Path(__file__).resolve().parent.parent
MINGW_RUNTIME = ("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll")
# libcohtml.prx carries the wrong DT_NEEDED name (core/libs/prx/libcohtml/DEPLOY.md); the game
# loads libcohtml.Prospero.prx, a renamed copy.
EXCLUDED_LIBRARIES = {"libcohtml.prx"}
EXTRA_LIBRARIES = {"libcohtml.Prospero.prx"}
PLAYER_ROOT = "AnyPS5-DemonsSouls"
PLAYER_FILES = Path("packaging/demons-souls")
LAUNCHER_NAMES = {"DemonsSouls.ps1", "DemonsSouls.bat", "DemonsSouls.sh"}
EXECUTABLE_NAMES = {"relinker", "DemonsSouls.sh", "ds_patch.py"}

# --- denylist -----------------------------------------------------------------------------------
# Path components that only ever hold game data, user data or development output.
DENY_COMPONENTS = {"app0", "sce_module", "sce_modules", "sce_sys", "unpatched", "implib", "shader_cache",
                   "_sd", "hostapp", "vsdump", "capture", "dlss", "streamline", "ngx"}
DENY_SUFFIXES = (".guest.prx", ".spv", ".req", ".sprx", ".self", ".elf", ".bin", ".pup", ".regs", ".pkg",
                 ".registry.json")
DENY_SUBSTRINGS = ("eboot", "nvngx", "dlss", "sl.interposer", "nvsdk", "psarc", "ppsa01341", "param.json",
                   "gameversion", "playgo-chunkdefs")
# Development trees on the maintainer's machine that hold game-derived files or NVIDIA SDKs.
DENY_SOURCE_PATTERNS = (re.compile(r"[\\/]dsd[\\/]port([\\/]|$)", re.I),
                        re.compile(r"[\\/]demons souls decomp[\\/]port([\\/]|$)", re.I),
                        re.compile(r"[\\/]tools[\\/]dlss([\\/]|$)", re.I),
                        re.compile(r"gamedumps", re.I))
SCE_ELF_TYPES = {0xFE00, 0xFE04, 0xFE0C, 0xFE10, 0xFE18}
SELF_MAGICS = (b"\x4f\x15\x3d\x1d", b"\x54\x14\xf5\xee")

# DLLs a Windows .prx may import besides the bundled MinGW runtime and other bundled .prx.
WINDOWS_SYSTEM_DLLS = {
    "kernel32.dll", "ntdll.dll", "ucrtbase.dll", "msvcrt.dll", "user32.dll", "gdi32.dll", "advapi32.dll",
    "shell32.dll", "ole32.dll", "oleaut32.dll", "ws2_32.dll", "winmm.dll", "imm32.dll", "version.dll",
    "setupapi.dll", "shlwapi.dll", "bcrypt.dll", "dbghelp.dll", "iphlpapi.dll", "cfgmgr32.dll", "hid.dll",
    "secur32.dll", "crypt32.dll", "dwmapi.dll", "uxtheme.dll", "psapi.dll", "powrprof.dll", "userenv.dll",
    "mswsock.dll", "synchronization.dll", "kernelbase.dll", "comdlg32.dll", "dxgi.dll", "d3d11.dll",
    "xinput1_4.dll", "dinput8.dll", "mfplat.dll", "avrt.dll", "ncrypt.dll", "winhttp.dll", "rpcrt4.dll", "comctl32.dll",
}


class PackagingError(RuntimeError):
    pass


def deny_reason(arcname, source):
    """Why `source` must not be packaged as `arcname`, or None."""
    name = Path(arcname).name
    lower = name.lower()
    parts = {p.lower() for p in Path(arcname).parts} | {p.lower() for p in Path(source).parts}
    hit = parts & DENY_COMPONENTS
    if hit:
        return f"path component {sorted(hit)[0]!r}"
    if lower in EXCLUDED_LIBRARIES:
        return "excluded library"
    for suffix in DENY_SUFFIXES:
        if lower.endswith(suffix):
            return f"suffix {suffix!r}"
    for text in DENY_SUBSTRINGS:
        if text in lower:
            return f"name contains {text!r}"
    if lower.startswith("demonssouls"):
        if name not in LAUNCHER_NAMES or Path(source).resolve().parent != (SOURCE_ROOT / PLAYER_FILES).resolve():
            return "game executable name"
    for pattern in DENY_SOURCE_PATTERNS:
        if pattern.search(str(Path(source).resolve())):
            return f"source path matches {pattern.pattern!r}"
    with open(source, "rb") as f:
        head = f.read(0x14)
    if head[:4] in SELF_MAGICS:
        return "Sony SELF image"
    if head[:4] == b"\x7fELF" and len(head) >= 0x12 and struct.unpack_from("<H", head, 0x10)[0] in SCE_ELF_TYPES:
        return "Sony SCE ELF image"
    return None


def check_roots(entries, roots):
    resolved = [r.resolve() for r in roots]
    for arcname, source in entries:
        path = Path(source).resolve()
        if not any(path == r or r in path.parents for r in resolved):
            raise PackagingError(f"{arcname}: source {source} is outside the allowed roots")


def check_denylist(entries):
    problems = []
    for arcname, source in entries:
        reason = deny_reason(arcname, source)
        if reason is not None:
            problems.append(f"{arcname} ({source}): {reason}")
    if problems:
        raise PackagingError("denylisted files would be packaged:\n  " + "\n  ".join(problems))


# --- binary checks ------------------------------------------------------------------------------
def pe_imports(path):
    data = Path(path).read_bytes()
    if data[:2] != b"MZ":
        raise PackagingError(f"{path}: not a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise PackagingError(f"{path}: not a PE file")
    nsections, = struct.unpack_from("<H", data, pe + 6)
    opt_size, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x20B:
        raise PackagingError(f"{path}: not a PE32+ (x64) file")
    sections = [struct.unpack_from("<IIII", data, opt + opt_size + 40 * i + 8) for i in range(nsections)]

    def offset(rva):
        for vsize, va, rawsize, rawptr in sections:
            if va <= rva < va + max(vsize, rawsize):
                return rva - va + rawptr
        raise PackagingError(f"{path}: RVA {rva:#x} outside sections")

    def cstr(rva):
        o = offset(rva)
        return data[o:data.index(b"\0", o)].decode("ascii", "replace")

    names = []
    ndirs, = struct.unpack_from("<I", data, opt + 108)
    for index, size, name_field in ((1, 20, 12), (13, 32, 4)):  # import and delay-import directories
        if index >= ndirs:
            continue
        rva, dsize = struct.unpack_from("<II", data, opt + 112 + 8 * index)
        if not rva or not dsize:
            continue
        o = offset(rva)
        while True:
            descriptor = data[o:o + size]
            if len(descriptor) < size or not any(descriptor):
                break
            names.append(cstr(struct.unpack_from("<I", descriptor, name_field)[0]))
            o += size
    return names


def elf_type(path):
    with open(path, "rb") as f:
        head = f.read(0x14)
    if head[:4] != b"\x7fELF":
        return None
    return struct.unpack_from("<H", head, 0x10)[0]


def check_binaries(platform, libraries, runtime_names, relinker=None):
    bundled = {name.lower() for name in runtime_names} | {lib.name.lower() for lib in libraries}
    problems = []
    for library in libraries:
        if platform == "windows":
            for dll in pe_imports(library):
                lower = dll.lower()
                if lower in bundled or lower in WINDOWS_SYSTEM_DLLS or lower.startswith("api-ms-win-"):
                    continue
                problems.append(f"{library.name} imports {dll}")
        elif elf_type(library) != 3:  # ET_DYN
            problems.append(f"{library.name} is not a host ELF shared object (e_type {elf_type(library)})")
    if platform == "windows" and relinker is not None:
        # The player runs tools/relinker.exe, which does not sit next to the bundled MinGW DLLs.
        for dll in pe_imports(relinker):
            lower = dll.lower()
            if lower not in WINDOWS_SYSTEM_DLLS and not lower.startswith("api-ms-win-"):
                problems.append(f"{relinker.name} imports {dll}")
    if problems:
        raise PackagingError("unexpected library dependencies:\n  " + "\n  ".join(problems))


# --- file selection -----------------------------------------------------------------------------
def select_libraries(build, source):
    present = {p.name: p for p in (build / "core/libs/libs").glob("*.prx") if p.is_file()}
    expected = {f"{d.name}.prx" for d in (source / "core/libs/prx").iterdir() if d.is_dir()}
    expected = (expected | EXTRA_LIBRARIES) - EXCLUDED_LIBRARIES
    missing = expected - set(present)
    if missing:
        raise PackagingError(f"Missing patched libraries: {', '.join(sorted(missing))}")
    return [present[name] for name in sorted(expected)], sorted(set(present) - expected)


def license_entries(build, source, root):
    entries = [(f"{root}LICENSE", source / "LICENSE")]
    for component in sorted(p for p in (source / "3rdparty").iterdir() if p.is_dir()):
        files = [p for p in component.iterdir() if p.is_file() and re.fullmatch(r"(LICENSE|COPYING|NOTICE)(\.[A-Za-z]+)?", p.name, re.I)]
        if (component / "LICENSES").is_dir():
            files += [p for p in (component / "LICENSES").iterdir() if p.is_file()]
        if component.name == "freetype":
            files += [component / "docs" / n for n in ("FTL.TXT", "GPLv2.TXT") if (component / "docs" / n).is_file()]
        for file in sorted(set(files)):
            entries.append((f"{root}licenses/{component.name}/{file.relative_to(component).as_posix()}", file))
    ffmpeg = sorted(build.glob("externals/ffmpeg-*/share/ffmpeg"))
    if not ffmpeg:
        raise PackagingError(f"ffmpeg license files not found under {build}/externals/ffmpeg-*/share/ffmpeg")
    for name in ("copyright", "SOURCE.txt"):
        entries.append((f"{root}licenses/ffmpeg/{name}", ffmpeg[-1] / name))
    return entries


def player_entries(platform, build, source, libraries, runtime, relinker):
    root = f"{PLAYER_ROOT}/"
    player = source / PLAYER_FILES
    entries = [
        (f"{root}README.md", source / "docs/user/DEMONS_SOULS.md"),
        (f"{root}INPUT_MAPPING.md", source / "docs/user/INPUT_MAPPING.md"),
        (f"{root}anyps5-input.ini.example", player / "anyps5-input.ini.example"),
        (f"{root}tools/ds_patch.py", source / "tools/ds_patch.py"),
        (f"{root}tools/{relinker.name}", relinker),
    ]
    if platform == "windows":
        entries += [(f"{root}DemonsSouls.ps1", player / "DemonsSouls.ps1"),
                    (f"{root}DemonsSouls.bat", player / "DemonsSouls.bat"),
                    (f"{root}licenses/mingw-runtime/NOTICE.txt", player / "mingw-runtime-NOTICE.txt")]
        entries += [(f"{root}{dll.name}", dll) for dll in runtime]
    else:
        entries += [(f"{root}DemonsSouls.sh", player / "DemonsSouls.sh")]
    entries += [(f"{root}libs/{lib.name}", lib) for lib in libraries]
    entries += license_entries(build, source, root)
    return entries


def write_zip(path, entries):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for arcname, file in entries:
            archive.write(file, arcname=arcname)


def write_tar(path, entries):
    def normalize(info):
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        info.mode = 0o755 if Path(info.name).name in EXECUTABLE_NAMES else 0o644
        return info

    with tarfile.open(path, "w:gz", compresslevel=9) as archive:
        for arcname, file in entries:
            archive.add(file, arcname=arcname, filter=normalize)


def package(platform, build, output, version, mingw_bin, game=None, dry_run=False, source=SOURCE_ROOT):
    if not re.fullmatch(r"v[0-9A-Za-z][0-9A-Za-z._-]*", version) or version.endswith("."):
        raise ValueError(f"Invalid release tag for asset filenames: {version}")
    libraries, skipped = select_libraries(build, source)
    runtime = [mingw_bin / name for name in MINGW_RUNTIME] if platform == "windows" else []
    relinker = build / "core/relinker" / ("relinker.exe" if platform == "windows" else "relinker")
    for file in [*libraries, *runtime, relinker]:
        if not file.is_file() or file.stat().st_size == 0:
            raise PackagingError(f"Missing or empty release file: {file}")
    check_binaries(platform, libraries, MINGW_RUNTIME if platform == "windows" else (), relinker)

    lib_entries = [(f"libs/{file.name}", file) for file in [*libraries, *runtime]]
    archives = {f"prx-{platform}-{version}.zip": ("zip", lib_entries),
                f"prx-{platform}-{version}.tar.gz": ("tar", lib_entries)}
    copies = {(f"relinker-{version}.exe" if platform == "windows" else f"relinker-{version}"): relinker}
    if game == "ds":
        entries = player_entries(platform, build, source, libraries, runtime, relinker)
        for _, file in entries:
            if not Path(file).is_file():
                raise PackagingError(f"Missing release file: {file}")
        names = [arcname for arcname, _ in entries]
        duplicates = sorted({n for n in names if names.count(n) > 1})
        if duplicates:
            raise PackagingError(f"Duplicate archive paths: {', '.join(duplicates)}")
        stem = f"AnyPS5-DemonsSouls-{version}-{platform}-x64"
        if platform == "windows":
            archives[f"{stem}.zip"] = ("zip", entries)
        else:
            archives[f"{stem}.tar.gz"] = ("tar", entries)

    roots = [source, build] + ([mingw_bin] if platform == "windows" else [])
    every = [entry for _, entries in archives.values() for entry in entries] + list(copies.items())
    check_roots(every, roots)
    check_denylist(every)

    if dry_run:
        for name, (_, entries) in archives.items():
            total = sum(Path(f).stat().st_size for _, f in entries)
            print(f"== {name}: {len(entries)} files, {total / 2**20:.1f} MiB uncompressed")
            for arcname, file in entries:
                print(f"  {arcname}  <-  {file}  ({Path(file).stat().st_size} B)")
        for name, file in copies.items():
            print(f"== {name}  <-  {file}")
        if skipped:
            print(f"-- not packaged (not on the allowlist): {', '.join(skipped)}")
        print("-- checks passed: allowlist, allowed roots, denylist, library dependencies")
        return

    output.mkdir(parents=True, exist_ok=True)
    for name, (kind, entries) in archives.items():
        (write_zip if kind == "zip" else write_tar)(output / name, entries)
    for name, file in copies.items():
        shutil.copy2(file, output / name)


def collect_docs(source, output):
    documents = sorted(source.rglob("*.md"))
    if not documents:
        raise RuntimeError(f"No Markdown documents found in {source}")
    names = set()
    for document in documents:
        if document.name in names or (output / document.name).exists():
            raise RuntimeError(f"Duplicate release asset name: {document.name}")
        names.add(document.name)
    output.mkdir(parents=True, exist_ok=True)
    for document in documents:
        shutil.copy2(document, output / document.name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", choices=("linux", "windows"))
    parser.add_argument("--build", type=Path)
    parser.add_argument("--version")
    parser.add_argument("--docs", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--mingw-bin", type=Path, default=Path(os.environ.get("MINGW_BIN", "C:/winlibs/mingw64/bin")),
                        help="MinGW runtime DLL directory (Windows; default $MINGW_BIN, else C:/winlibs/mingw64/bin)")
    parser.add_argument("--game", choices=("ds",), help="also build the player archive for this game")
    parser.add_argument("--dry-run", action="store_true", help="list the files and run every check; write nothing")
    args = parser.parse_args()
    if args.output is None and not args.dry_run:
        parser.error("--output is required unless --dry-run is given")
    try:
        if args.docs is not None:
            if args.platform is not None or args.build is not None or args.version is not None:
                parser.error("--docs cannot be combined with --platform, --build or --version")
            if args.dry_run:
                for document in sorted(args.docs.rglob("*.md")):
                    print(document)
            else:
                collect_docs(args.docs, args.output)
        else:
            if args.platform is None or args.build is None or args.version is None:
                parser.error("--platform, --build and --version are required when --docs is not specified")
            package(args.platform, args.build, args.output, args.version, args.mingw_bin, args.game, args.dry_run)
    except PackagingError as error:
        print(f"package_release: {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
