"""Apply the Demon's Souls (PPSA01341) code patches to a relinked executable.

Standard library only (Python 3.8+), so players need nothing beyond a stock Python. Works on the
Windows PE (relinker --windows) and on the Linux ELF output.

Each patch names an ELF vaddr in the dump's eboot.bin, the bytes expected there, and replacement
code. Code that does not fit in place goes to a "cave": a run of int3 padding between functions in
the original text segment, which the game never executes. The eboot bytes act as the version check:
a dump whose bytes differ is refused and nothing is written. Patching is idempotent: a site that
already holds its replacement is reported as present.

The text segment's position in the output is found two ways and both must agree when both apply:
the PE mapping the relinker uses (ELF vaddr V at RVA V + 0x10000), and a search for unique byte
windows of the eboot text around each site (the only way for the ELF output).

usage: ds_patch.py [--check] <eboot.bin> <DemonsSouls.exe|DemonsSouls.elf>
exit codes: 0 patched or already present, 1 usage, 2 refused (nothing written)
"""
import argparse
import os
import stat
import struct
import sys

ELF_TO_RVA = 0x10000  # Windows output: the relinker maps ELF vaddr V at RVA V + 0x10000 (.elf0 = text)
PT_LOAD = 1
PF_X = 1


class PatchError(Exception):
    pass


def read_elf_segments(data):
    if data[:4] != b"\x7fELF":
        raise PatchError("eboot.bin is not a decrypted ELF (an encrypted SELF needs decrypting first)")
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    segments = []
    for i in range(phnum):
        p_type, flags, offset, vaddr, filesz, memsz = struct.unpack_from("<IIQQ8xQQ", data, phoff + i * phentsize)
        segments.append((p_type, flags, offset, vaddr, filesz, memsz))
    return segments


class Eboot:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        loads = [s for s in read_elf_segments(self.data) if s[0] == PT_LOAD and s[1] != 0]
        if not loads or not loads[0][1] & PF_X:
            raise PatchError("eboot.bin: first loadable segment is not executable")
        _, _, self.text_offset, self.text_vaddr, self.text_size, _ = loads[0]
        self.caves = self._find_caves()

    def text_contains(self, vaddr, size):
        return self.text_vaddr <= vaddr and vaddr + size <= self.text_vaddr + self.text_size

    def read(self, vaddr, size):
        if not self.text_contains(vaddr, size):
            raise PatchError(f"vaddr {vaddr:#x} is outside the eboot text segment")
        o = vaddr - self.text_vaddr + self.text_offset
        return self.data[o:o + size]

    def _find_caves(self, minimum=15):
        """Inter-function padding: int3 runs that end at a 16-byte aligned function start.
        Clang pads with at most 15 bytes, so each cave holds one small code fragment.
        Same rule as the development patcher, so both pick the same caves."""
        code = self.data[self.text_offset:self.text_offset + self.text_size]
        caves = []
        i, n = 0, len(code)
        while True:
            start = code.find(b"\xcc", i)
            if start < 0:
                break
            end = start
            while end < n and code[end] == 0xCC:
                end += 1
            if end < n:  # a run that reaches the segment end never ends at a function start
                va_end = self.text_vaddr + end
                if end - start >= minimum and va_end % 16 == 0:
                    # keep one int3 of the original padding on each side
                    caves.append([self.text_vaddr + start + 1, va_end - 1])
            i = end + 1
        return caves

    def cave(self, near, size):
        """Reserve `size` bytes of int3 padding within +-1 GiB of `near`."""
        for c in sorted(self.caves, key=lambda c: abs(c[0] - near)):
            if c[1] - c[0] >= size and abs(c[0] - near) < (1 << 30):
                addr = c[0]
                c[0] = c[1]  # one fragment per cave
                return addr
        raise PatchError("no code cave")


def pe_text_delta(data, eboot):
    """File offset minus ELF vaddr for the text segment in a relinker PE output, or None."""
    if data[:2] != b"MZ":
        return None
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    nsections = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + opt_size
    rva = eboot.text_vaddr + ELF_TO_RVA
    for i in range(nsections):
        _, vsize, va, rawsize, rawptr = struct.unpack_from("<8sIIII", data, table + 40 * i)
        if va <= rva and rva + eboot.text_size <= va + min(vsize, rawsize):
            return rawptr - va + ELF_TO_RVA
    raise PatchError("PE output: no section holds the eboot text segment (not a relinker --windows output?)")


def unique_find(data, needle):
    first = data.find(needle)
    if first < 0 or data.find(needle, first + 1) >= 0:
        return None
    return first


