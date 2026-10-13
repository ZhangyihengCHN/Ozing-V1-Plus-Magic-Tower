"""Build native WebAssembly and the framework-free Web host from local resources."""
import argparse
import base64
import gzip
import json
import os
import re
import shutil
import subprocess
from pathlib import Path
from prepare_assets import validate_assets

HERE = Path(__file__).resolve().parent
NATIVE = HERE / 'native'
EXPORTS = ['emu_init', 'emu_key', 'emu_timer', 'emu_paint', 'emu_mode', 'emu_selection',
           'emu_dialog_active', 'emu_floor', 'emu_x', 'emu_y', 'emu_hp', 'emu_atk', 'emu_def',
           'emu_framebuffer', 'emu_last_error', 'emu_last_error_pc', 'emu_save_size',
           'emu_save_generation', 'emu_save_export', 'emu_save_import', 'web_scratch',
           'web_timer_id', 'web_timer_interval', 'web_checkpoint', 'web_tick', 'web_busy',
           'web_touch', 'web_combat_active', 'web_book', 'web_can_checkpoint',
           'web_can_book', 'web_book_active', 'web_can_fly', 'web_fly', 'web_item_owned',
           'web_item_count', 'web_can_item', 'web_item_use']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clang', default=os.environ.get('CLANG') or shutil.which('clang'),
                        help='Clang with wasm32 + wasm-ld support; alternatively set CLANG')
    args = parser.parse_args()
    if not args.clang:
        parser.error('Clang with wasm32 support is required; pass --clang /path/to/clang')
    node = shutil.which('node')
    if not node:
        parser.error('Node.js is required; install Node.js and run npm ci first')
    validate_assets(HERE)
    cmd = [args.clang, '--target=wasm32', '-O2', '-nostdlib', '-fno-builtin',
           '-I' + str(NATIVE / 'include'), '-I' + str(NATIVE),
           str(NATIVE / 'emu_core.c'), str(NATIVE / 'profile_magic_tower.c'),
           str(NATIVE / 'web_bridge.c'), str(NATIVE / 'assets.s'),
           '-Wl,--no-entry', '-Wl,--strip-all', '-Wl,--stack-first', '-Wl,-z,stack-size=1048576',
           '-Wl,--initial-memory=142606336', '-Wl,--max-memory=142606336',
           *['-Wl,--export=' + name for name in EXPORTS], '-o', str(HERE / 'game.wasm')]
    subprocess.run(cmd, check=True, cwd=HERE)
    wasm = (HERE / 'game.wasm').read_bytes()
    packed = {'format': 'gzip', 'bytes': len(wasm),
              'data': base64.b64encode(gzip.compress(wasm, compresslevel=9, mtime=0)).decode('ascii')}
    payload = 'window.MAGIC_TOWER_PACK=' + json.dumps(packed, separators=(',', ':')) + ';\n'
    (HERE / 'game-data.js').write_bytes(payload.encode('ascii'))
    index = HERE / 'index.html'
    index.write_text(re.sub(r'data-runtime-size="\d+"', f'data-runtime-size="{len(payload)}"',
                            index.read_text(encoding='utf-8')), encoding='utf-8', newline='\n')
    subprocess.run([node, str(HERE / 'build-web.mjs')], check=True, cwd=HERE)
    print(f'Built game.wasm ({len(wasm)} bytes) and the four static runtime files')


if __name__ == '__main__':
    main()
