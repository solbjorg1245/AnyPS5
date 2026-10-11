#!/usr/bin/env python3
"""Reads an APS5_PASS_DUMP dump (index.csv; see core/libs/prx/libSceAgcDriver/Execution/include/Driver/PassDump.hpp)
and prints:
  1. where NaN or infinity first appear: the first row of each image that holds any, in frame order, and
     likewise the first row of each image with tile-stepped holes (edge tiles);
  2. per image, the rows that read it back (NaN, infinity, zero fraction, bad and edge tiles), for the images
     with NaN, infinity or hole edges (--all: every image);
  3. the producer chain of the presented image: the pass or dispatch that last wrote it, the textures that
     pass sampled, the last writers of those, and so on (--depth levels).
usage: passdump_report.py [dump folder or index.csv] [--all] [--depth 8] [--limit 60]
       (default: the newest folder under E:/projects/dsd/port/passdump)
Plain Python, no third-party modules.
"""
import csv
import os
import sys

DEFAULT_ROOT = "E:/projects/dsd/port/passdump"
INTS = ("seq", "event", "present", "slot", "queue", "bytes", "vkformat", "guest_format", "width", "height", "level", "layer",
        "layers", "alias", "aliases", "draws", "skipped", "texels", "nan", "inf", "zero", "negative", "tiles", "bad_tiles",
        "edge_tiles")
FLOATS = ("nan_frac", "inf_frac", "zero_frac", "neg_frac", "ms")
IMAGE_PLANES = ("color", "depth", "storage", "buffer", "display")


def newest(root):
    folders = [os.path.join(root, name) for name in os.listdir(root) if os.path.isfile(os.path.join(root, name, "index.csv"))]
    if not folders:
        raise SystemExit(f"no dump under {root}")
    return max(folders, key=os.path.getmtime)


def load(path):
    if os.path.isdir(path):
        path = os.path.join(path, "index.csv")
    complete, lines = None, []
    with open(path, newline="") as file:
        for line in file:
            if line.startswith("#complete"):
                complete = line.strip()
            elif not line.startswith("#"):
                lines.append(line)
    rows = []
    for row in csv.DictReader(lines):
        try:
            for key in INTS:
                row[key] = int(row[key] or 0)
            for key in FLOATS:
                row[key] = float(row[key] or 0)
            row["addr"] = int(row["address"], 16) if row["address"] else 0
        except (TypeError, ValueError, KeyError):
            continue  # the last line of a dump still being written
        rows.append(row)
    return path, rows, complete


def flags(row):
    if row["format"] == "no-image":
        return "not resident"
    if row["plane"] not in IMAGE_PLANES:
        return ""
    text = f"nan={row['nan']} inf={row['inf']} zero={100 * row['zero_frac']:.1f}% neg={row['negative']} bad={row['bad_tiles']}/{row['tiles']} edge={row['edge_tiles']}"
    if row["min"] or row["max"]:
        text += f" range=[{row['min']},{row['max']}]"
    return text


def bad(row):
    return row["nan"] > 0 or row["inf"] > 0 or row["edge_tiles"] > 0


def programs(row):
    if row["kind"] == "dispatch":
        return f"cs={row['cs']}"
    if row["kind"] == "draw-pass":
        return f"draws={row['draws']}+{row['skipped']} vs={row['vs']} ps={row['ps']}"
    return ""


def describe(row):
    return (f"#{row['seq']} e{row['event']} {row['kind']} q{row['queue']} {row['plane']} 0x{row['addr']:x} "
            f"{row['format']} {row['width']}x{row['height']}"
            + (f" m{row['level']}" if row["level"] else "") + (f" l{row['layer']}" if row["layer"] else "")
            + (f" alias{row['alias']}/{row['aliases']}" if row["aliases"] > 1 else ""))


def image_key(row):
    return (row["plane"], row["addr"], row["level"], row["layer"], row["alias"])


def first_rows(rows, test):
    seen, found = set(), []
    for row in rows:
        key = image_key(row)
        if row["plane"] in IMAGE_PLANES and key not in seen and test(row):
            seen.add(key)
            found.append(row)
    return found


