#!/usr/bin/env python3
"""Exercise native inventory drags on two virtual displays, then knife combat.

Usage: python3 tools/test_native_multiplayer_inventory.py BINARY DATA_ROOT
Requires xvfb-run, xdotool and ImageMagick import. Screenshots and logs are
preserved in the printed temporary directory; no physical display is used.
"""
import argparse
import sys
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
parser.add_argument('--defeat-context-menu', action='store_true', help='test native death while holding the inventory action menu')
parser.add_argument('--defeat-drag', action='store_true', help='test native death while the host holds an inventory drag')
parser.add_argument('--context-actions', action='store_true', help='also use/drop drug stacks, unload a pistol, and drag ammo onto it')
parser.add_argument('--owned-containers', action='store_true', help='deposit a knife, open its container, withdraw to parent, and close navigation')
parser.add_argument('--nested-actions', action='store_true', help='also use a drug and unload/reload a pistol inside an owned container')
parser.add_argument('--combat-container', action='store_true', help='also exercise guest container navigation and actions during its combat turn')
args = parser.parse_args()
if args.defeat_context_menu:
    args.defeat_drag = True
if args.combat_container:
    args.nested_actions = True
if args.nested_actions:
    args.owned_containers = True
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
    details = []
    for role, process, directory in procs:
        details.append(f'{role}: process={process.poll()} world={world(directory)}')
        if process.poll() is None:
            env = dict(baseenv, DISPLAY=(directory / 'display').read_text(),
                       XAUTHORITY=(directory / 'xauthority').read_text())
            subprocess.run(['import', '-window', 'root', str(directory / 'timeout.png')],
                           env=env, check=False)
    raise RuntimeError('timed out\n' + '\n'.join(details))

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
    return sorted(world(d).get('local_inventory', []), key=lambda e: e.get('inventory_index', e['entity_id']))

def contained(d, holder_id):
    return sorted((e for e in world(d).get('contained_inventory', []) if e['holder_id'] == holder_id),
                  key=lambda e: e['inventory_index'])

def contained_item(d, holder_id, pid):
    return next(e for e in contained(d, holder_id) if e['pid'] == pid)

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
def context_action_at(d, row, index):
    origin = (155, 59 + row * 48)
    point(d, origin)
    x(d, 'mousedown', 1)
    time.sleep(0.8)
    for step in range(index):
        point(d, (origin[0], origin[1] + (step + 1) * 15))
    x(d, 'mouseup', 1)
    time.sleep(0.8)

def context_action(d, pid, index):
    rows = [e for e in inv(d) if not (e.get('left_hand') or e.get('right_hand') or e.get('armor'))]
    row = next(i for i, e in enumerate(rows) if e['pid'] == pid)
    context_action_at(d, row, index)

def toggle_cursor(d):
    x(d, 'mousedown', 3)
    time.sleep(0.2)
    x(d, 'mouseup', 3)
    time.sleep(0.3)

