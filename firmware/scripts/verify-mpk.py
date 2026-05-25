#!/usr/bin/env python3
"""Validate embedded MPK1 sprite packs against the firmware reader contract.

MPK1 is the build-time pack format the device embeds (EMBED_FILES in
firmware/main/CMakeLists.txt) and reads with firmware/main/mochi_pack.h.
This script mirrors that reader's parsing so the committed packs
(firmware/main/assets/*.mpk) can be checked without flashing hardware —
useful in CI and as the firmware-side half of the device-sprite
consolidation conformance (the val-side half lives in
c15r/mochi-device/conformance.ts).

Format (format byte 0 — cells only; design/13-build-time-asset-packs.md):

  header (16 bytes, little-endian envelope):
    0..3   magic "MPK1"   4: version=1   5: format=0
    6..7   cell_w u16 LE   8..9: cell_h u16 LE   10..11: count u16 LE
    12: label_len u8       13: flags (bit0=has_mask)   14..15: reserved
  entries (count), each `stride` bytes:
    label     label_len bytes, NUL-padded UTF-8
    cell blob 8-byte BIG-endian header (w, h, flags, 3 reserved)
              + ink plane [+ mask plane]
  plane_bytes = ((cell_w + 7) // 8) * cell_h
  cell_bytes  = 8 + (has_mask ? 2 : 1) * plane_bytes
  stride      = label_len + cell_bytes

format byte 1 (zones + typed actions, design/14) is variable-stride: a
u32 LE entry-offset directory, per-entry zone trailers (24 B each), and a
pack-global talk_seed label table. Validated by _validate_fmt1 below.

Usage:
    verify-mpk.py [pack.mpk ...]      # defaults to firmware/main/assets/*.mpk
Exit status: 0 if every pack validates, 1 otherwise.
"""

import glob
import os
import struct
import sys


