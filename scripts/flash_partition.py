#!/usr/bin/env python3
"""Resolve a named partition from a read-back ESP32 partition table; validate MD5."""
import argparse
import hashlib
import struct
from pathlib import Path


def find_partition(blob, name, subtype=0x81):
    """Default subtype 0x81 is FAT (video/recording); 0x82 is SPIFFS (ESP-SR model)."""
    found = None
    md5_ok = False
    spans = []
    for pos in range(0, len(blob) - 31, 32):
        entry = blob[pos:pos + 32]
        if entry[:2] == b'\xeb\xeb':
            if entry[16:] != hashlib.md5(blob[:pos]).digest():
                raise ValueError('partition table MD5 mismatch')
            md5_ok = True
            break
        if entry[:2] != b'\xaa\x50':
            break
        _, typ, sub, offset, size, label, flags = struct.unpack('<HBBII16sI', entry)
        if not size or offset < 0x9000 or offset + size > 0x2000000:
            raise ValueError('partition outside 32 MB flash')
        if any(offset < end and start < offset + size for start, end in spans):
            raise ValueError('overlapping partitions')
        spans.append((offset, offset + size))
        if label.rstrip(b'\0').decode('ascii') == name:
            if found is not None or (typ, sub) != (1, subtype) or flags:
                raise ValueError(f'expected one unencrypted data partition of subtype {subtype:#x}')
            found = offset, size
    if not md5_ok or found is None:
        raise ValueError('missing MD5 or partition')
    return found


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('table')
    ap.add_argument('--name', default='ffat')
    a = ap.parse_args()
    offset, size = find_partition(Path(a.table).read_bytes(), a.name)
    print(hex(offset), size)
