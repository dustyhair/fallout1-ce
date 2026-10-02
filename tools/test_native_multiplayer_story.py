#!/usr/bin/env python3
"""Test real movies, settlement slides and final departure/credits in muted isolated multiplayer."""
import argparse
import os
import pathlib
import re
import shutil
import signal
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=pathlib.Path)
    parser.add_argument('data_root', type=pathlib.Path)
    parser.add_argument('--port', type=int, default=63400)
    parser.add_argument('--reconnect', action='store_true')
    parser.add_argument('--missing-movie', action='store_true')
    parser.add_argument('--unfocused-guest', action='store_true')
    parser.add_argument('--watch-guest', action='store_true', help='Let the guest watch every movie, narration and credit without skipping.')
    args = parser.parse_args()
    binary = args.binary.resolve()
    data = args.data_root.resolve()
    root = pathlib.Path(tempfile.mkdtemp(prefix='fallout-native-story-'))
    print(root, flush=True)
    env = dict(os.environ, SDL_AUDIODRIVER='dummy', SDL_RENDER_DRIVER='software')
    processes = []

    def log(directory):
        return (directory / 'game.log').read_text(errors='replace')

    def wait_for(directory, marker, timeout=45):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            text = log(directory)
            if marker in text:
                return
            process = next(p for d, p in processes if d == directory)
            if process.poll() is not None:
                raise RuntimeError(f'{directory.name} exited before {marker}:\n{text[-4000:]}')
            time.sleep(0.1)
        raise RuntimeError(f'{directory.name} timed out before {marker}:\n{log(directory)[-4000:]}')

    try:
        for index, role in enumerate(('host', 'guest')):
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
            config = config.replace('enabled=1', 'enabled=0')
            (directory / 'fallout.cfg').write_text(config)
            endpoint = f'--multiplayer-host={args.port}' if index == 0 else f'--multiplayer-join=127.0.0.1:{args.port}'
            command = ['timeout', '--kill-after=5s', '2100s' if args.watch_guest else '180s', 'xvfb-run', '-a', '--server-args=-screen 0 640x480x24',
                       'sh', '-c', 'printf %s "$DISPLAY" > display; printf %s "$XAUTHORITY" > xauthority; exec "$@"',
                       'sh', './fallout-ce', endpoint, '--multiplayer-smoke-test',
                       '--multiplayer-smoke-scenario=recovery', '--multiplayer-smoke-native-story', f'--agent-command-file={directory}/commands']
            if args.reconnect: command.append('--multiplayer-smoke-native-story-reconnect')
            if args.missing_movie: command.append('--multiplayer-smoke-native-story-missing')
            with (directory / 'game.log').open('w') as output:
                process = subprocess.Popen(command, cwd=directory, env=env, stdout=output,
                                           stderr=subprocess.STDOUT, start_new_session=True)
            processes.append((directory, process))
            if index == 0:
                wait_for(directory, 'WAITING ON PORT')
        # Drive each native UI independently; different skip timing exercises
        # the barrier instead of forcing both peers to return together.
        if args.unfocused_guest:
            guest = root / 'guest'
            wait_for(guest, 'NATIVE_STORY_STARTED role=guest revision=1')
            local_env = dict(env, DISPLAY=(guest / 'display').read_text(), XAUTHORITY=(guest / 'xauthority').read_text())
            subprocess.run(['xdotool', 'windowfocus', '0'], env=local_env, check=True)
        sequence = 0
        deadline = time.monotonic() + (2090 if args.watch_guest else 170)
        while time.monotonic() < deadline:
            for directory, process in processes:
                if process.poll() is not None and 'MULTIPLAYER_SMOKE_TEST_PASS' not in log(directory):
                    raise RuntimeError(log(directory)[-6000:])
            sequence += 1
            for directory, process in processes:
                if process.poll() is None and (directory.name == 'host' or (not args.watch_guest and sequence % 3 == 0)):
                    with (directory / 'commands').open('a') as commands:
                        commands.write(f'{sequence} key space\n')
            if all('MULTIPLAYER_SMOKE_TEST_PASS' in log(directory) for directory, _ in processes):
                break
            time.sleep(0.7)
        for directory, process in processes:
            wait_for(directory, f'MULTIPLAYER_SMOKE_TEST_PASS role={directory.name}', timeout=5)
            if process.wait(timeout=15) != 0:
                raise RuntimeError(log(directory))
            for revision in range(1, 5):
                wait_for(directory, f'NATIVE_STORY_COMPLETED role={directory.name} revision={revision}')
        guest_log = log(root / 'guest')
        if args.reconnect:
            wait_for(root / 'guest', 'NATIVE_STORY_RECONNECT_REQUEST revision=1')
            if guest_log.count('NATIVE_STORY_STARTED role=guest revision=1 ') != 1:
                raise RuntimeError('recovery replayed an already completed native movie')
        expected_result = -1 if args.missing_movie else 0
        wait_for(root / 'guest', f'NATIVE_STORY_COMPLETED role=guest revision=1 kind=0 result={expected_result}')
        host_slides = re.findall(r'NATIVE_STORY_SLIDE role=host narration=(\d+)', log(root / 'host'))
        guest_slides = re.findall(r'NATIVE_STORY_SLIDE role=guest narration=(\d+)', log(root / 'guest'))
        if not host_slides or host_slides != guest_slides:
            raise RuntimeError(f'settlement presentations diverged: {host_slides} != {guest_slides}')
        if 'acknowledgement timed out' in log(root / 'host'):
            raise RuntimeError(log(root / 'host'))
        print(f'NATIVE_MULTIPLAYER_STORY_PASS revisions=4 slides={host_slides} terminal=acknowledged', flush=True)
    finally:
        for _, process in processes:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


if __name__ == '__main__':
    main()