try:
    for i, role in enumerate(('host', 'guest')):
        d = root / role
        d.mkdir()
        shutil.copy2(binary, d / 'fallout-ce')
        shutil.copytree(data / 'DATA' if (data / 'DATA').is_dir() else data / 'data', d / 'DATA')
        for f in ['CRITTER.DAT', 'MASTER.DAT']:
            archive = (data / f if (data / f).exists() else data / f.lower()).resolve()
            (d / f).symlink_to(archive)
            (d / f.lower()).symlink_to(archive)
        config = (data / 'fallout.cfg').read_text().replace('enabled=1', 'enabled=0')
        config = re.sub(r'(?m)^(master_patches|critter_patches)\s*=.*$', r'\1=DATA', config)
        config = re.sub('(?m)^width\\s*=.*$', 'width=640', config)
        config = re.sub('(?m)^height\\s*=.*$', 'height=480', config)
        (d / 'fallout.cfg').write_text(config)
        game_args = [str(d / 'fallout-ce'), f'--multiplayer-host={args.port}' if i == 0 else f'--multiplayer-join=127.0.0.1:{args.port}', '--multiplayer-smoke-test', '--multiplayer-smoke-scenario=combat-attack', '--multiplayer-smoke-weapon=knife', '--multiplayer-smoke-native-inventory', f'--agent-journal={d}/journal.jsonl', f'--agent-command-file={d}/commands']
        if args.defeat_drag:
            game_args.append('--multiplayer-smoke-native-inventory-defeat')
        if args.combat_container:
            game_args.append('--multiplayer-smoke-native-combat-inventory')
        p = subprocess.Popen(['timeout', '--kill-after=5s', '300s' if args.nested_actions else '180s', 'xvfb-run', '-a', '--server-args=-screen 0 640x480x24', 'sh', '-c', 'printf %s "$DISPLAY" > display; printf %s "$XAUTHORITY" > xauthority; exec "$@"', 'sh', *game_args], cwd=d, env=baseenv, stdout=(d / 'game.log').open('w'), stderr=subprocess.STDOUT, start_new_session=True)
        procs.append((role, p, d))
        if i == 0:
            wait_for(lambda: 'WAITING ON PORT' in (d / 'game.log').read_text())
    if args.defeat_drag:
        for role, p, d in procs:
            wait_for(lambda: 'NATIVE_INVENTORY_READY' in (d / 'game.log').read_text())
        host = procs[0][2]
        if args.defeat_context_menu:
            toggle_cursor(host)
        point(host, (155, 59))
        time.sleep(0.8)
        before = inv(host)
        x(host, 'mousedown', 1)
        # Leave the mouse held throughout shutdown: releasing it would hide
        # the native drag-loop hang this fixture is intended to catch.
        for role, p, d in procs:
            wait_for(lambda: f'MULTIPLAYER_SMOKE_TEST_PASS role={role}' in (d / 'game.log').read_text(), 15)
            if p.wait(timeout=10) != 0:
                raise RuntimeError((d / 'game.log').read_text())
        if 'NATIVE_INVENTORY_LETHAL_DAMAGE mouse_held=1' not in (host / 'game.log').read_text():
            raise RuntimeError('native held-mouse death fixture did not run')
        if inv(host) != before:
            raise RuntimeError('host inventory changed during cancelled mouse action')
        action = 'context_menu' if args.defeat_context_menu else 'drag'
        print(f'NATIVE_MULTIPLAYER_INVENTORY_DEFEAT_PASS held_action={action} cancelled=1 inventory_unchanged=1 native_windows_closed=both ending=acknowledged', flush=True)
        sys.exit(0)
    for role, p, d in procs:
        wait_for(lambda: 'NATIVE_INVENTORY_READY' in (d / 'game.log').read_text())
        point(d, (155, 59))
        wait_for(lambda: any(e['pid'] == 4 for e in inv(d)), 10)
        print(role, 'inventory', inv(d), flush=True)
        env = dict(baseenv, DISPLAY=(d / 'display').read_text(), XAUTHORITY=(d / 'xauthority').read_text())
        subprocess.run(['import', '-window', 'root', str(d / 'before.png')], env=env, check=True)
        if args.owned_containers:
            rows = [e for e in inv(d) if not (e.get('left_hand') or e.get('right_hand') or e.get('armor'))]
            knife_row = next(i for i, e in enumerate(rows) if e['pid'] == 4)
            bag_row = next(i for i, e in enumerate(rows) if e['pid'] == 211)
            expected = sum(e['quantity'] for e in rows if e['pid'] == 4)
            drag(d, (155, 59 + knife_row * 48), (155, 59 + bag_row * 48))
            if expected > 1:
                x(d, 'key', 'Return')
            wait_for(lambda: sum(e['quantity'] for e in inv(d) if e['pid'] == 4) == expected - 1, 10)
            toggle_cursor(d)
            context_action(d, 211, 1)
            toggle_cursor(d)
            # The new container contains only the deposited knife. Drag to
            # the body panel to return it to the parent inventory.
            drag(d, (155, 59), (285, 85))
            wait_for(lambda: sum(e['quantity'] for e in inv(d) if e['pid'] == 4) == expected, 10)
            point(d, (285, 85))
            x(d, 'mousedown', 1)
            time.sleep(0.2)
            x(d, 'mouseup', 1)
            time.sleep(0.5)
            print(role, 'owned container deposit/navigation/withdraw verified', flush=True)
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
        if args.nested_actions:
            for pid in (40, 8, 29):
                rows = [e for e in inv(d) if not (e.get('left_hand') or e.get('right_hand') or e.get('armor'))]
                row = next(i for i, e in enumerate(rows) if e['pid'] == pid)
                bag_row = next(i for i, e in enumerate(rows) if e['pid'] == 211)
                quantity = rows[row]['quantity']
                drag(d, (155, 59 + row * 48), (155, 59 + bag_row * 48))
                if quantity > 1:
                    x(d, 'key', str(quantity))
                    x(d, 'key', 'Return')
                wait_for(lambda: not any(e['pid'] == pid for e in inv(d)), 10)
            bag_id = next(e['entity_id'] for e in inv(d) if e['pid'] == 211)
            toggle_cursor(d)
            context_action(d, 211, 1)
            context_action_at(d, contained_item(d, bag_id, 40)['inventory_index'], 1)
            wait_for(lambda: contained_item(d, bag_id, 40)['quantity'] == 3, 10)
            toggle_cursor(d)
            row = contained_item(d, bag_id, 40)['inventory_index']
            drag(d, (155, 59 + row * 48), (285, 85))
            x(d, 'key', '3')
            x(d, 'key', 'Return')
            wait_for(lambda: sum(e['quantity'] for e in inv(d) if e['pid'] == 40) == 3, 10)
            toggle_cursor(d)
            context_action_at(d, contained_item(d, bag_id, 8)['inventory_index'], 1)
            wait_for(lambda: contained_item(d, bag_id, 8)['ammo'] == 0, 10)
            toggle_cursor(d)
            ammo_row = contained_item(d, bag_id, 29)['inventory_index']
            weapon_row = contained_item(d, bag_id, 8)['inventory_index']
            drag(d, (155, 59 + ammo_row * 48), (155, 59 + weapon_row * 48))
            x(d, 'key', 'Return')
            wait_for(lambda: contained_item(d, bag_id, 8)['ammo'] == 12, 10)
            weapon_row = contained_item(d, bag_id, 8)['inventory_index']
            drag(d, (155, 59 + weapon_row * 48), (285, 85))
            wait_for(lambda: any(e['pid'] == 8 and e['ammo'] == 12 for e in inv(d)), 10)
            ammo_row = contained_item(d, bag_id, 29)['inventory_index']
            drag(d, (155, 59 + ammo_row * 48), (285, 85))
            x(d, 'key', '2')
            x(d, 'key', 'Return')
            wait_for(lambda: sum(e['quantity'] for e in inv(d) if e['pid'] == 29) == 2, 10)
            assert sum(e['ammo'] + (e['quantity'] - 1) * 24 for e in inv(d) if e['pid'] == 29) == 48
            point(d, (285, 85))
            x(d, 'mousedown', 1)
            time.sleep(0.2)
            x(d, 'mouseup', 1)
            time.sleep(0.5)
            print(role, 'contained drug use/unload/reload verified', flush=True)
        if args.context_actions:
            toggle_cursor(d)
            context_action(d, 40, 1)  # Drug menu: look, use, drop, cancel.
            wait_for(lambda: sum(e['quantity'] for e in inv(d) if e['pid'] == 40) == 3, 10)
            context_action(d, 40, 2)
            x(d, 'key', '2')
            x(d, 'key', 'Return')
            wait_for(lambda: sum(e['quantity'] for e in inv(d) if e['pid'] == 40) == 1, 10)
            context_action(d, 8, 1)  # Weapon menu: look, unload, drop, cancel.
            wait_for(lambda: any(e['pid'] == 8 and e.get('ammo') == 0 for e in inv(d)), 10)
            toggle_cursor(d)
            rows = [e for e in inv(d) if not (e.get('left_hand') or e.get('right_hand') or e.get('armor'))]
            ammo_row = next(i for i, e in enumerate(rows) if e['pid'] == 29)
            weapon_row = next(i for i, e in enumerate(rows) if e['pid'] == 8)
            # Ammo dragging opens the native quantity picker. Release first,
            # then confirm one clip while network checkpoints keep flowing.
            point(d, (155, 59 + ammo_row * 48))
            x(d, 'mousedown', 1)
            time.sleep(0.25)
            point(d, (155, 59 + weapon_row * 48))
            x(d, 'mouseup', 1)
            time.sleep(0.8)
            x(d, 'key', 'Return')
            wait_for(lambda: any(e['pid'] == 8 and e.get('ammo') == 12 for e in inv(d)), 10)
            print(role, 'native use/drop/unload/ammo drag verified', flush=True)

        if args.combat_container and role == 'guest':
            # Keep one drug and the pistol in this player's bag for combat.
            for pid, quantity in ((40, 1), (8, 1)):
                rows = [e for e in inv(d) if not (e.get('left_hand') or e.get('right_hand') or e.get('armor'))]
                row = next(i for i, e in enumerate(rows) if e['pid'] == pid)
                bag_row = next(i for i, e in enumerate(rows) if e['pid'] == 211)
                available = rows[row]['quantity']
                drag(d, (155, 59 + row * 48), (155, 59 + bag_row * 48))
                if available > 1:
                    x(d, 'key', str(quantity))
                    x(d, 'key', 'Return')
                wait_for(lambda: sum(e['quantity'] for e in inv(d) if e['pid'] == pid) == available - quantity, 10)

    for role, p, d in reversed(procs):
        x(d, 'key', 'Escape')
    if args.combat_container:
        d = root / 'guest'
        wait_for(lambda: 'NATIVE_COMBAT_INVENTORY_READY' in (d / 'game.log').read_text(), 45)
        time.sleep(0.8)
        bag_id = next(e['entity_id'] for e in inv(d) if e['pid'] == 211)
        toggle_cursor(d)
        context_action(d, 211, 1)
        toggle_cursor(d)
        drug_row = contained_item(d, bag_id, 40)['inventory_index']
        drag(d, (155, 59 + drug_row * 48), (285, 85))
        wait_for(lambda: sum(e['quantity'] for e in inv(d) if e['pid'] == 40) == 3, 10)
        point(d, (285, 85))
        x(d, 'mousedown', 1)
        time.sleep(0.2)
        x(d, 'mouseup', 1)
        time.sleep(0.5)
        rows = [e for e in inv(d) if not (e.get('left_hand') or e.get('right_hand') or e.get('armor'))]
        drug_row = next(i for i, e in enumerate(rows) if e['pid'] == 40)
        bag_row = next(i for i, e in enumerate(rows) if e['pid'] == 211)
        drag(d, (155, 59 + drug_row * 48), (155, 59 + bag_row * 48))
        x(d, 'key', '1')
        x(d, 'key', 'Return')
        wait_for(lambda: sum(e['quantity'] for e in inv(d) if e['pid'] == 40) == 2, 10)
        wait_for(lambda: any(e['pid'] == 40 and e['quantity'] == 1 for e in contained(d, bag_id)), 10)
        toggle_cursor(d)
        context_action(d, 211, 1)
        print('guest combat container withdrawal/deposit verified', flush=True)
        context_action_at(d, contained_item(d, bag_id, 40)['inventory_index'], 1)
        wait_for(lambda: not any(e['pid'] == 40 for e in contained(d, bag_id)), 10)
        context_action_at(d, contained_item(d, bag_id, 8)['inventory_index'], 1)
        wait_for(lambda: contained_item(d, bag_id, 8)['ammo'] == 0, 10)
        # Close while still inside the container. CloseInventory must use the
        # actor's equipment and turn, and charge no second inventory opening.
        x(d, 'key', 'Escape')
        wait_for(lambda: 'NATIVE_COMBAT_INVENTORY_PASS' in (d / 'game.log').read_text(), 10)
        print('guest combat container navigation/use/unload verified', flush=True)

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
