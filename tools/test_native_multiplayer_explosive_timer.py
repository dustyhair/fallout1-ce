#!/usr/bin/env python3
"""Test both native explosive timer pickers with isolated data and muted Xvfb sessions."""
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
    parser.add_argument('--port', type=int, default=63000)
    parser.add_argument('--defeat', choices=('host', 'guest'), help='test death while both actual timer prompts are open')
    parser.add_argument('--gdb', action='store_true')
    args = parser.parse_args()
    binary = args.binary.resolve()
    data = args.data_root.resolve()
    root = pathlib.Path(tempfile.mkdtemp(prefix='fallout-native-explosive-timer-'))
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

    def key(directory, name):
        local_env = dict(env, DISPLAY=(directory / 'display').read_text(),
                         XAUTHORITY=(directory / 'xauthority').read_text())
        subprocess.run(['xdotool', 'key', '--clearmodifiers', name], env=local_env, check=True)
        time.sleep(0.35)

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
            command = ['timeout', '180s', 'xvfb-run', '-a', '--server-args=-screen 0 640x480x24',
                       'sh', '-c', 'printf %s "$DISPLAY" > display; printf %s "$XAUTHORITY" > xauthority; exec "$@"',
                       'sh', './fallout-ce', endpoint, '--multiplayer-smoke-test',
                       '--multiplayer-smoke-native-explosive-timer']
            if args.defeat:
                command.append(f'--multiplayer-smoke-native-timer-defeat={args.defeat}')
            if args.gdb:
                position = command.index('./fallout-ce')
                command[position:position] = ['gdb', '-batch', '-return-child-result', '-ex', 'set debuginfod enabled off', '-ex', 'run', '-ex', 'thread apply all bt', '--args']
            with (directory / 'game.log').open('w') as output:
                process = subprocess.Popen(command, cwd=directory, env=env, stdout=output,
                                           stderr=subprocess.STDOUT, start_new_session=True)
            processes.append((directory, process))
            if index == 0:
                wait_for(directory, 'WAITING ON PORT')
        if args.defeat:
            for directory, _ in processes:
                wait_for(directory, f'NATIVE_QUANTITY_PROMPT_OPEN role={directory.name} type=timer')
            host = root / 'host'
            local_env = dict(env, DISPLAY=(host / 'display').read_text(),
                             XAUTHORITY=(host / 'xauthority').read_text())
            subprocess.run(['xdotool', 'mousemove', '320', '180', 'mousedown', '1'], env=local_env, check=True)
        for stage in (() if args.defeat else ('cancel', 'arm')):
            for directory, _ in processes:
                wait_for(directory, f'NATIVE_EXPLOSIVE_TIMER_READY role={directory.name} stage={stage}')
                time.sleep(0.8)
                key(directory, 'Escape' if stage == 'cancel' else 'Return')
                wait_for(directory, f'NATIVE_EXPLOSIVE_TIMER_STAGE_PASS role={directory.name} stage={stage}')
        for directory, process in processes:
            wait_for(directory, f'MULTIPLAYER_SMOKE_TEST_PASS role={directory.name}')
            if process.wait(timeout=15) != 0:
                raise RuntimeError(log(directory))
        if args.defeat:
            wait_for(root / 'host', f'NATIVE_TIMER_LETHAL_DAMAGE mouse_held=1 victim={args.defeat}')
            print(f'NATIVE_MULTIPLAYER_TIMER_DEFEAT_PASS victim={args.defeat} prompts_closed=both bombs_unarmed=both ending=acknowledged', flush=True)
        else:
            print('NATIVE_MULTIPLAYER_EXPLOSIVE_TIMER_PASS cancel=both arm=both split=both save=host reconnect=guest', flush=True)
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
