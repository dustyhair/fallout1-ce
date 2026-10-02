#!/usr/bin/env python3
"""Verify native script-created objects survive TLS checkpoint application and reconnect."""
import argparse
import os
import pathlib
import re
import shutil
import signal
import subprocess
import tempfile
import time


def run_fixture(binary, data, port, root):
    family = "world-discovery"
    root.mkdir()
    processes = []
    env = dict(os.environ, TMPDIR='/var/tmp', SDL_AUDIODRIVER='dummy', SDL_RENDER_DRIVER='software')

    def log(directory):
        return (directory / 'game.log').read_text(errors='replace')

    def wait(directory, marker, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            content = log(directory)
            if marker in content:
                return content
            process = next(p for d, p in processes if d == directory)
            if process.poll() is not None:
                raise RuntimeError(f'{family} {directory.name} exited before {marker}:\n{content[-7000:]}')
            time.sleep(0.1)
        raise RuntimeError(f'{family} {directory.name} timed out before {marker}:\n{log(directory)[-7000:]}')

    try:
        for role in ('host', 'guest'):
            directory = root / role
            directory.mkdir()
            shutil.copy2(binary, directory / 'fallout-ce')
            shutil.copytree(data / 'data', directory / 'DATA')
            for archive in ('master.dat', 'critter.dat'):
                (directory / archive).symlink_to(data / archive)
            config = (data / 'fallout.cfg').read_text()
            config = re.sub(r'(?m)^(master_patches|critter_patches)\s*=.*$', r'\1=DATA', config)
            config = re.sub(r'(?m)^(width|height)\s*=.*$',
                            lambda m: f'{m[1]}={640 if m[1] == "width" else 480}', config)
            (directory / 'fallout.cfg').write_text(config.replace('enabled=1', 'enabled=0'))
            endpoint = f'--multiplayer-host={port}' if role == 'host' else f'--multiplayer-join=127.0.0.1:{port}'
            command = ['timeout', '--kill-after=5s', '90s', 'xvfb-run', '-a', '--server-args=-screen 0 640x480x24',
                       './fallout-ce', endpoint, '--multiplayer-smoke-test',
                       '--multiplayer-smoke-native-world-discovery']
            with (directory / 'game.log').open('w') as output:
                process = subprocess.Popen(command, cwd=directory, env=env, stdout=output,
                                           stderr=subprocess.STDOUT, start_new_session=True)
            processes.append((directory, process))
            if role == 'host':
                wait(directory, 'WAITING ON PORT', 15)
        deadline = time.monotonic() + 85
        while time.monotonic() < deadline:
            if all('NATIVE_WORLD_DISCOVERY_TLS_PASS' in log(d) for d, _ in processes):
                break
            for directory, process in processes:
                if process.poll() is not None and 'NATIVE_WORLD_DISCOVERY_TLS_PASS' not in log(directory):
                    raise RuntimeError(f'{family} {directory.name}:\n{log(directory)[-7000:]}')
            time.sleep(0.1)
        contents = {}
        for directory, process in processes:
            contents[directory.name] = wait(directory, 'NATIVE_WORLD_DISCOVERY_TLS_PASS', 2)
            if process.wait(timeout=10) != 0:
                raise RuntimeError(f'{family} {directory.name} returned {process.returncode}:\n{contents[directory.name][-7000:]}')
        digests = [re.findall(r'NATIVE_WORLD_DISCOVERY_TLS_PASS[^\n]*digest=(\d+)', contents[r])[-1] for r in ('host', 'guest')]
        if digests[0] != digests[1]:
            raise RuntimeError(f'final native digests differ: {digests}')
        guest = contents['guest']
        for marker in ('NATIVE_WORLD_DISCOVERY_APPLIED role=guest',
                       'NATIVE_WORLD_DISCOVERY_ERASED_BEFORE_RECONNECT role=guest',
                       'NATIVE_WORLD_DISCOVERY_RECONSTRUCTED role=guest'):
            if marker not in guest:
                raise RuntimeError(f'missing native reconstruction proof: {marker}')
        if not re.search(r'NATIVE_WORLD_DISCOVERY_TLS_PASS role=guest[^\n]*scripts=0 attacks=0 rng=0', guest):
            raise RuntimeError('guest executed native rules')
        for role in ('host', 'guest'):
            if not re.search(r'NATIVE_WORLD_DISCOVERY_TLS_PASS role=' + role
                             + r'[^\n]*reconnect=1 native_apply=1 resources=3 timer_exact=1', contents[role]):
                raise RuntimeError(f'missing {role} timer/resource proof')
        print(f'NATIVE_MULTIPLAYER_WORLD_DISCOVERY_PASS digest={digests[0]} guest_rules=0 root={root}', flush=True)
    finally:
        for _, process in processes:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=pathlib.Path)
    parser.add_argument('data_root', type=pathlib.Path)
    parser.add_argument('--port', type=int, default=64900)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535:
        parser.error('port range exceeds available TCP ports')
    root = pathlib.Path(tempfile.mkdtemp(prefix='fallout-native-world-discovery-', dir='/var/tmp'))
    print(root, flush=True)
    # Pin the binary while other work may rebuild it.
    pinned_binary = root / 'fallout-ce-fixture'
    shutil.copy2(args.binary.resolve(), pinned_binary)
    run_fixture(pinned_binary, args.data_root.resolve(), args.port, root / 'fixture')


if __name__ == '__main__':
    main()
