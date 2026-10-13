"""Prepare locally supplied original resources; never download firmware."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent


def validate(files):
    manifest = json.loads((HERE / 'extraction.json').read_text(encoding='utf-8'))
    for name, expected in manifest.items():
        data = files[name]
        if len(data) != expected['bytes'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
            raise ValueError(f'{name}.bin does not match the supported firmware/resource version')
    return files


def from_firmware(folder):
    firmware = (folder / 'V1PU.bin').read_bytes()
    expected = json.loads((HERE / 'extraction.json').read_text(encoding='utf-8'))['fw']
    if hashlib.sha256(firmware).hexdigest() != expected['sha256']:
        raise ValueError('V1PU.bin does not match the supported firmware version')
    files = {'fw': firmware}
    with (folder / 'V1PRU.bin').open('rb') as stream:
        stream.seek(0, 2)
        total = stream.tell()

        def resource(rid):
            logical, length = struct.unpack_from('<II', firmware, 0x3aa99c + 0x48 + rid * 8)
            offset = logical - 0xa00000 + 0x1c
            if offset < 0 or length == 0 or offset + length > total:
                raise ValueError(f'V1PRU.bin is missing resource 0x{rid:x}')
            stream.seek(offset)
            data = stream.read(length)
            if len(data) != length:
                raise ValueError(f'Truncated resource 0x{rid:x}')
            return data

        for name, rid in {'font_ascii12': 0x100, 'font96': 0x101,
                          'font_cjk12': 0x106, 'font100': 0x107,
                          'map0280': 0x280, 'dlg0281': 0x281}.items():
            files[name] = resource(rid)
        bitmaps = resource(0x176)
        if len(bitmaps) < 56:
            raise ValueError('Truncated bitmap offset table')
        offsets = struct.unpack_from('<14I', bitmaps)
        for index, offset in enumerate(offsets):
            if offset < 56 or offset + 14 > len(bitmaps) or bitmaps[offset:offset+2] != b'BM':
                raise ValueError(f'Invalid bitmap {index}')
            size = struct.unpack_from('<I', bitmaps, offset + 2)[0]
            if size < 54 or offset + size > len(bitmaps):
                raise ValueError(f'Truncated bitmap {index}')
            files[f'bmp{index:02}'] = bitmaps[offset:offset + size]
    return validate(files)


def validate_assets(root):
    names = json.loads((root / 'extraction.json').read_text(encoding='utf-8'))
    try:
        validate({name: (root / 'assets' / (name + '.bin')).read_bytes() for name in names})
    except (OSError, ValueError) as error:
        raise SystemExit(f'Local resources are missing or unsupported: {error}\n'
                         'Run python prepare-assets.py --firmware-dir /path/to/your/firmware first.') from None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--firmware-dir', type=Path, help='Directory with local V1PU.bin and V1PRU.bin')
    source.add_argument('--from-dir', type=Path, help='Directory with the already extracted .bin files')
    args = parser.parse_args()
    try:
        if args.firmware_dir:
            files = from_firmware(args.firmware_dir)
        else:
            names = json.loads((HERE / 'extraction.json').read_text(encoding='utf-8'))
            files = validate({name: (args.from_dir / (name + '.bin')).read_bytes() for name in names})
        output = HERE / 'assets'
        output.mkdir(exist_ok=True)
        for name, data in files.items():
            (output / (name + '.bin')).write_bytes(data)
        print(f'Validated and prepared {len(files)} local resources in assets/')
    except (OSError, ValueError, KeyError, struct.error) as error:
        parser.exit(1, f'Resource preparation failed: {error}\n')


if __name__ == '__main__':
    main()