def anchor_delta(data, eboot, vaddrs, width=48):
    """Find eboot text windows near `vaddrs` in the output; every unique hit must give one delta."""
    deltas = set()
    for va in vaddrs:
        for start in (va - 2 * width, va - width, va + width, va + 2 * width):
            if not eboot.text_contains(start, width):
                continue
            window = eboot.read(start, width)
            if window.count(b"\xcc") > width // 2:
                continue  # padding is not distinctive
            at = unique_find(data, window)
            if at is not None:
                deltas.add(at - start)
    if len(deltas) > 1:
        raise PatchError("eboot text found at several places in the output; refusing to guess")
    return deltas.pop() if deltas else None


# x86-64 encodings (hand assembled, checked against the development patcher's output)
def jmp_rel32(source, target):
    rel = target - (source + 5)
    if not -(1 << 31) <= rel < (1 << 31):
        raise PatchError("jump out of rel32 range")
    return b"\xe9" + struct.pack("<i", rel)


def job_worker_spin(eboot):
    """Job workers spin with `pause` for 2 * TSC frequency ticks (two seconds) of idleness before
    they sleep. A PS5 gives every worker its own core; on a PC a dozen spinning workers starve
    the main, render and driver threads. Spin for TSC frequency >> 12 ticks (~0.25 ms) instead.

        0x83b6b3: add rdx, rdx          ; threshold = 2 * frequency
        0x83b6b6: mov [rbp-50h], rdx
    """
    site, resume = 0x83B6B3, 0x83B6BA
    cave = eboot.cave(site, 13)
    body = bytes.fromhex("48c1ea0c")       # shr rdx, 12
    body += bytes.fromhex("488955b0")      # mov [rbp-50h], rdx
    body += jmp_rel32(cave + len(body), resume)
    jump = jmp_rel32(site, cave)
    jump += b"\x90" * (resume - site - len(jump))
    return [(cave, body), (site, jump)]


PATCHES = [
    ("job-worker-spin", 0x83B6B3, bytes.fromhex("4801d2488955b0"), job_worker_spin),
]


def apply(eboot_path, exe_path, check_only=False):
    eboot = Eboot(eboot_path)
    plans = []
    for name, vaddr, original, build in PATCHES:
        found = eboot.read(vaddr, len(original))
        if found != original:
            raise PatchError(f"{name}: this eboot.bin has {found.hex()} at {vaddr:#x}, expected {original.hex()}: "
                             "unsupported game version (the port supports BuildVersion 2025-10-15.877562)")
        plans.append((name, vaddr, original, build(eboot)))

    with open(exe_path, "rb") as f:
        data = bytearray(f.read())
    sites = [va for _, _, _, writes in plans for va, _ in writes]
    pe_delta = pe_text_delta(data, eboot)
    found_delta = anchor_delta(data, eboot, sites)
    if pe_delta is not None and found_delta is not None and pe_delta != found_delta:
        raise PatchError(f"output layout mismatch (PE mapping {pe_delta:#x}, byte search {found_delta:#x})")
    delta = pe_delta if pe_delta is not None else found_delta
    if delta is None:
        raise PatchError("cannot locate the game's code in the output (is it a relinker output of this eboot.bin?)")

    def out(va, size):
        o = va + delta
        if o < 0 or o + size > len(data):
            raise PatchError(f"vaddr {va:#x} maps outside the output file")
        return o

    changed = False
    for name, vaddr, original, writes in plans:
        site_now = bytes(data[out(vaddr, len(original)):out(vaddr, len(original)) + len(original)])
        if all(bytes(data[out(va, len(b)):out(va, len(b)) + len(b)]) == b for va, b in writes):
            print(f"present  {name}")
            continue
        if site_now != original:
            raise PatchError(f"{name}: unexpected bytes at {vaddr:#x} in the output: {site_now.hex()}")
        for va, blob in writes:
            if va == vaddr:
                continue
            now = bytes(data[out(va, len(blob)):out(va, len(blob)) + len(blob)])
            if now != b"\xcc" * len(blob):
                raise PatchError(f"{name}: code cave at {va:#x} is not padding in the output: {now.hex()}")
        if not check_only:
            for va, blob in writes:
                o = out(va, len(blob))
                data[o:o + len(blob)] = blob
            changed = True
        print(f"{'would apply' if check_only else 'applied'}  {name}")

    if changed:
        temporary = exe_path + ".patching"
        with open(temporary, "wb") as f:
            f.write(data)
        os.chmod(temporary, stat.S_IMODE(os.stat(exe_path).st_mode))  # keep the executable bit (Linux)
        os.replace(temporary, exe_path)


def main():
    parser = argparse.ArgumentParser(description="Patch a relinked Demon's Souls executable (stdlib only).")
    parser.add_argument("--check", action="store_true", help="verify only, write nothing")
    parser.add_argument("eboot")
    parser.add_argument("executable")
    args = parser.parse_args()
    try:
        apply(args.eboot, args.executable, args.check)
    except (PatchError, OSError) as error:
        print(f"ds_patch: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
