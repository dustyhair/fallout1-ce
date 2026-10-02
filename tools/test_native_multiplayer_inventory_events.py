#!/usr/bin/env python3
"""Check native inventory event rejection, ownership and clean host/guest shutdown."""
import argparse
import os
import pathlib
import re
import shutil
import signal
import socket
import subprocess
import tempfile
import time

import test_native_multiplayer_snapshot_creation as native

def run_fixture(binary, data, port, root, mode, expect_negative):
    processes = []
    env = dict(os.environ, SDL_AUDIODRIVER='dummy', SDL_RENDER_DRIVER='software', FALLOUT_TTS_ENABLED='0', FALLOUT_INVENTORY_EVENT_BOUNDARY=mode)
    if expect_negative:
        env['FALLOUT_INVENTORY_EVENT_EXPECT_NEGATIVE'] = '1'

    def read_log(directory):
        return (directory / 'game.log').read_text(errors='replace')

    try:
        for role in ('host', 'guest'):
            directory = root / role
            directory.mkdir()
            shutil.copy2(binary, directory / 'fallout-ce')
            shutil.copytree(native.installed_file(data, 'data'), directory / 'DATA')
            for name in ('master.dat', 'critter.dat'):
                archive = native.installed_file(data, name).resolve()
                (directory / name).symlink_to(archive)
                (directory / name.upper()).symlink_to(archive)
            config = native.installed_file(data, 'fallout.cfg').read_text()
            config = re.sub(r'(?m)^(master_patches|critter_patches)\s*=.*$', r'\1=DATA', config)
            config = re.sub(r'(?m)^(width|height)\s*=.*$',
                            lambda match: f'{match[1]}={640 if match[1] == "width" else 480}', config)
            config = re.sub(r'(?m)^enabled\s*=\s*1\s*$', 'enabled=0', config)
            (directory / 'fallout.cfg').write_text(config)
            endpoint = (f'--multiplayer-host={port}' if role == 'host'
                        else f'--multiplayer-join=127.0.0.1:{port}')
            with (directory / 'game.log').open('w') as output:
                process = subprocess.Popen(
                    ['xvfb-run', '-a', '--server-args=-screen 0 640x480x24',
                     './fallout-ce', endpoint, '--multiplayer-smoke-test'],
                    cwd=directory, env=env, stdout=output, stderr=subprocess.STDOUT,
                    start_new_session=True)
            processes.append((directory, process))
            if role == 'host':
                deadline = time.monotonic() + 20
                while 'WAITING ON PORT' not in read_log(directory):
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError(f'host did not become ready:\n{read_log(directory)[-7000:]}')
                    time.sleep(0.1)
        deadline = time.monotonic() + 100
        while any(process.poll() is None for _, process in processes):
            if time.monotonic() >= deadline:
                raise RuntimeError('native host/guest fixture timed out')
            time.sleep(0.1)
        exits = {directory.name: process.returncode for directory, process in processes}
        logs = {directory.name: read_log(directory) for directory, _ in processes}
        guest = logs['guest']
        line = next((line for line in guest.splitlines() if line.startswith(('NATIVE_TRANSFER_BOUNDARY_CONTROL ', 'NATIVE_INVENTORY_EVENT_POSITIVE '))), '')
        print(line or 'NATIVE_TRANSFER_BOUNDARY_CONTROL_MISSING', flush=True)
        print(f'NATIVE_TRANSFER_BOUNDARY_EXITS host={exits["host"]} guest={exits["guest"]}', flush=True)
        expected = 1 if expect_negative else 0
        if exits != {'host': expected, 'guest': expected}:
            raise RuntimeError(f'expected explicit exits{expected}/{expected}:\n{guest[-7000:]}')
        marker = (f'NATIVE_TRANSFER_BOUNDARY_NEGATIVE_CONFIRMED mode={mode}' if expect_negative
                  else f'NATIVE_INVENTORY_EVENT_REJECTION mode={mode} preserved=1')
        if mode in ('missing-transfer', 'missing-drop'):
            marker = f'NATIVE_INVENTORY_EVENT_POSITIVE mode={mode} codec=1 applied=1 exact=1 replay=1 counts=1 restored=1'
        if marker not in guest:
            raise RuntimeError(f'boundary control did not pass:\n{guest[-7000:]}')
        if not expect_negative and any('MULTIPLAYER_SMOKE_TEST_PASS ' not in log for log in logs.values()):
            raise RuntimeError('normal multiplayer smoke continuation is missing')
        print(f'NATIVE_INVENTORY_EVENTS_PASS mode={mode} exits={expected}/{expected}', flush=True)
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
    parser.add_argument('build_directory', type=pathlib.Path)
    parser.add_argument('data_root', type=pathlib.Path)
    parser.add_argument('--source-root', type=pathlib.Path)
    parser.add_argument('--mode', choices=('remainder', 'missing', 'cycle', 'missing-transfer', 'missing-drop', 'all'), default='all')
    parser.add_argument('--expect-negative', action='store_true')
    parser.add_argument('--port', type=int, default=0)
    args = parser.parse_args()
    if args.port != 0 and not 1024 <= args.port <= 65531:
        parser.error('port must be 0 or between 1024 and 65531')
    if args.expect_negative and args.mode not in ('remainder', 'cycle'):
        parser.error('clean old-code negatives support remainder or cycle only')
    checkout = pathlib.Path(__file__).resolve().parents[1]
    source = (args.source_root or checkout).resolve()
    root = pathlib.Path(tempfile.mkdtemp(prefix='fallout-native-inventory-events-', dir='/var/tmp'))
    print(f'NATIVE_INVENTORY_EVENTS_ARTIFACTS {root}', flush=True)
    native.FIXTURES = {
        'src/multiplayer/network_world.cc': 'native_inventory_events_world.cc',
        'src/game/object.cc': 'native_snapshot_creation_objects.cc',
        'src/game/scripts.cc': 'native_snapshot_creation_scripts.cc',
    }
    binary = native.compile_fixture(args.build_directory.resolve(), source, checkout / 'tests', root)
    modes = ('remainder', 'missing', 'cycle', 'missing-transfer', 'missing-drop') if args.mode == 'all' else (args.mode,)
    for index, mode in enumerate(modes):
        port = args.port + index if args.port else 0
        if port == 0:
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                port = reservation.getsockname()[1]
        directory = root / mode
        directory.mkdir()
        run_fixture(binary, args.data_root.resolve(), port, directory, mode, args.expect_negative)


if __name__ == '__main__':
    main()
