"""Apply Demon's Souls (PPSA01341) patches to the relinked Windows executable.

Each patch names an ELF vaddr in eboot.bin (the relinker maps ELF vaddr V at RVA V + 0x10000), the
bytes expected there, and their replacement. Code that does not fit in place goes to a "cave": a run
of int3 padding between functions in the original text section (.elf0), which the game never
executes. Patching is idempotent: a site that already holds its replacement is left alone, and any
other mismatch aborts before anything is written. Standard library only.

    patch_game.py [--fps120] [eboot.bin] DemonsSouls.exe

eboot.bin is accepted for compatibility with the earlier command line and is not read.

--fps120 turns the title's 60 FPS mode into a 120 FPS mode (FPS120-A, FPS120-B below). The game
steps physics once per frame by 1/refresh, so it must be paired with a 119.88 Hz vblank in the
runtime (APS5_VBLANK_HZ=119.88): without it the game caps at 60 FPS and physics runs at half speed.
See docs/research/frame-pacing.md in the notes repository.
"""
import argparse
import os
import struct
import sys

ELF_TO_RVA = 0x10000
TEXT_SECTION = b".elf0"


class Executable:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = bytearray(f.read())
        if self.data[:2] != b"MZ":
            raise RuntimeError(f"{path}: not a PE file")
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe:pe + 4] != b"PE\0\0":
            raise RuntimeError(f"{path}: not a PE file")
        count, = struct.unpack_from("<H", self.data, pe + 6)
        optional, = struct.unpack_from("<H", self.data, pe + 20)
        table = pe + 24 + optional
        self.sections = []
        for i in range(count):
            name, vsize, rva, rsize, raw = struct.unpack_from("<8sIIII", self.data, table + 40 * i)
            self.sections.append((name.rstrip(b"\0"), rva, vsize, raw, rsize))
        self.applied = []
        self.caves = self._find_caves()

    def offset(self, vaddr, size=1):
        rva = vaddr + ELF_TO_RVA
        for _, start, vsize, raw, rsize in self.sections:
            if start <= rva and rva + size <= start + min(vsize, rsize):
                return raw + rva - start
        raise RuntimeError(f"ELF vaddr {vaddr:#x} is not backed by the file")

    def read(self, vaddr, size):
        o = self.offset(vaddr, size)
        return bytes(self.data[o:o + size])

    def write(self, vaddr, blob):
        o = self.offset(vaddr, len(blob))
        self.data[o:o + len(blob)] = blob

    def _find_caves(self, minimum=15):
        """Inter-function padding: int3 runs that end at a 16-byte aligned function start.
        Clang pads with at most 15 bytes, so each cave holds one small code fragment."""
        caves = []
        for name, rva, vsize, raw, rsize in self.sections:
            if name != TEXT_SECTION:
                continue
            base = rva - ELF_TO_RVA
            code = self.data[raw:raw + min(vsize, rsize)]
            start = None
            for i, b in enumerate(code):
                if b == 0xCC:
                    if start is None:
                        start = i
                    continue
                end = base + i
                if start is not None and i - start >= minimum and end % 16 == 0:
                    # keep one int3 of the original padding on each side
                    caves.append([base + start + 1, end - 1])
                start = None
        return caves

    def cave(self, near, size):
        """Reserve `size` bytes of int3 padding within +-1 GiB of `near`."""
        for c in sorted(self.caves, key=lambda c: abs(c[0] - near)):
            if c[1] - c[0] >= size and abs(c[0] - near) < (1 << 30):
                addr = c[0]
                c[0] = c[1]  # one fragment per cave
                return addr
        raise RuntimeError("no code cave")

    def save(self):
        if not self.applied:
            return
        temporary = self.path + ".patching"
        with open(temporary, "wb") as f:
            f.write(self.data)
        os.replace(temporary, self.path)


def jmp(source, target):
    """jmp rel32 at `source`."""
    return b"\xE9" + struct.pack("<i", target - (source + 5))


def jmp_target(blob, source):
    return source + 5 + struct.unpack_from("<i", blob, 1)[0]


class DataPatch:
    """Replace `original` with `new` in place."""

    def __init__(self, name, vaddr, original, new, doc):
        self.name, self.vaddr, self.original, self.new, self.doc = name, vaddr, original, new, doc

    def state(self, exe):
        current = exe.read(self.vaddr, len(self.original))
        if current == self.original:
            return "absent"
        if current == self.new:
            return "present"
        return f"unexpected bytes at {self.vaddr:#x}: {current.hex()}"

    def apply(self, exe):
        exe.write(self.vaddr, self.new)


