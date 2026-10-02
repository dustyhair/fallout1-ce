#!/usr/bin/env python3
"""Run bounded installed weapon attacks through native host combat and guest commands."""
import argparse
import os
import pathlib
import re
import shutil
import signal
import subprocess
import tempfile
import time

FAMILIES = ('smg', 'assault', 'minigun', 'grenade', 'rocket', 'laser', 'plasma',
            'flamer', 'laser-rifle', 'plasma-rifle')


def run_family(binary, data, family, port, root):
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
                       '--multiplayer-smoke-scenario=combat-attack',
                       f'--multiplayer-smoke-native-weapon-family={family}']
            with (directory / 'game.log').open('w') as output:
                process = subprocess.Popen(command, cwd=directory, env=env, stdout=output,
                                           stderr=subprocess.STDOUT, start_new_session=True)
            processes.append((directory, process))
            if role == 'host':
                wait(directory, 'WAITING ON PORT', 15)
        deadline = time.monotonic() + 85
        while time.monotonic() < deadline:
            if all('MULTIPLAYER_SMOKE_TEST_PASS' in log(d) for d, _ in processes):
                break
            for directory, process in processes:
                if process.poll() is not None and 'MULTIPLAYER_SMOKE_TEST_PASS' not in log(directory):
                    raise RuntimeError(f'{family} {directory.name}:\n{log(directory)[-7000:]}')
            time.sleep(0.1)
        contents = {}
        for directory, process in processes:
            contents[directory.name] = wait(directory, 'MULTIPLAYER_SMOKE_TEST_PASS', 2)
            if process.wait(timeout=10) != 0:
                raise RuntimeError(f'{family} {directory.name} returned {process.returncode}:\n{contents[directory.name][-7000:]}')
            if f'NATIVE_WEAPON_RESULT_PASS role={directory.name} family={family}' not in contents[directory.name]:
                raise RuntimeError(f'{family} has no {directory.name} resource proof')
            if 'NATIVE_WEAPON_COMMAND_FAIL' in contents[directory.name]:
                raise RuntimeError(f'{family} has a failed native command conservation check')
        digests = [re.findall(r'MULTIPLAYER_SMOKE_TEST_PASS[^\n]*digest=(\d+)', contents[r])[-1] for r in ('host', 'guest')]
        if digests[0] != digests[1]:
            raise RuntimeError(f'{family} final digests differ: {digests}')
        if not re.search(r'MULTIPLAYER_SMOKE_TEST_PASS role=guest[^\n]*scripts=0 attacks=0 rng=0', contents['guest']):
            raise RuntimeError(f'{family} guest executed native rules')
        attacks = re.findall(r'NATIVE_WEAPON_COMMAND_PASS[^\n]*owner=(host|guest) attack=1[^\n]*', contents['host'])
        if sorted(attacks) != ['guest', 'host']:
            raise RuntimeError(f'{family} requires one native attack from each player, got {attacks}')
        print(f'NATIVE_MULTIPLAYER_WEAPON_PASS family={family} digest={digests[0]} guest_rules=0 root={root}', flush=True)
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
    parser.add_argument('--family', choices=FAMILIES, action='append', help='Repeat to select families; default runs all.')
    parser.add_argument('--port', type=int, default=64900)
    args = parser.parse_args()
    families = args.family or FAMILIES
    if not 1024 <= args.port <= 65535 - len(families):
        parser.error('port range exceeds available TCP ports')
    root = pathlib.Path(tempfile.mkdtemp(prefix='fallout-native-weapons-', dir='/var/tmp'))
    print(root, flush=True)
    # Pin one binary for the entire matrix while other work may rebuild it.
    pinned_binary = root / 'fallout-ce-fixture'
    shutil.copy2(args.binary.resolve(), pinned_binary)
    for index, family in enumerate(families):
        run_family(pinned_binary, args.data_root.resolve(), family, args.port + index, root / f'{index:02d}-{family}')
    print(f'NATIVE_MULTIPLAYER_WEAPONS_PASS families={len(families)} root={root}', flush=True)


if __name__ == '__main__':
    main()