def main():
    args = [arg for arg in sys.argv[1:] if not arg.startswith("--")]
    show_all = "--all" in sys.argv
    depth = int(sys.argv[sys.argv.index("--depth") + 1]) if "--depth" in sys.argv else 8
    limit = int(sys.argv[sys.argv.index("--limit") + 1]) if "--limit" in sys.argv else 60
    if "--depth" in sys.argv:
        args.remove(str(depth))
    if "--limit" in sys.argv:
        args.remove(str(limit))
    path, rows, complete = load(args[0] if args else newest(DEFAULT_ROOT))
    events = {row["event"] for row in rows}
    images = [row for row in rows if row["plane"] in IMAGE_PLANES]
    print(f"{path}: {len(rows)} rows, {len(events)} events, {sum(1 for row in images if row['format'] == 'no-image')} not resident")
    print(complete or "(no #complete line: the dump is partial)")

    print("\n== 1. first NaN/infinity per image (frame order)")
    nan_rows = first_rows(rows, lambda row: row["nan"] > 0 or row["inf"] > 0)
    for row in nan_rows[:limit]:
        print(f"  {describe(row)}  {flags(row)}  {programs(row)}")
    if not nan_rows:
        print("  none: no image read back holds NaN or infinity")
    print("\n== 1b. first hole edges per image (16x16 tiles >= 90% zero/NaN next to one that is not)")
    edge_rows = first_rows(rows, lambda row: row["edge_tiles"] > 0)
    for row in edge_rows[:limit]:
        print(f"  {describe(row)}  {flags(row)}  {programs(row)}")
    if not edge_rows:
        print("  none")

    print("\n== 2. per-image timelines" + ("" if show_all else " (images with NaN, infinity or hole edges)"))
    timelines = {}
    for row in images:
        timelines.setdefault((row["plane"], row["addr"], row["level"], row["layer"]), []).append(row)
    shown = 0
    for key, history in sorted(timelines.items(), key=lambda item: item[1][0]["seq"]):
        if not show_all and not any(bad(row) for row in history):
            continue
        shown += 1
        if shown > limit:
            print(f"  ... ({len(timelines)} images; --limit)")
            break
        head = history[0]
        print(f"  {head['plane']} 0x{head['addr']:x} {head['format']} {head['width']}x{head['height']}"
              + (f" m{head['level']}" if head["level"] else "") + (f" l{head['layer']}" if head["layer"] else ""))
        for row in history:
            print(f"    #{row['seq']:<6} e{row['event']:<5} {row['kind']:<12} {flags(row)}  {programs(row)}")

    print("\n== 3. producer chain of the presented image")
    first_seq = {}
    for row in rows:
        first_seq.setdefault(row["event"], row["seq"])
    writers = {}
    for row in rows:
        if row["kind"] in ("draw-pass", "dispatch") and row["plane"] in IMAGE_PLANES:
            writers.setdefault(row["addr"], []).append(row)

    def last_writer(address, before):
        best = None
        for row in writers.get(address, []):
            if row["seq"] < before and (best is None or row["seq"] > best["seq"]):
                best = row
        return best

    def event_rows(event):
        return [row for row in rows if row["event"] == event and row["plane"] in IMAGE_PLANES]

    displays = [row for row in rows if row["kind"] == "present-blit"]
    root = displays[-1] if displays else (images[-1] if images else None)
    if root is None:
        print("  no image rows")
        return
    printed, visited = [0], set()

    def walk(row, level, label):
        if printed[0] >= limit * 4:
            return
        printed[0] += 1
        indent = "  " * (level + 1)
        mark = "! " if bad(row) else "  "
        print(f"{indent}{mark}{label}{describe(row)}  {flags(row)}  {programs(row)}")
        if row["kind"] == "present-blit":
            writer = last_writer(row["addr"], row["seq"])
            if writer is None:
                print(f"{indent}    (no pass or dispatch of the frame wrote 0x{row['addr']:x})")
                return
            walk(writer, level + 1, "written by ")
            return
        if row["event"] in visited:
            print(f"{indent}    (event e{row['event']} expanded above)")
            return
        visited.add(row["event"])
        for other in event_rows(row["event"]):
            if other["seq"] != row["seq"]:
                print(f"{indent}    {'! ' if bad(other) else '  '}same event: {describe(other)}  {flags(other)}")
        if level >= depth:
            return
        inputs = [int(item, 16) for item in row["inputs"].split(";") if item]
        for address in inputs:
            writer = last_writer(address, first_seq[row["event"]])
            if writer is None:
                print(f"{indent}    input 0x{address:x}: not written earlier in the frame")
            else:
                walk(writer, level + 1, f"input 0x{address:x} <- ")

    walk(root, 0, "")


if __name__ == "__main__":
    main()
