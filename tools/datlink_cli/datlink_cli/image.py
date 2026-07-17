from __future__ import annotations

import struct
from pathlib import Path


def _merge(chunks: list[tuple[int, bytes]]) -> list[tuple[int, bytes]]:
    result: list[tuple[int, bytearray]] = []
    for address, data in sorted(chunks):
        if not data:
            continue
        if result and address == result[-1][0] + len(result[-1][1]):
            result[-1][1].extend(data)
        elif result and address < result[-1][0] + len(result[-1][1]):
            raise ValueError("input contains overlapping load regions")
        else:
            result.append((address, bytearray(data)))
    return [(address, bytes(data)) for address, data in result]


def parse_ihex(path: Path) -> list[tuple[int, bytes]]:
    memory: dict[int, int] = {}
    upper = 0
    eof = False
    for line_number, text in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
        text = text.strip()
        if not text:
            continue
        if not text.startswith(":"):
            raise ValueError(f"HEX line {line_number}: missing ':'")
        raw = bytes.fromhex(text[1:])
        if len(raw) < 5 or len(raw) != raw[0] + 5 or sum(raw) & 0xFF:
            raise ValueError(f"HEX line {line_number}: invalid length/checksum")
        count, address, kind = raw[0], int.from_bytes(raw[1:3], "big"), raw[3]
        data = raw[4:4 + count]
        if kind == 0:
            absolute = upper + address
            for offset, value in enumerate(data):
                key = absolute + offset
                if key in memory and memory[key] != value:
                    raise ValueError(f"HEX line {line_number}: conflicting data")
                memory[key] = value
        elif kind == 1:
            eof = True
            break
        elif kind == 2:
            if count != 2:
                raise ValueError("invalid extended segment record")
            upper = int.from_bytes(data, "big") << 4
        elif kind == 4:
            if count != 2:
                raise ValueError("invalid extended linear record")
            upper = int.from_bytes(data, "big") << 16
        elif kind in (3, 5):
            continue
        else:
            raise ValueError(f"HEX line {line_number}: unsupported record {kind}")
    if not eof or not memory:
        raise ValueError("HEX file has no data or EOF record")
    chunks: list[tuple[int, bytes]] = []
    start = previous = min(memory)
    data = bytearray([memory[start]])
    for address in sorted(memory)[1:]:
        if address == previous + 1:
            data.append(memory[address])
        else:
            chunks.append((start, bytes(data)))
            start, data = address, bytearray([memory[address]])
        previous = address
    chunks.append((start, bytes(data)))
    return chunks


def parse_elf(path: Path) -> list[tuple[int, bytes]]:
    blob = path.read_bytes()
    if len(blob) < 52 or blob[:4] != b"\x7fELF" or blob[4] != 1 or blob[5] != 1:
        raise ValueError("only ELF32 little-endian files are supported")
    phoff, = struct.unpack_from("<I", blob, 28)
    phentsize, phnum = struct.unpack_from("<HH", blob, 42)
    if phentsize < 32 or phoff + phentsize * phnum > len(blob):
        raise ValueError("invalid ELF program header table")
    chunks = []
    for index in range(phnum):
        fields = struct.unpack_from("<IIIIIIII", blob, phoff + index * phentsize)
        kind, offset, vaddr, paddr, file_size, _, _, _ = fields
        if kind != 1 or file_size == 0:
            continue
        address = paddr if paddr < 0x20000 else vaddr
        if address < 0x20000 and offset + file_size <= len(blob):
            chunks.append((address, blob[offset:offset + file_size]))
    if not chunks:
        raise ValueError("ELF has no loadable MAIN-flash segment")
    return _merge(chunks)


def load_image(path_value: str, base: int | None = None) -> list[tuple[int, bytes]]:
    path = Path(path_value)
    suffix = path.suffix.lower()
    if suffix in (".elf", ".out", ".axf"):
        chunks = parse_elf(path)
    elif suffix in (".hex", ".ihex"):
        chunks = parse_ihex(path)
    elif suffix == ".bin":
        if base is None:
            raise ValueError("BIN input requires --base")
        chunks = [(base, path.read_bytes())]
    else:
        raise ValueError(f"unsupported image format: {suffix}")
    chunks = _merge(chunks)
    if len(chunks) > 8:
        raise ValueError("image has more than eight disjoint segments")
    for address, data in chunks:
        if address < 0 or address & 7 or not data or address + len(data) > 0x20000:
            raise ValueError("image reaches outside MAIN flash 0x00000000..0x0001FFFF")
    return chunks
