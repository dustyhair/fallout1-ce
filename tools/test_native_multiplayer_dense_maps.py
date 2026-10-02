#!/usr/bin/env python3
"""Check installed dense maps through native arrival, disk recovery and TLS reconnect."""
import argparse
import os
import pathlib
import re
import shutil
import signal
import subprocess
import tempfile
import time

MAPS = {'gunrunners': 46, 'hub': 38, 'necropolis': 3, 'mariposa': 31, 'cathedral': 17}


def run_map(binary, data, name, port, root):
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
                raise RuntimeError(f'{name} {directory.name} exited before {marker}:\n{content[-9000:]}')
            time.sleep(0.1)
        raise RuntimeError(f'{name} {directory.name} timed out before {marker}:\n{log(directory)[-9000:]}')

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
            command = ['timeout', '--kill-after=5s', '180s', 'xvfb-run', '-a', '--server-args=-screen 0 640x480x24',
                       './fallout-ce', endpoint, '--multiplayer-smoke-test',
                       '--multiplayer-smoke-scenario=recovery', f'--multiplayer-smoke-native-dense-map={name}']
            with (directory / 'game.log').open('w') as output:
                process = subprocess.Popen(command, cwd=directory, env=env, stdout=output,
                                           stderr=subprocess.STDOUT, start_new_session=True)
            processes.append((directory, process))
            if role == 'host':
                wait(directory, 'WAITING ON PORT', 20)
        deadline = time.monotonic() + 175
        peer_ready = root / 'host' / 'dense-peer-native-ready'
        while time.monotonic() < deadline:
            guest_text = log(root / 'guest')
            if not peer_ready.exists() and re.search(
                    rf'NATIVE_DENSE_SNAPSHOT_PASS role=guest name={name} stage=arrival map={MAPS[name]} ', guest_text):
                pending = peer_ready.with_suffix('.pending')
                pending.write_text(f'{MAPS[name]}\n')
                pending.replace(peer_ready)
            if all('MULTIPLAYER_RECOVERY_SMOKE_PASS' in log(d) for d, _ in processes):
                break
            for directory, process in processes:
                if process.poll() is not None and 'MULTIPLAYER_RECOVERY_SMOKE_PASS' not in log(directory):
                    raise RuntimeError(f'{name} {directory.name}:\n{log(directory)[-9000:]}')
            time.sleep(0.1)
        contents = {}
        metrics = {}
        sections = {}
        digests = []
        for directory, process in processes:
            text = contents[directory.name] = wait(directory, 'MULTIPLAYER_RECOVERY_SMOKE_PASS', 2)
            if process.wait(timeout=10) != 0:
                raise RuntimeError(f'{name} {directory.name} returned {process.returncode}:\n{text[-9000:]}')
            if f'KNOCKOUT_RECOVERY_PASS role={directory.name} disk_load=1 reconnect=1 still_incapacitated=1 timer_exact=1' not in text:
                raise RuntimeError(f'{name} {directory.name} lacks native disk/reconnect timer proof')
            if not re.search(rf'NATIVE_DENSE_SNAPSHOT_PASS role={directory.name} name={name} stage=arrival map={MAPS[name]} ', text):
                raise RuntimeError(f'{name} {directory.name} lacks actual native arrival')
            lines = re.findall(rf'NATIVE_DENSE_SNAPSHOT_PASS role={directory.name} name={name} stage=recovered[^\n]*', text)
            if not lines or 'NATIVE_DENSE_SNAPSHOT_FAIL' in text:
                raise RuntimeError(f'{name} {directory.name} lacks valid recovered native snapshot')
            metrics[directory.name] = dict(re.findall(r'(\w+)=(\d+)', lines[-1]))
            sections[directory.name] = re.findall(r'RECOVERY_SECTION_DIGEST[^\n]*', text)[-1]
            passes = re.findall(r'MULTIPLAYER_RECOVERY_SMOKE_PASS[^\n]*', text)
            result = dict(re.findall(r'(\w+)=(\d+)', passes[-1]))
            if int(result['map']) != MAPS[name]:
                raise RuntimeError(f'{name} recovered incorrect map {result["map"]}')
            digests.append(result['digest'])
        if digests[0] != digests[1] or metrics['host'] != metrics['guest'] or sections['host'] != sections['guest']:
            raise RuntimeError(f'{name} native phase/object/timer sections differ: {metrics}, {sections}')
        if f'NATIVE_DENSE_BASELINE_SAVE_PASS map={MAPS[name]} slot=2 isolated_profile=1' not in contents['host']:
            raise RuntimeError(f'{name} did not preserve the native arrival before fixture mutations')
        if f'NATIVE_DENSE_PEER_NATIVE_READY_PASS map={MAPS[name]} driver_guest_capture_witness=1' not in contents['host']:
            raise RuntimeError(f'{name} recovery started without the guest native arrival witness')
        state = metrics['host']
        print(f'NATIVE_MULTIPLAYER_DENSE_MAP_PASS name={name} map={MAPS[name]} bytes={state["bytes"]} '
              f'critters={state["critters"]} scenery={state["scenery"]} items={state["items"]} '
              f'timers={state["timers"]} digest={digests[0]} root={root}', flush=True)
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
    parser.add_argument('--map', choices=tuple(MAPS), action='append', help='Repeat to select maps; default runs all five.')
    parser.add_argument('--port', type=int, default=64800)
    args = parser.parse_args()
    names = args.map or tuple(MAPS)
    if not 1024 <= args.port <= 65535 - len(names):
        parser.error('port range exceeds available TCP ports')
    root = pathlib.Path(tempfile.mkdtemp(prefix='fallout-native-dense-maps-', dir='/var/tmp'))
    print(root, flush=True)
    pinned = root / 'fallout-ce-fixture'
    shutil.copy2(args.binary.resolve(), pinned)
    for index, name in enumerate(names):
        run_map(pinned, args.data_root.resolve(), name, args.port + index, root / f'{index:02d}-{name}')
    print(f'NATIVE_MULTIPLAYER_DENSE_MAPS_PASS maps={len(names)} root={root}', flush=True)


if __name__ == '__main__':
    main()
