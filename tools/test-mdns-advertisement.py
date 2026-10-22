"""Decode packets built by the actual port encoder; ensure no live closed ports."""
import argparse
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def decode(packet):
    header = struct.unpack_from('!6H', packet)
    assert header[:3] == (0, 0x8400, 0) and header[4:] == (0, 0)
    p = 12

    def name():
        nonlocal p
        labels = []
        while packet[p]:
            size = packet[p]
            assert size <= 63 and p + 1 + size < len(packet)
            p += 1
            labels.append(packet[p:p + size].decode())
            p += size
        p += 1
        return '.'.join(labels)

    records = []
    for _ in range(header[3]):
        owner = name()
        kind, cls, ttl, size = struct.unpack_from('!HHIH', packet, p)
        p += 10
        assert p + size <= len(packet)
        records.append((owner, kind, cls, ttl, packet[p:p + size]))
        p += size
    assert p == len(packet)
    return records


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx', default='clang++')
    args = parser.parse_args()
    source = (ROOT / 'ESP32Drop/src/airdrop/port/ad_port_esp32.cpp').read_text(encoding='utf-8')
    functions = []
    for name in ('put_dnsname', 'put_pair_goodbye', 'build_mdns_msg'):
        found = re.findall(r'^static [^\n]+\b' + name + r'\([^)]*\) \{.*?^\}', source, re.M | re.S)
        if len(found) != 1:
            raise RuntimeError('Expected one definition: ' + name)
        functions.append(found[0])
    harness = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include "ESP32DropConfig.h"
#include "airdrop/core/ad_dnssd.h"
#define AIRDROP_DNM "M5 Badge"
static struct {
  char instance[13]; char pair_sid[37]; uint8_t ll[16];
} g_id = {"12bda3c97a61", "12BDA3C9-7A61-4D5E-8F00-12BDA3C97A61", {0xfe,0x80}};
/* FUNCTIONS */
int main(int argc, char **) {
  uint8_t buf[1400]; memset(buf, 0xa5, sizeof buf);
  int n = build_mdns_msg(buf, argc > 1);
  if (n > 1280 || n <= 12) return 2;
  for (unsigned i = unsigned(n); i < sizeof buf; ++i) if (buf[i] != 0xa5) return 3;
  _setmode(_fileno(stdout), _O_BINARY);
  return fwrite(buf, 1, n, stdout) == unsigned(n) ? 0 : 4;
}
'''
    out = ROOT / '.build-host-tests'
    out.mkdir(exist_ok=True)
    src, exe = out / 'mdns_encoder.cpp', out / 'mdns_encoder.exe'
    src.write_text(harness.replace('/* FUNCTIONS */', '\n'.join(functions)), encoding='utf-8')
    subprocess.run([args.cxx, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-DAWDL_AIRDROP_NO_IRIS=1', '-I', str(ROOT / 'ESP32Drop/src'),
                    str(src), '-o', str(exe)], check=True)
    normal = subprocess.check_output([str(exe)])
    retiring = subprocess.check_output([str(exe), 'retire'])
    live, migration = decode(normal), decode(retiring)
    assert len(live) == 6 and len(migration) == 12
    assert migration[:6] == live and all(r[3] > 0 for r in live)
    assert all('Pair' not in r[0] for r in live)
    services = [r for r in live if r[1] == 33]
    assert len(services) == 1 and struct.unpack_from('!H', services[0][4], 4)[0] == 8770
    assert any(r[1] == 16 and r[4] == b'\x09flags=955' for r in live)
    goodbye = migration[6:]
    assert all(r[3] == 0 for r in goodbye)
    assert [r[1] for r in goodbye] == [12, 33, 16, 12, 33, 16]
    assert [struct.unpack_from('!H', r[4], 4)[0] for r in goodbye if r[1] == 33] == [54763, 54764]
    print(f'PASS: {len(normal)} B normal message has six live records, only port 8770')
    print(f'PASS: {len(retiring)} B migration message preserves live records and withdraws all six pairing records with TTL zero')
    print('PASS: answer counts, names, bounds, TXT capability and retired ports verified')


if __name__ == '__main__':
    main()
