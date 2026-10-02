#!/usr/bin/env python3
"""Recruit actual Ian through native voted dialogue and verify replica party membership."""
import argparse
import json
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
    parser.add_argument('--talker', choices=('host', 'guest'), default='host')
    parser.add_argument('--death', action='store_true', help='Kill recruited Ian through native death, retain corpse/gear and recover it from disk.')
    parser.add_argument('--cleanup', action='store_true', help='Exercise native orphan companion cleanup with distinct/shared script IDs, nested resources and timers.')
    parser.add_argument('--battle', action='store_true', help='Exercise native Ian pistol AI against an installed cave scorpion before recovery.')
    parser.add_argument('--hostility', action='store_true', help='Guest damages Ian through native combat and verifies native retaliation, retained party membership and recovery.')
    args = parser.parse_args()
    if args.cleanup and args.death:
        parser.error('--cleanup requires the retained living companion')
    if args.death and args.hostility:
        parser.error('--death and --hostility are separate fixtures')
    if args.battle and args.hostility:
        parser.error('--battle and --hostility are separate fixtures')
    binary = args.binary.resolve()
    data = args.data_root.resolve()
    root = pathlib.Path(tempfile.mkdtemp(prefix='fallout-native-companion-'))
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
            command = ['timeout', '--kill-after=5s', '240s', 'xvfb-run', '-a', '--server-args=-screen 0 640x480x24',
                       'sh', '-c', 'printf %s "$DISPLAY" > display; printf %s "$XAUTHORITY" > xauthority; exec "$@"',
                       'sh', './fallout-ce', endpoint, '--multiplayer-smoke-test',
                       '--multiplayer-smoke-scenario=dialogue', f'--multiplayer-smoke-native-companion={args.talker}', f'--agent-command-file={directory}/commands', f'--agent-journal={directory}/journal.jsonl']
            if args.death:
                command.append('--multiplayer-smoke-native-companion-death')
            if args.cleanup:
                command.append('--multiplayer-smoke-native-companion-cleanup')
            if args.battle:
                command.append('--multiplayer-smoke-native-companion-battle')
            if args.hostility:
                command.append('--multiplayer-smoke-native-companion-hostility')
            with (directory / 'game.log').open('w') as output:
                process = subprocess.Popen(command, cwd=directory, env=env, stdout=output,
                                           stderr=subprocess.STDOUT, start_new_session=True)
            processes.append((directory, process))
            if index == 0:
                wait_for(directory, 'WAITING ON PORT')
        sequence = 0
        voted = set()
        instructed = set()
        intro_names = set()
        reply_names = set()
        deadline = time.monotonic() + 230
        while time.monotonic() < deadline:
            for directory, process in processes:
                if process.poll() is not None and 'MULTIPLAYER_SMOKE_TEST_PASS' not in log(directory):
                    raise RuntimeError(log(directory)[-6000:])
                journal = directory / 'journal.jsonl'
                if not journal.exists(): continue
                records = []
                for line in journal.read_text(errors='replace').splitlines():
                    try: records.append(json.loads(line))
                    except json.JSONDecodeError: pass
                worlds = [record for record in records if record.get('event') == 'world_state']
                if not worlds: continue
                state = worlds[-1]
                dialogue = state.get('dialogue')
                if not dialogue or not dialogue.get('options'): continue
                revision = dialogue['revision']
                if (directory.name, revision) in voted: continue
                options = dialogue['options']
                print(directory.name, revision, dialogue.get('reply'), options, flush=True)
                expected_name = 'Smoke Host' if args.talker == 'host' else 'Smoke Guest'
                other_name = 'Smoke Guest' if args.talker == 'host' else 'Smoke Host'
                if directory.name not in intro_names:
                    intro = next((option for option in options if "I'm " in option), None)
                    if intro is not None:
                        if f"I'm {expected_name}." not in intro or other_name in intro:
                            raise RuntimeError(f'{directory.name} native Ian introduction uses incorrect talker name: {intro!r}')
                        intro_names.add(directory.name)
                reply = dialogue.get('reply', '')
                if reply.startswith(('So, ', 'What can I do for you, ')):
                    if expected_name not in reply or other_name in reply:
                        raise RuntimeError(f'{directory.name} native Ian reply uses incorrect talker name: {reply!r}')
                    reply_names.add(directory.name)
                # Ordinary dialogue choices, never invoke party_add in the fixture.
                dismissing = 'NATIVE_COMPANION_RECRUITED role=host stage=1' in log(root / 'host') and 'NATIVE_COMPANION_DISMISSED role=host' not in log(root / 'host')
                configuring = dismissing and 'NATIVE_COMPANION_EQUIPMENT_PASS role=host' not in log(root / 'host')
                priorities = (('nothing', 'never mind') if directory.name in instructed else ('draw your best weapon',)) if configuring else ('leave', 'dismiss', 'go home', 'no longer', 'thanks', 'bye') if dismissing else ('stick together', 'join', 'help me', 'come with', '100', 'here you', 'deal', 'piece of', 'what do you do', "i'm smoke", 'thanks', 'goodbye', 'bye')
                selected = len(options) - 1
                for phrase in priorities:
                    match = next((i for i, option in enumerate(options) if phrase in option.lower()), None)
                    if match is not None:
                        selected = match
                        break
                sequence += 1
                if 'draw your best weapon' in options[selected].lower():
                    instructed.add(directory.name)
                with (directory / 'commands').open('a') as commands:
                    commands.write(f'{sequence} key {selected + 1}\n')
                voted.add((directory.name, revision))
            if all('MULTIPLAYER_SMOKE_TEST_PASS' in log(directory) for directory, _ in processes): break
            time.sleep(0.2)
        for directory, process in processes:
            wait_for(directory, f'MULTIPLAYER_SMOKE_TEST_PASS role={directory.name}', timeout=5)
            if process.wait(timeout=15) != 0: raise RuntimeError(log(directory))
        digests = [re.findall(r'MULTIPLAYER_RECOVERY_SMOKE_PASS[^\n]*digest=(\d+)', log(directory)) for directory, _ in processes]
        if not all(digests) or digests[0][-1] != digests[1][-1]:
            raise RuntimeError(f'Companion recovery digests differ: {digests}')
        for directory, _ in processes:
            if directory.name not in intro_names or directory.name not in reply_names:
                raise RuntimeError(f'{directory.name} lacks native Ian option/reply talker-name proof')
            for stage in ('option', 'reply'):
                if f'NATIVE_COMPANION_TALKER_NAME_PASS role={directory.name} stage={stage}' not in log(directory):
                    raise RuntimeError(f'{directory.name} lacks native runtime talker-name proof for {stage}')
            if (args.battle or args.hostility) and f'NATIVE_COMPANION_COMBAT_PASS role={directory.name}' not in log(directory):
                raise RuntimeError(f'{directory.name} has no native combat proof')
            if args.cleanup and f'NATIVE_COMPANION_CLEANUP_RECOVERY_PASS role={directory.name}' not in log(directory):
                raise RuntimeError(f'{directory.name} lacks retained native script/party recovery proof')
            if args.cleanup and not re.search(
                    rf'NATIVE_COMPANION_CLEANUP_READY role={directory.name} map=26 event=\d+ native_capture=1', log(directory)):
                raise RuntimeError(f'{directory.name} lacks native idle checkpoint proof before cleanup')
        if args.cleanup:
            for stage in ('initial_session', 'recovered_session'):
                if f'NATIVE_COMPANION_CLEANUP_AUTHORITY_PASS role=guest stage={stage} script=0 combat=0 random=0' not in log(root / 'guest'):
                    raise RuntimeError(f'Guest executed native rules during companion {stage}')
            for mode in ('distinct', 'shared'):
                if f'NATIVE_COMPANION_CLEANUP_PASS sid_mode={mode} first_tile_visits=2' not in log(root / 'host'):
                    raise RuntimeError(f'Host lacks native {mode} script cleanup proof')
        print(f'NATIVE_MULTIPLAYER_COMPANION_PASS talker={args.talker} recruited=Ian corpse={int(args.death)} battle={int(args.battle)} hostility={int(args.hostility)} cleanup={int(args.cleanup)} membership={"neither" if args.death else "both"} digest={digests[0][-1]}', flush=True)
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
