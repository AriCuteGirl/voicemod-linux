#!/usr/bin/env python3
"""Extract Qt resources (rcc format v1-v3) compiled into a PE image.

usage: extract_qrc.py EXE OUT_DIR [TREE_VA NAMES_VA DATA_VA]
The three addresses are the arguments of a qRegisterResourceData(version, tree, names, data) call.
Without them, every MSVC x64 registration stub in the image is found and extracted into
OUT_DIR/0, OUT_DIR/1, ...
Needs the `zstandard` module only for zstd-compressed entries.
"""
import re
import struct
import sys
import zlib
from pathlib import Path

FLAG_COMPRESSED, FLAG_DIRECTORY, FLAG_ZSTD = 1, 2, 4
ENTRY_SIZE = 22  # v2+: includes 8-byte last-modified


class Image:
    def __init__(self, data: bytes):
        self.data = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        count = struct.unpack_from("<H", data, pe + 6)[0]
        opt = struct.unpack_from("<H", data, pe + 20)[0]
        self.base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
        self.sections = []
        for i in range(count):
            off = pe + 24 + opt + i * 40
            vsize, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
            self.sections.append((va, max(vsize, rsize), raw))

    def offset(self, va: int) -> int:
        rva = va - self.base
        for sva, size, raw in self.sections:
            if sva <= rva < sva + size:
                return raw + rva - sva
        raise ValueError(f"address {va:#x} not in any section")


    def va_of(self, offset: int) -> int:
        for sva, size, raw in self.sections:
            if raw <= offset < raw + size:
                return self.base + sva + offset - raw
        raise ValueError(f"offset {offset:#x} not in any section")


# sub rsp,28h; lea r9,[rip+data]; mov ecx,ver; lea r8,[rip+names]; lea rdx,[rip+tree]; call
REGISTER_STUB = re.compile(
    rb"\x48\x83\xec\x28\x4c\x8d\x0d(....)\xb9[\x01-\x03]\x00\x00\x00"
    rb"\x4c\x8d\x05(....)\x48\x8d\x15(....)\xe8", re.S)


def find_registrations(img: Image):
    for m in REGISTER_STUB.finditer(img.data):
        rip = lambda group, end: img.va_of(m.start() + end) + struct.unpack("<i", m.group(group))[0]
        # Offsets of the end of each lea instruction within the match.
        yield rip(3, 30), rip(2, 23), rip(1, 11)


def extract(img: Image, out: Path, tree_va: int, names_va: int, data_va: int) -> None:
    d = img.data
    tree, names, blob = img.offset(tree_va), img.offset(names_va), img.offset(data_va)
    out_root = out.resolve()

    def name_at(off: int) -> str:
        length = struct.unpack_from(">H", d, names + off)[0]
        return d[names + off + 6 : names + off + 6 + length * 2].decode("utf-16-be")

    def walk(index: int, path: Path) -> None:
        e = tree + index * ENTRY_SIZE
        name_off, flags = struct.unpack_from(">IH", d, e)
        name = name_at(name_off) if index else ""
        here = path / name if name else path
        if flags & FLAG_DIRECTORY:
            count, first = struct.unpack_from(">II", d, e + 6)
            for child in range(first, first + count):
                walk(child, here)
            return
        data_off = struct.unpack_from(">I", d, e + 10)[0]
        size = struct.unpack_from(">I", d, blob + data_off)[0]
        payload = d[blob + data_off + 4 : blob + data_off + 4 + size]
        if flags & FLAG_COMPRESSED:
            payload = zlib.decompress(payload[4:])
        elif flags & FLAG_ZSTD:
            import zstandard
            payload = zstandard.ZstdDecompressor().decompress(payload)
        target = here.resolve()
        if out_root not in target.parents:
            raise ValueError(f"refusing path outside output dir: {here}")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)

    walk(0, out_root)


def main() -> None:
    img = Image(Path(sys.argv[1]).read_bytes())
    out = Path(sys.argv[2])
    if len(sys.argv) == 6:
        extract(img, out, *(int(x, 16) for x in sys.argv[3:6]))
        return
    found = list(find_registrations(img))
    if not found:
        sys.exit("no qRegisterResourceData stubs found")
    for i, (tree, names, data) in enumerate(found):
        print(f"{i}: tree={tree:#x} names={names:#x} data={data:#x}")
        extract(img, out / str(i), tree, names, data)


if __name__ == "__main__":
    main()
