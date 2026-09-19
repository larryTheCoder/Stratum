#!/usr/bin/env python3
"""Dump the block palette of chunks in a vanilla region file.

Used to establish, empirically, what post-terrain steps ran when a golden
region was generated: a Nether region with no glowstone and no quartz ore had
its features removed, and one with no cave-shaped air had its carvers removed.

Reads Mojang-derived fixtures under .fixtures (never committed, SPEC §12) and
prints a census. Nothing here is a fixture itself.

Usage: python3 tools/analysis/region-palette.py <region.mca> [max_chunks]
"""
import collections
import struct
import sys
import zlib


def read_nbt(buf, pos, tag_type, named=True):
    """Minimal NBT reader. Returns (value, pos)."""
    if named and tag_type != 0:
        (name_len,) = struct.unpack_from(">H", buf, pos)
        pos += 2 + name_len
    return read_payload(buf, pos, tag_type)


def read_payload(buf, pos, tag_type):
    if tag_type == 0:
        return None, pos
    if tag_type == 1:
        return struct.unpack_from(">b", buf, pos)[0], pos + 1
    if tag_type == 2:
        return struct.unpack_from(">h", buf, pos)[0], pos + 2
    if tag_type == 3:
        return struct.unpack_from(">i", buf, pos)[0], pos + 4
    if tag_type == 4:
        return struct.unpack_from(">q", buf, pos)[0], pos + 8
    if tag_type == 5:
        return struct.unpack_from(">f", buf, pos)[0], pos + 4
    if tag_type == 6:
        return struct.unpack_from(">d", buf, pos)[0], pos + 8
    if tag_type == 7:
        (n,) = struct.unpack_from(">i", buf, pos)
        pos += 4
        return buf[pos:pos + n], pos + n
    if tag_type == 8:
        (n,) = struct.unpack_from(">H", buf, pos)
        pos += 2
        return buf[pos:pos + n].decode("utf-8", "replace"), pos + n
    if tag_type == 9:
        (elem,) = struct.unpack_from(">b", buf, pos)
        pos += 1
        (n,) = struct.unpack_from(">i", buf, pos)
        pos += 4
        out = []
        for _ in range(n):
            value, pos = read_payload(buf, pos, elem)
            out.append(value)
        return out, pos
    if tag_type == 10:
        out = {}
        while True:
            (child,) = struct.unpack_from(">b", buf, pos)
            pos += 1
            if child == 0:
                return out, pos
            (name_len,) = struct.unpack_from(">H", buf, pos)
            pos += 2
            name = buf[pos:pos + name_len].decode("utf-8", "replace")
            pos += name_len
            value, pos = read_payload(buf, pos, child)
            out[name] = value
    if tag_type == 11:
        (n,) = struct.unpack_from(">i", buf, pos)
        pos += 4
        return list(struct.unpack_from(">%di" % n, buf, pos)), pos + 4 * n
    if tag_type == 12:
        (n,) = struct.unpack_from(">i", buf, pos)
        pos += 4
        return list(struct.unpack_from(">%dq" % n, buf, pos)), pos + 8 * n
    raise ValueError("unknown NBT tag %d at %d" % (tag_type, pos))


def chunks(path, limit=None):
    with open(path, "rb") as handle:
        data = handle.read()
    seen = 0
    for index in range(1024):
        offset = int.from_bytes(data[index * 4:index * 4 + 3], "big")
        count = data[index * 4 + 3]
        if offset == 0 or count == 0:
            continue
        start = offset * 4096
        (length,) = struct.unpack_from(">i", data, start)
        scheme = data[start + 4]
        payload = data[start + 5:start + 4 + length]
        if scheme == 2:
            raw = zlib.decompress(payload)
        elif scheme == 1:
            raw = zlib.decompress(payload, 16 + zlib.MAX_WBITS)
        else:
            continue
        (root_type,) = struct.unpack_from(">b", raw, 0)
        value, _ = read_nbt(raw, 1, root_type)
        yield value
        seen += 1
        if limit is not None and seen >= limit:
            return


def main():
    path = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 16
    census = collections.Counter()
    total = 0
    for chunk in chunks(path, limit):
        total += 1
        for section in chunk.get("sections", []):
            states = section.get("block_states") or {}
            for entry in states.get("palette", []):
                census[entry.get("Name", "?")] += 1
    print("%d chunk(s) of %s" % (total, path))
    print("%d distinct block names in section palettes:" % len(census))
    for name, count in census.most_common():
        print("   %-44s in %d section palette(s)" % (name, count))


if __name__ == "__main__":
    main()