class CavePatch:
    """Jump from `vaddr` to a cave holding `body(cave)`, which jumps back to `resume`."""

    def __init__(self, name, vaddr, original, resume, size, body, doc):
        self.name, self.vaddr, self.original, self.resume = name, vaddr, original, resume
        self.size, self.body, self.doc = size, body, doc

    def site(self, cave):
        return jmp(self.vaddr, cave) + b"\x90" * (self.resume - self.vaddr - 5)

    def state(self, exe):
        current = exe.read(self.vaddr, len(self.original))
        if current == self.original:
            return "absent"
        if current[0] == 0xE9:
            cave = jmp_target(current, self.vaddr)
            try:
                if current == self.site(cave) and exe.read(cave, self.size) == self.body(cave):
                    return "present"
            except RuntimeError:
                pass
        return f"unexpected bytes at {self.vaddr:#x}: {current.hex()}"

    def apply(self, exe):
        cave = exe.cave(self.vaddr, self.size)
        body = self.body(cave)
        assert len(body) == self.size
        exe.write(cave, body)
        exe.write(self.vaddr, self.site(cave))


def job_worker_spin_body(cave):
    """shr rdx, 12 ; mov [rbp-50h], rdx ; jmp 0x83b6ba"""
    code = bytes.fromhex("48c1ea0c") + bytes.fromhex("488955b0")
    return code + jmp(cave + len(code), 0x83B6BA)


JOB_WORKER_SPIN = CavePatch(
    "job-worker-spin", 0x83B6B3, bytes.fromhex("4801d2488955b0"), 0x83B6BA, 13, job_worker_spin_body,
    """Job workers spin with `pause` for 2 * TSC frequency ticks (two seconds) of idleness before
    they sleep. A PS5 gives every worker its own core; on a PC a dozen spinning workers starve the
    main, render and driver threads. Spin for TSC frequency >> 12 ticks (~0.25 ms) instead.
        0x83b6b3: add rdx, rdx          ; threshold = 2 * frequency
        0x83b6b6: mov [rbp-50h], rdx""")

# 59.94f -> 119.88f. The refresh table at 0x24f7b90 is indexed by TargetFramesPerSecond (enum 1 is
# the 60 FPS mode) and read once, by FUN_14091b3a0; the game derives its nominal frame time, the
# gameMinDeltaTime floor and the physics step from it, so all three become 8.34 ms.
FPS120_A = DataPatch(
    "fps120-a", 0x24F7B94, bytes.fromhex("8fc26f42"), bytes.fromhex("8fc2ef42"),
    "60 FPS mode refresh 59.94 -> 119.88 (nominal frame time, delta floor, physics step)")
# The same constant for enum 1 in FUN_141481b10's table, which the time-sliced job runner
# FUN_1402eb3a0 uses to budget 95% of a frame.
FPS120_B = DataPatch(
    "fps120-b", 0x24F9A2C, bytes.fromhex("8fc26f42"), bytes.fromhex("8fc2ef42"),
    "60 FPS mode job-runner budget 59.94 -> 119.88")

ALWAYS = [JOB_WORKER_SPIN]
FPS120 = [FPS120_A, FPS120_B]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--fps120", action="store_true",
                        help="also apply FPS120-A/B (run the port with APS5_VBLANK_HZ=119.88)")
    parser.add_argument("paths", nargs="+", metavar="[eboot.bin] DemonsSouls.exe")
    args = parser.parse_args(argv)
    if len(args.paths) > 2:
        parser.error("expected [eboot.bin] DemonsSouls.exe")
    exe = Executable(args.paths[-1])
    wanted = ALWAYS + (FPS120 if args.fps120 else [])
    states = {patch.name: patch.state(exe) for patch in ALWAYS + FPS120}
    bad = [f"{p.name}: {states[p.name]}" for p in wanted if states[p.name] not in ("absent", "present")]
    if bad:
        raise SystemExit("refusing to patch, nothing written: " + "; ".join(bad))
    for patch in ALWAYS + FPS120:
        if patch not in wanted:
            print(f"{states[patch.name]} {patch.name} (not requested)")
        elif states[patch.name] == "absent":
            patch.apply(exe)
            exe.applied.append(patch.name)
            print(f"applied  {patch.name}")
        else:
            print(f"present  {patch.name}")
    exe.save()
    if states[FPS120_A.name] == "present" or FPS120_A.name in exe.applied:
        print("fps120-a is in the executable: run the port with APS5_VBLANK_HZ=119.88")
    return 0


if __name__ == "__main__":
    sys.exit(main())
