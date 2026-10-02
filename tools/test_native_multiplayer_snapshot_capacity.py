#!/usr/bin/env python3
"""Verify exhausted native snapshot capacity stops gameplay and preserves the last save."""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import signal
import subprocess
import tempfile
import time


def run_fixture(binary, data, port, root):
    family = "snapshot-capacity"
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
                       '--multiplayer-smoke-native-snapshot-capacity']
            with (directory / 'game.log').open('w') as output:
                process = subprocess.Popen(command, cwd=directory, env=env, stdout=output,
                                           stderr=subprocess.STDOUT, start_new_session=True)
            processes.append((directory, process))
            if role == 'host':
                wait(directory, 'WAITING ON PORT', 15)
        host = root / 'host'
        wait(host, 'NATIVE_SNAPSHOT_CAPACITY_BASELINE_SAVE_READY', 45)

        def saved_files():
            result = {}
            for path in host.rglob('*'):
                parts = {part.upper() for part in path.parts}
                if path.is_file() and 'SAVEGAME' in parts and 'SLOT02' in parts:
                    result[str(path.relative_to(host))] = hashlib.sha256(path.read_bytes()).hexdigest()
            if not result or not any(path.upper().endswith('/SAVE.DAT') for path in result):
                raise RuntimeError(f'baseline native save not found: {result}')
            return result

        baseline = saved_files()
        (root / 'baseline-save-sha256.json').write_text(json.dumps(baseline, sort_keys=True, indent=2))
        wait(root / 'guest', 'NATIVE_SNAPSHOT_CAPACITY_PEER_READY native_capture=1', 20)
        (host / 'native-capacity-save-checked').write_text('hashed complete save\n')
        contents = {}
        for directory, process in processes:
            marker = ('NATIVE_SNAPSHOT_CAPACITY_PASS' if directory.name == 'host'
                      else 'NATIVE_SNAPSHOT_CAPACITY_PEER_STOPPED_PASS')
            contents[directory.name] = wait(directory, marker, 45)
            if process.wait(timeout=10) != 0:
                raise RuntimeError(f'{family} {directory.name} returned {process.returncode}:\n{contents[directory.name][-7000:]}')
        final = saved_files()
        (root / 'final-save-sha256.json').write_text(json.dumps(final, sort_keys=True, indent=2))
        if final != baseline:
            raise RuntimeError('last successful save changed after refused capacity save')
        if not re.search(r'NATIVE_SNAPSHOT_CAPACITY_PASS[^\n]*busy_retry=1 gameplay_blocked=1 status_stable=1 save_refused=1 world_unchanged=1 timer_control=1 simulation_frozen=1 pending_animation=1 load_allowed=1 timer_thawed=1 rest_stopped=1', contents['host']):
            raise RuntimeError('native capacity/busy/gameplay proof missing')
        if 'MULTIPLAYER STOPPED: WORLD CHECKPOINT EXCEEDS CAPACITY.' not in contents['host']:
            raise RuntimeError('explicit user-visible capacity status missing')
        print(f'NATIVE_MULTIPLAYER_SNAPSHOT_CAPACITY_PASS last_save_unchanged=1 files={len(baseline)} root={root}', flush=True)
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
    parser.add_argument('--port', type=int, default=64920)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535:
        parser.error('port range exceeds available TCP ports')
    root = pathlib.Path(tempfile.mkdtemp(prefix='fallout-native-snapshot-capacity-', dir='/var/tmp'))
    print(root, flush=True)
    # Pin the binary while other work may rebuild it.
    pinned_binary = root / 'fallout-ce-fixture'
    shutil.copy2(args.binary.resolve(), pinned_binary)
    run_fixture(pinned_binary, args.data_root.resolve(), args.port, root / 'fixture')


if __name__ == '__main__':
    main()
