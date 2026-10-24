#!/usr/bin/env python3
"""Install the personal APK and optionally transfer a verified original RomFS."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tarfile
import shlex
import build


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--serial', help='ADB serial/address; required when multiple devices are connected')
    ap.add_argument('--romfs', type=Path, help='Original, extracted Dread 2.1.0 RomFS')
    ap.add_argument('--no-launch', action='store_true')
    args = ap.parse_args()
    adb = ['adb'] + (['-s', args.serial] if args.serial else [])
    if args.romfs:
        print('Verifying RomFS against the upstream 2.1.0 manifest...', flush=True)
        rows = []
        checksums = []
        for line in (build.ROOT / 'dread/romfs_v210.manifest').read_text().splitlines():
            if not line or line.startswith('#'):
                continue
            name, size, sha = line.split('\t')
            p = args.romfs / name
            if not p.is_file() or p.stat().st_size != int(size):
                raise SystemExit(f'RomFS file missing or wrong size: {name}')
            h = hashlib.sha1()
            with p.open('rb') as f:
                for chunk in iter(lambda: f.read(1024 * 1024), b''):
                    h.update(chunk)
            if h.hexdigest() != sha:
                raise SystemExit(f'RomFS hash mismatch: {name}')
            rows.append(name)
            checksums.append(f'{sha}  {name}\n')
        print(f'Verified {len(rows)} files.', flush=True)
    build.run(adb + ['install', '-r', str(build.APK)])
    build.run(adb + ['shell', 'am', 'force-stop', build.PACKAGE])
    if args.romfs:
        target = 'files/MetroidDread_data/romfs'
        build.run(adb + ['shell', 'run-as', build.PACKAGE, 'mkdir', '-p', target])
        print('Transferring RomFS to the app private files...', flush=True)
        proc = subprocess.Popen(adb + ['shell', '-T', 'run-as', build.PACKAGE, 'tar', '-xf', '-', '-C', target], stdin=subprocess.PIPE)
        try:
            sent = 0
            next_report = 512 * 1024 * 1024
            with tarfile.open(fileobj=proc.stdin, mode='w|', format=tarfile.GNU_FORMAT) as archive:
                for name in rows:
                    archive.add(args.romfs / name, arcname=name, recursive=False)
                    sent += (args.romfs / name).stat().st_size
                    if sent >= next_report:
                        print(f'Transferred {sent / 2**30:.1f} GiB...', flush=True)
                        next_report += 512 * 1024 * 1024
        finally:
            proc.stdin.close()
        if proc.wait():
            raise SystemExit('RomFS transfer failed; rerun deployment before launching.')
        print('Checking every file on the device...', flush=True)
        command = shlex.quote(f'cd {target} && sha1sum -c -')
        result = subprocess.run(adb + ['shell', '-T', 'run-as', build.PACKAGE, 'sh', '-c', command],
                                input=''.join(checksums), text=True, capture_output=True)
        report = build.LOCAL / 'device-romfs-verify.log'
        report.write_text(result.stdout + result.stderr)
        if result.returncode or result.stdout.count(': OK') != len(rows):
            raise SystemExit(f'Device RomFS verification failed: {report}')
        print(f'Verified {len(rows)} device files.', flush=True)
    if not args.no_launch:
        build.run(adb + ['shell', 'am', 'start', '-n', build.PACKAGE + '/.LauncherActivity'])


if __name__ == '__main__':
    main()
