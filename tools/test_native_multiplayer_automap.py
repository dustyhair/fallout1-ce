#!/usr/bin/env python3
"""Test native automap controls and terminal map/Pip-Boy screens in muted isolated sessions."""
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
    parser.add_argument('--screen', choices=('automap', 'pipboy', 'screensaver'), default='automap', help='native screen for --defeat')
    parser.add_argument('--hazard', choices=('poison', 'radiation'), help='use a real native timed hazard; requires --defeat')
    parser.add_argument('--gdb', action='store_true')
    parser.add_argument('--defeat', choices=('host', 'guest'), help='inflict native lethal damage while both selected screens are open')
    parser.add_argument('--screenshots', action='store_true', help='save first scan images; requires Pillow')
    args = parser.parse_args()
    if args.screen != "automap" and not args.defeat:
        parser.error("--screen requires --defeat")
    if args.hazard and not args.defeat:
        parser.error("--hazard requires --defeat")
    binary = args.binary.resolve()
    data = args.data_root.resolve()
    root = pathlib.Path(tempfile.mkdtemp(prefix='fallout-native-automap-'))
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
                       '--multiplayer-smoke-native-automap']
            if args.defeat:
                command.append(f'--multiplayer-smoke-native-automap-defeat={args.defeat}')
            if args.screen == 'pipboy':
                command.append('--multiplayer-smoke-native-pipboy-defeat')
            elif args.screen == 'screensaver':
                command.append('--multiplayer-smoke-native-pipboy-screensaver')
            if args.hazard:
                command.append(f'--multiplayer-smoke-native-hazard={args.hazard}')
            if args.gdb:
                position = command.index('./fallout-ce')
                command[position:position] = ['gdb', '-batch', '-return-child-result', '-ex', 'set debuginfod enabled off', '-ex', 'run', '-ex', 'thread apply all bt', '--args']
            with (directory / 'game.log').open('w') as output:
                process = subprocess.Popen(command, cwd=directory, env=env, stdout=output,
                                           stderr=subprocess.STDOUT, start_new_session=True)
            processes.append((directory, process))
            if index == 0:
                wait_for(directory, 'WAITING ON PORT')
        for stage in ((1,) if args.defeat else (1, 2, 3)):
            for directory, _ in processes:
                wait_for(directory, f'NATIVE_AUTOMAP_READY role={directory.name} stage={stage}')
                if args.screen != 'automap':
                    marker = 'NATIVE_PIPBOY_SCREENSAVER_OPEN' if args.screen == 'screensaver' else 'NATIVE_PIPBOY_OPEN'
                    wait_for(directory, f'{marker} role={directory.name}')
                    continue
                time.sleep(0.8)
                key(directory, 's')
                if args.defeat:
                    continue
                if stage == 3:
                    key(directory, 'Return')
                else:
                    time.sleep(1)
                    key(directory, 's')
                if stage == 1 and args.screenshots:
                    local_env = dict(env, DISPLAY=(directory / 'display').read_text(),
                                     XAUTHORITY=(directory / 'xauthority').read_text())
                    subprocess.run(['python3', '-c', 'from PIL import ImageGrab; ImageGrab.grab().save("automap.png")'],
                                   cwd=directory, env=local_env, check=True)
                key(directory, 'Escape')
                wait_for(directory, f'NATIVE_AUTOMAP_STAGE_PASS role={directory.name} stage={stage}')
        for directory, process in processes:
            wait_for(directory, f'MULTIPLAYER_SMOKE_TEST_PASS role={directory.name}')
            if process.wait(timeout=15) != 0:
                raise RuntimeError(log(directory))
        if args.defeat:
            label = 'AUTOMAP' if args.screen == 'automap' else 'PIPBOY'
            wait_for(root / 'host', f'NATIVE_{label}_LETHAL_DAMAGE victim={args.defeat}')
            if args.hazard:
                wait_for(root / 'host', f'NATIVE_LETHAL_HAZARD_PASS hazard={args.hazard}')
            print(f'NATIVE_MULTIPLAYER_AUTOMAP_DEFEAT_PASS screen={args.screen} victim={args.defeat} native_window_unwound=both ending=acknowledged', flush=True)
        else:
            print('NATIVE_MULTIPLAYER_AUTOMAP_PASS scan=both repeat=both empty=both save=host reconnect=guest', flush=True)
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
