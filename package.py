"""Package locally built runtime using a strict static-file allowlist."""
import argparse
import hashlib
import re
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNTIME = ('index.html', 'style.css', 'game.js', 'game-data.js')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--docker', action='store_true', help='Include Docker build configuration')
    args = parser.parse_args()
    try:
        files = {name: (HERE / name).read_bytes() for name in RUNTIME}
    except FileNotFoundError:
        parser.exit(1, 'Build the game first: npm ci, prepare local assets, then python build.py\n')
    html = files['index.html'].decode('utf-8')
    for name in RUNTIME[1:]:
        version = hashlib.sha256(files[name]).hexdigest()[:16]
        html = html.replace('"' + name + '"', '"' + name + '?v=' + version + '"')
    html = re.sub(r'data-runtime-size="\d+"', f'data-runtime-size="{len(files["game-data.js"])}"', html)
    files['index.html'] = html.encode('utf-8')
    if args.docker:
        for name in ('Dockerfile', '.dockerignore', 'compose.yaml', 'docker/nginx.conf'):
            files[name] = (HERE / name).read_bytes()
        files['README.md'] = b'Run docker compose up -d --build; open http://localhost:8080\n'
        filename = 'magic-tower-docker.zip'
    else:
        files['_headers'] = b'/*\n  Cache-Control: public, max-age=0, must-revalidate\n  X-Content-Type-Options: nosniff\n'
        files['404.html'] = b'<!doctype html><html><meta charset="utf-8"><title>404</title><p>Not found. <a href="/">Magic Tower</a></p></html>'
        filename = 'magic-tower-cloudflare-pages.zip'
    output_dir = HERE / 'dist'
    output_dir.mkdir(exist_ok=True)
    output = output_dir / filename
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in files.items():
            archive.writestr(name, data)
    with zipfile.ZipFile(output) as archive:
        assert set(archive.namelist()) == set(files)
        assert archive.testzip() is None
        assert all(archive.read(name) == data for name, data in files.items())
    print(f'Created {output.name}: {output.stat().st_size} bytes, {len(files)} allowlisted files in dist/')


if __name__ == '__main__':
    main()
