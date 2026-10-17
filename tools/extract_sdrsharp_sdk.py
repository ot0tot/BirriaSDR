#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Extract three build references from the user's installed SDR# .NET bundle.

The assemblies remain Airspy's files. They are not redistributed with this
project. Reading the bundle also avoids mixing a .NET 9 SDK with a .NET 8 app.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

REFERENCES = {'SDRSharp.Radio.dll', 'SDRSharp.Common.dll', 'SDRSharp.PanView.dll'}
SIGNATURE = bytes.fromhex('8b1202b96a612038727b930214d7a03213f5b9e6efae3318ee3b2dce24b36aae')


def extract(application, output):
    data = application.read_bytes()
    marker = data.find(SIGNATURE)
    if marker < 8:
        raise ValueError('No .NET single-file bundle signature')
    cursor = struct.unpack_from('<Q', data, marker-8)[0]

    def unpack(fmt):
        nonlocal cursor
        values = struct.unpack_from(fmt, data, cursor)
        cursor += struct.calcsize(fmt)
        return values

    def string():
        nonlocal cursor
        size = 0
        for shift in range(0, 35, 7):
            byte, = unpack('<B')
            size |= (byte & 127) << shift
            if not byte & 128:
                break
        else:
            raise ValueError('Invalid bundle string length')
        end = cursor + size
        if end > len(data):
            raise ValueError('Truncated bundle string')
        text = data[cursor:end].decode('utf-8')
        cursor = end
        return text

    major, minor, count = unpack('<III')
    if major != 6 or count > 10000:
        raise ValueError(f'Unsupported bundle manifest {major}.{minor}')
    identity = string()
    unpack('<qqqqQ')
    references = {}
    for _ in range(count):
        offset, size, compressed, _kind = unpack('<qqqB')
        name = string()
        if name not in REFERENCES:
            continue
        length = compressed or size
        if min(offset, size, compressed) < 0 or offset+length > len(data) or size > 64*1024*1024:
            raise ValueError('Invalid reference bounds')
        blob = data[offset:offset+length]
        if compressed:
            blob = zlib.decompress(blob, -15)
        if len(blob) != size or name in references:
            raise ValueError('Invalid or duplicate reference')
        references[name] = blob
    if references.keys() != REFERENCES:
        raise ValueError('The bundle is missing SDR# reference assemblies')
    output.mkdir(parents=True, exist_ok=True)
    for name, blob in references.items():
        (output/name).write_bytes(blob)
    manifest = dict(application=str(application.resolve()),
                    application_sha256=hashlib.sha256(data).hexdigest(), bundle=identity,
                    references={name:hashlib.sha256(blob).hexdigest() for name, blob in references.items()})
    (output/'references.json').write_text(json.dumps(manifest, indent=2)+'\n')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('application', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    print(json.dumps(extract(args.application, args.output)))
