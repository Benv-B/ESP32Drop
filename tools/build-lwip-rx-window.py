"""Build an opt-in C5 TCP receive-window experiment against the pinned SDK.

The installed SDK is never modified. Rebuild every lwIP translation unit using
TCP_WND / DEFAULT_TCP_RECVMBOX_SIZE, then link those objects before liblwip.a.
Keep window scaling and all ABI-affecting settings identical to the SDK.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parent.parent
SDK = ROOT / '.build-tools/arduino-data/packages/m5stack/tools/esp32c5-libs/3.3.8'
COMMIT = 'fd432e4ee2cfb7f7f1c7eb7227e0173412e7b84e'
ZIP_SHA256 = '6a1cb6d2f829134ecc413ad392077aecdc1b4f220d66c98b4b2bd8fe850791a4'
CACHE = ROOT / '.build-tools/lwip-rx-window'
SOURCES = ('core/init.c', 'core/tcp.c', 'core/tcp_in.c', 'core/tcp_out.c', 'api/api_msg.c')


def run(args):
    try:
        return subprocess.check_output([str(x) for x in args], text=True, stderr=subprocess.STDOUT)
    except subprocess.CalledProcessError as error:
        raise RuntimeError(error.output) from error


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--window', type=int, choices=(5760, 16384), required=True)
    ap.add_argument('--verify-map', type=Path)
    args = ap.parse_args()
    if 'esp-idf: v5.5.4 735507283d' not in (SDK / 'versions.txt').read_text():
        raise RuntimeError('Expected the pinned IDF 5.5.4 build; refusing a mixed SDK')
    if args.verify_map:
        manifest = json.loads((CACHE / ('wnd' + str(args.window)) / 'manifest.json').read_text())
        mapping = args.verify_map.read_text(encoding='utf-8', errors='replace').replace('\\', '/')
        for filename in manifest['objects']:
            obj = Path(filename)
            if 'LOAD ' + obj.as_posix() not in mapping:
                raise RuntimeError('Profile object missing from link: ' + str(obj))
            if 'liblwip.a(' + obj.stem + '.c.obj)' in mapping:
                raise RuntimeError('Stock lwIP object still linked: ' + obj.stem)
        print('lwIP receive-window profile link verified')
        return
    source = CACHE / COMMIT
    # Always validate the archive, including cached runs. No unverified extracted
    # source is used merely because a previous directory exists.
    archive = CACHE / (COMMIT + '.zip')
    CACHE.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        url = 'https://codeload.github.com/espressif/esp-lwip/zip/' + COMMIT
        archive.write_bytes(urllib.request.urlopen(url, timeout=30).read())
    blob = archive.read_bytes()
    if hashlib.sha256(blob).hexdigest() != ZIP_SHA256:
        raise RuntimeError('Pinned lwIP source archive hash mismatch')
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        for item in z.infolist():
            rel = Path(*Path(item.filename).parts[1:])
            if item.is_dir():
                continue
            if rel.is_absolute() or '..' in rel.parts:
                raise RuntimeError('Unsafe archive path')
            dest = source / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(z.read(item))
    source_headers = source / 'src/include'
    sdk_headers = SDK / 'include/lwip/lwip/src/include'
    checked = 0
    for header in source_headers.rglob('*.h'):
        installed = sdk_headers / header.relative_to(source_headers)
        if not installed.exists() or header.read_bytes().replace(b'\r\n', b'\n') != installed.read_bytes().replace(b'\r\n', b'\n'):
            raise RuntimeError('Source/SDK header mismatch: ' + str(header))
        checked += 1
    out = CACHE / ('wnd' + str(args.window))
    out.mkdir(parents=True, exist_ok=True)
    mailbox = 6 if args.window == 5760 else 16
    override = out / 'rx_window_config.h'
    override.write_text(
        '#include "sdkconfig.h"\n'
        '#undef CONFIG_LWIP_TCP_WND_DEFAULT\n'
        f'#define CONFIG_LWIP_TCP_WND_DEFAULT {args.window}\n'
        '#undef CONFIG_LWIP_TCP_RECVMBOX_SIZE\n'
        f'#define CONFIG_LWIP_TCP_RECVMBOX_SIZE {mailbox}\n', encoding='utf-8')
    toolchain = ROOT / '.build-tools/rv32/bin'
    gcc = toolchain / 'riscv32-esp-elf-gcc.exe'
    nm = toolchain / 'riscv32-esp-elf-nm.exe'
    common = [gcc, '@' + str(SDK / 'flags/c_flags'), '@' + str(SDK / 'flags/defines'),
              '-iprefix', str(SDK / 'include') + '/', '@' + str(SDK / 'flags/includes'),
              '-I' + str(SDK / 'qio_qspi/include'), '-Os', '-c']
    # Compare the layouts used across the prebuilt / rebuilt object boundary.
    abi = out / 'abi.c'
    abi.write_text('#include <lwip/tcp.h>\n#include <lwip/api.h>\n'
                   '#include <lwip/priv/sockets_priv.h>\n'
                   'char abi_tcp[sizeof(struct tcp_pcb)];\n'
                   'char abi_conn[sizeof(struct netconn)];\n'
                   'char abi_sock[sizeof(struct lwip_sock)];\n'
                   'char abi_pbuf[sizeof(struct pbuf)];\n', encoding='utf-8')
    layouts = []
    for label, extra in [('sdk', []), ('profile', ['-include', override])]:
        obj = out / ('abi_' + label + '.o')
        run(common + extra + [abi, '-o', obj])
        layouts.append(sorted(line.split()[1:] for line in run([nm, '-S', obj]).splitlines() if ' abi_' in line))
    if len(layouts[0]) != 4 or len(layouts[1]) != 4:
        raise RuntimeError('Missing SDK layout probes in compiler output')
    if layouts[0] != layouts[1]:
        raise RuntimeError('TCP receive-window profile changed an SDK structure layout')
    objects = []
    for rel in SOURCES:
        obj = out / (Path(rel).stem + '.o')
        run(common + ['-include', override, source / 'src' / rel, '-o', obj])
        objects.append(obj)
    marker = out / 'rx_window_marker.c'
    marker.write_text('#include <lwip/opt.h>\n'
                      'unsigned photo_frame_tcp_window(void) { return TCP_WND; }\n'
                      'unsigned photo_frame_tcp_mailbox(void) { return DEFAULT_TCP_RECVMBOX_SIZE; }\n', encoding='utf-8')
    obj = out / 'rx_window_marker.o'
    run(common + ['-include', override, marker, '-o', obj])
    objects.append(obj)
    manifest = {'idf': '735507283d', 'lwip': COMMIT, 'archive_sha256': ZIP_SHA256,
                'matching_headers': checked, 'window': args.window, 'mailbox': mailbox,
                'abi_match': True, 'objects': [str(p.resolve()) for p in objects]}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    # A response file avoids shell quoting and MAX_PATH command-line growth.
    response = out / 'link.rsp'
    response.write_text(''.join('"' + p.resolve().as_posix() + '"\n' for p in objects), encoding='utf-8')
    print(json.dumps(manifest))


if __name__ == '__main__':
    main()
