#!/usr/bin/env python3
"""Exercise native inventory drags on two virtual displays, then knife combat.

Usage: python3 tools/test_native_multiplayer_inventory.py BINARY DATA_ROOT
Requires xvfb-run, xdotool and ImageMagick import. Screenshots and logs are
preserved in the printed temporary directory; no physical display is used.
"""
import argparse
import os
import subprocess
import time
import pathlib
import shutil
import json
import re
import signal
import tempfile
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=pathlib.Path)
parser.add_argument('data_root', type=pathlib.Path)
parser.add_argument('--port', type=int, default=46688)
args = parser.parse_args()
binary = args.binary.resolve()
data = args.data_root.resolve()
root = pathlib.Path(tempfile.mkdtemp(prefix='fallout-native-inventory-'))
print(root, flush=True)
procs = []
baseenv = dict(os.environ, SDL_AUDIODRIVER='dummy', SDL_RENDER_DRIVER='software')

def wait_for(fn, seconds=45):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        val = fn()
        if val:
            return val
        time.sleep(0.1)
    raise RuntimeError('timed out')

def world(d):
    if not (d / 'journal.jsonl').exists():
        return {}
    for l in reversed((d / 'journal.jsonl').read_text().splitlines()):
        try:
            j = json.loads(l)
        except ValueError:
            continue
        if j.get('event') == 'world_state':
            return j
    return {}

def inv(d):
    return world(d).get('local_inventory', [])

def x(d, *args):
    env = dict(baseenv, DISPLAY=(d / 'display').read_text(), XAUTHORITY=(d / 'xauthority').read_text())
    subprocess.run(['xdotool', *map(str, args)], env=env, check=True)
seq = 0

def point(d, xy):
    global seq
    seq += 1
    with (d / 'commands').open('a') as f:
        f.write(f'{seq} move {xy[0]} {xy[1]}\n')
    time.sleep(0.2)

def drag(d, source, dest):
    point(d, source)
    time.sleep(0.15)
    x(d, 'mousedown', 1)
    time.sleep(0.25)
    point(d, dest)
    time.sleep(0.3)
    x(d, 'mouseup', 1)
    time.sleep(0.8)
try:
    for i, role in enumerate(('host', 'guest')):
        d = root / role
        d.mkdir()
        shutil.copy2(binary, d / 'fallout-ce')
        shutil.copytree(data / 'DATA' if (data / 'DATA').is_dir() else data / 'data', d / 'DATA')
        for f in ['CRITTER.DAT', 'MASTER.DAT']:
            (d / f).symlink_to(data / f if (data / f).exists() else data / f.lower())
        config = (data / 'fallout.cfg').read_text().replace('enabled=1', 'enabled=0')
        config = re.sub('(?m)^width\\s*=.*$', 'width=640', config)
        config = re.sub('(?m)^height\\s*=.*$', 'height=480', config)
        (d / 'fallout.cfg').write_text(config)
        game_args = [str(d / 'fallout-ce'), f'--multiplayer-host={args.port}' if i == 0 else f'--multiplayer-join=127.0.0.1:{args.port}', '--multiplayer-smoke-test', '--multiplayer-smoke-scenario=combat-attack', '--multiplayer-smoke-weapon=knife', '--multiplayer-smoke-native-inventory', f'--agent-journal={d}/journal.jsonl', f'--agent-command-file={d}/commands']
        p = subprocess.Popen(['timeout', '110s', 'xvfb-run', '-a', '--server-args=-screen 0 640x480x24', 'sh', '-c', 'printf %s "$DISPLAY" > display; printf %s "$XAUTHORITY" > xauthority; exec "$@"', 'sh', *game_args], cwd=d, env=baseenv, stdout=(d / 'game.log').open('w'), stderr=subprocess.STDOUT, start_new_session=True)
        procs.append((role, p, d))
        if i == 0:
            wait_for(lambda: 'WAITING ON PORT' in (d / 'game.log').read_text())
    for role, p, d in procs:
        wait_for(lambda: 'NATIVE_INVENTORY_READY' in (d / 'game.log').read_text())
        time.sleep(0.7)
        print(role, 'inventory', inv(d), flush=True)
        env = dict(baseenv, DISPLAY=(d / 'display').read_text(), XAUTHORITY=(d / 'xauthority').read_text())
        subprocess.run(['import', '-window', 'root', str(d / 'before.png')], env=env, check=True)
        items = inv(d)
        rows = [e for e in items if not (e.get('left_hand') or e.get('right_hand') or e.get('armor'))]
        idx = next((i for i, e in enumerate(rows) if e['pid'] == 4))
        drag(d, (155, 59 + idx * 48), (370, 313))
        wait_for(lambda: any((e['pid'] == 4 and e.get('right_hand') for e in inv(d))), 10)
        drag(d, (370, 313), (155, 59))
        wait_for(lambda: not any((e.get('right_hand') for e in inv(d))), 10)
        rows = [e for e in inv(d) if not (e.get('left_hand') or e.get('right_hand') or e.get('armor'))]
        idx = next((i for i, e in enumerate(rows) if e['pid'] == 4))
        drag(d, (155, 59 + idx * 48), (370, 313))
        wait_for(lambda: any((e['pid'] == 4 and e.get('right_hand') for e in inv(d))), 10)
        subprocess.run(['import', '-window', 'root', str(d / 'after.png')], env=env, check=True)
        print(role, 'drag equipment verified', flush=True)
    for role, p, d in reversed(procs):
        x(d, 'key', 'Escape')
    for role, p, d in procs:
        rc = p.wait()
        txt = (d / 'game.log').read_text()
        print(role, rc, '\n'.join((l for l in txt.splitlines() if any((k in l for k in ['PASS', 'FAIL', 'rejected', 'Host weapon'])))), flush=True)
        if rc != 0 or 'NATIVE_INVENTORY_PASS' not in txt or 'MULTIPLAYER_SMOKE_TEST_PASS' not in txt:
            raise RuntimeError(f'{role} failed; see {d}/game.log')
    digests = [re.findall('MULTIPLAYER_SMOKE_TEST_PASS.*digest=(\\d+)', (d / 'game.log').read_text())[-1] for _, _, d in procs]
    if len(set(digests)) != 1:
        raise RuntimeError(f'host/guest state mismatch: {digests}')
    for role, _, d in procs:
        knives = [item for item in inv(d) if item['pid'] == 4]
        expected = 2 if role == 'host' else 1
        if sum(item['quantity'] for item in knives) != expected:
            raise RuntimeError(f'{role}: equipping changed the knife count')
        if not any(item['right_hand'] and item['quantity'] == 1 for item in knives):
            raise RuntimeError(f'{role}: the knife was not left equipped')
    print('NATIVE_DRAG_AND_COMBAT_PASS', root, flush=True)
finally:
    for _, p, _ in procs:
        if p.poll() is None:
            os.killpg(p.pid, signal.SIGTERM)
            p.wait(timeout=5)