def _validate_fmt1(path, d, cell_w, cell_h, count, label_len, has_mask) -> list[str]:
    """Validate a format=1 pack (directory + per-entry zones + label table)."""
    errs: list[str] = []
    plane_bytes = ((cell_w + 7) // 8) * cell_h
    cell_bytes = 8 + (2 if has_mask else 1) * plane_bytes
    if 16 + count * 4 > len(d):
        return ["directory runs past EOF"]
    offsets = [struct.unpack_from("<I", d, 16 + i * 4)[0] for i in range(count)]

    labels: list[str] = []
    zone_total = 0
    for i, off in enumerate(offsets):
        if off + label_len + cell_bytes + 1 > len(d):
            errs.append(f"entry {i} runs past EOF")
            continue
        labels.append(d[off:off + label_len].split(b"\x00", 1)[0].decode("utf-8", "replace"))
        w, h, cf = struct.unpack_from(">HHB", d, off + label_len)
        if w != cell_w or h != cell_h or (cf & 1) != has_mask:
            errs.append(f"entry {i} ({labels[-1]!r}): cell header {w}x{h}/mask{cf & 1} "
                        f"!= envelope {cell_w}x{cell_h}/mask{has_mask}")
        zc = d[off + label_len + cell_bytes]
        if zc > 15:
            errs.append(f"entry {i}: zone_count {zc} > 15")
        zone_total += zc
        zbase = off + label_len + cell_bytes + 1
        for z in range(zc):
            zp = zbase + z * 24
            if zp + 24 > len(d):
                errs.append(f"entry {i} zone {z} runs past EOF")
                break
            kind = d[zp + 8]
            # MPK_ACTION_* in firmware/main/mochi_pack.h:
            # 0 NONE, 1 EVENT, 2 NAV_SCENE, 3 NAV_RELATIVE,
            # 4 TALK_SEED, 5 NAV_PLACE, 6 TEXT, 7 PET.
            if kind > 7:
                errs.append(f"entry {i} zone {z}: action_kind {kind} > 7")

    # label table sits just past the last entry.
    n_labels = 0
    if count and not errs:
        last = offsets[-1]
        lzc = d[last + label_len + cell_bytes]
        lt = last + label_len + cell_bytes + 1 + lzc * 24
        if lt + 2 > len(d):
            errs.append("label table runs past EOF")
        else:
            n_labels = struct.unpack_from("<H", d, lt)[0]
            lt += 2
            for _ in range(n_labels):
                if lt >= len(d):
                    errs.append("label table truncated")
                    break
                lt += 1 + d[lt]
            if lt != len(d):
                errs.append(f"trailing bytes after label table (end {lt} != {len(d)})")
        # talk_seed label_idx must be in range
        for i, off in enumerate(offsets):
            zbase = off + label_len + cell_bytes + 1
            for z in range(d[off + label_len + cell_bytes]):
                zp = zbase + z * 24
                if d[zp + 8] == 4:  # talk_seed
                    li = struct.unpack_from("<H", d, zp + 10)[0]
                    if li != 0xFFFF and li >= n_labels:
                        errs.append(f"entry {i} zone {z}: talk_seed label_idx {li} >= {n_labels}")

    status = "OK" if not errs else "FAIL"
    print(f"  {os.path.basename(path)}: {status} — format=1 {cell_w}x{cell_h}, "
          f"{count} cells, {zone_total} zones, {n_labels} seeds, "
          f"label_len={label_len}, mask={has_mask}, {len(d)} bytes")
    if labels:
        print(f"    labels: {', '.join(labels)}")
    return errs


def validate(path: str) -> list[str]:
    """Return a list of error strings (empty == valid). Prints a summary."""
    errs: list[str] = []
    with open(path, "rb") as fh:
        d = fh.read()

    if len(d) < 16:
        return [f"too short: {len(d)} bytes"]
    if d[0:4] != b"MPK1":
        return [f"bad magic: {d[0:4]!r}"]
    version, fmt = d[4], d[5]
    if version != 1:
        return [f"unsupported version: {version}"]

    cell_w, cell_h, count = struct.unpack_from("<HHH", d, 6)
    label_len, flags = d[12], d[13]
    has_mask = flags & 1

    if fmt == 1:
        return _validate_fmt1(path, d, cell_w, cell_h, count, label_len, has_mask)
    if fmt != 0:
        return [f"unsupported format {fmt}"]

    plane_bytes = ((cell_w + 7) // 8) * cell_h
    cell_bytes = 8 + (2 if has_mask else 1) * plane_bytes
    stride = label_len + cell_bytes
    expected = 16 + count * stride
    if expected != len(d):
        errs.append(f"size {len(d)} != 16 + count*stride ({expected})")

    labels: list[str] = []
    for i in range(count):
        off = 16 + i * stride
        if off + stride > len(d):
            errs.append(f"entry {i} runs past EOF")
            break
        labels.append(d[off:off + label_len].split(b"\x00", 1)[0]
                      .decode("utf-8", "replace"))
        w, h, cf = struct.unpack_from(">HHB", d, off + label_len)
        if w != cell_w or h != cell_h:
            errs.append(f"entry {i} ({labels[-1]!r}): cell header {w}x{h} "
                        f"!= envelope {cell_w}x{cell_h}")
        if (cf & 1) != has_mask:
            errs.append(f"entry {i} ({labels[-1]!r}): cell flags mask "
                        f"{cf & 1} != envelope {has_mask}")

    status = "OK" if not errs else "FAIL"
    print(f"  {os.path.basename(path)}: {status} — {cell_w}x{cell_h}, "
          f"{count} cells, label_len={label_len}, mask={has_mask}, "
          f"stride={stride}, {len(d)} bytes")
    if labels:
        print(f"    labels: {', '.join(labels)}")
    return errs


def main(argv: list[str]) -> int:
    paths = argv[1:]
    if not paths:
        here = os.path.dirname(os.path.abspath(__file__))
        assets = os.path.join(here, "..", "main", "assets", "*.mpk")
        paths = sorted(glob.glob(assets))
    if not paths:
        print("no .mpk packs found", file=sys.stderr)
        return 1

    print(f"verify-mpk: checking {len(paths)} pack(s)")
    failed = 0
    for p in paths:
        errs = validate(p)
        for e in errs:
            print(f"    ERROR: {e}", file=sys.stderr)
        failed += bool(errs)

    if failed:
        print(f"\n{failed}/{len(paths)} pack(s) FAILED", file=sys.stderr)
        return 1
    print(f"\nall {len(paths)} pack(s) valid")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
