#!/usr/bin/env python3
"""Compile native checkpoint timers fixtures privately and run a headless host/guest pair."""
import argparse
import os
import pathlib
import re
import shlex
import shutil
import signal
import socket
import subprocess
import tempfile
import time


FIXTURES = {
    'src/multiplayer/network_world.cc': 'native_snapshot_timers_world.cc',
    'src/game/object.cc': 'native_snapshot_timers_objects.cc',
    'src/game/queue.cc': 'native_snapshot_timers_queue.cc',
    'src/game/scripts.cc': 'native_snapshot_timers_scripts.cc',
    'src/plib/gnw/memory.cc': 'native_snapshot_timers_memory.cc',
}


def compile_fixture(build, source, tests, root):
    commands = subprocess.check_output(
        ['ninja', '-C', str(build), '-t', 'commands', 'fallout-ce'], text=True, timeout=30).splitlines()
    link = shlex.split(commands[-1])
    if link[:2] == [':', '&&']:
        link = link[2:]
    if link[-2:] == ['&&', ':']:
        link = link[:-2]
    if '&&' in link or '-o' not in link:
        raise RuntimeError('unsupported Ninja link command')
    link = [arg for arg in link if not arg.startswith('-Wl,--dependency-file=')]
    pinned = root / 'source'
    shutil.copytree(source / 'src', pinned / 'src')
    shutil.copytree(tests, pinned / 'tests', ignore=shutil.ignore_patterns('__pycache__'))
    # Pin every linked object/archive before compiling, so a later rebuild of
    # the supplied game does not change this fixture's runtime or entry point.
    for index, argument in enumerate(link):
        if argument.endswith(('.o', '.a')):
            original = pathlib.Path(argument)
            if not original.is_absolute():
                original = build / original
            target = root / f'linked-{index}-{original.name}'
            shutil.copy2(original, target)
            link[index] = str(target)
    print(f'NATIVE_SNAPSHOT_TIMERS_PINNED {root}', flush=True)
    for relative, fixture in FIXTURES.items():
        compile_line = next((line for line in commands
                             if ' -c ' in line and line.endswith('/' + relative)), None)
        if compile_line is None:
            raise RuntimeError(f'could not find Ninja compile command for {relative}')
        compile_args = shlex.split(compile_line)
        obj = root / (fixture + '.o')
        compile_args[compile_args.index('-o') + 1] = str(obj)
        compile_args[compile_args.index('-c') + 1] = str(pinned / 'tests' / fixture)
        if '-MF' in compile_args:
            compile_args[compile_args.index('-MF') + 1] = str(root / (fixture + '.d'))
        compile_args[1:1] = ['-I', str(pinned / 'src'), '-I', str(pinned / 'tests')]
        if relative == 'src/multiplayer/network_world.cc' and 'class PreparedQueueEvents' in (pinned / 'src/game/queue.h').read_text():
            compile_args.insert(1, '-DNATIVE_SNAPSHOT_TIMERS_PREPARED_OWNER_CONTROL')
        subprocess.run(compile_args, cwd=build, check=True, timeout=120)
        original_name = relative.rsplit('/', 1)[-1] + '.o'
        matches = [index for index, argument in enumerate(link)
                   if pathlib.Path(argument).name.endswith('-' + original_name)]
        if len(matches) != 1:
            raise RuntimeError(f'could not uniquely replace {relative} object')
        link[matches[0]] = str(obj)
    binary = root / 'fallout-ce-fixture'
    link[link.index('-o') + 1] = str(binary)
    subprocess.run(link, cwd=build, check=True, timeout=120)
    return binary


def installed_file(data, name):
    for entry in data.iterdir():
        if entry.name.lower() == name.lower():
            return entry
    raise RuntimeError(f'installed game is missing {name}: {data}')


def run_fixture(binary, data, port, root, expect_negative, fault_mode, expect_owner_negative=False):
    processes = []
    env = dict(os.environ, SDL_AUDIODRIVER='dummy', SDL_RENDER_DRIVER='software',
               NATIVE_SNAPSHOT_TIMER_FAULT=fault_mode)
    env.pop('NATIVE_SNAPSHOT_TIMER_ALLOCATION_NEGATIVE', None)
    if expect_negative:
        env['NATIVE_SNAPSHOT_TIMER_ALLOCATION_NEGATIVE'] = '1'

    def read_log(directory):
        return (directory / 'game.log').read_text(errors='replace')

    try:
        for role in ('host', 'guest'):
            directory = root / role
            directory.mkdir()
            shutil.copy2(binary, directory / 'fallout-ce')
            shutil.copytree(installed_file(data, 'data'), directory / 'DATA')
            for name in ('master.dat', 'critter.dat'):
                archive = installed_file(data, name).resolve()
                (directory / name).symlink_to(archive)
                (directory / name.upper()).symlink_to(archive)
            config = installed_file(data, 'fallout.cfg').read_text()
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
        fault = next((line for line in guest.splitlines()
                      if line.startswith('NATIVE_SNAPSHOT_TIMERS_FAILURE_CONTROL ')), '')
        placement = next((line for line in guest.splitlines()
                          if line.startswith('NATIVE_SNAPSHOT_TIMERS_PLACEMENT_CONTROL ')), '')
        item_positive = next((line for line in guest.splitlines()
                              if line.startswith('NATIVE_SNAPSHOT_TIMERS_ITEM_OWNER_POSITIVE ')), '')
        item_negative = next((line for line in guest.splitlines()
                              if line.startswith('NATIVE_SNAPSHOT_TIMERS_ITEM_OWNER_REJECTION ')), '')
        if item_positive:
            print(item_positive, flush=True)
        if item_negative:
            print(item_negative, flush=True)
        owner = next((line for line in guest.splitlines()
                      if line.startswith('NATIVE_SNAPSHOT_TIMERS_OWNER_REBIND_CONTROL ')), '')
        if owner:
            print(owner, flush=True)
        if fault or not expect_owner_negative:
            print(fault or 'NATIVE_SNAPSHOT_TIMERS_FAILURE_CONTROL_MISSING', flush=True)
        if placement:
            print(placement, flush=True)
        print(f'NATIVE_SNAPSHOT_TIMERS_EXITS host={exits["host"]} guest={exits["guest"]}', flush=True)
        if not re.search(r'queued_used=1 checksummed=1 applied=1 matched=1 restored=1', item_positive):
            raise RuntimeError('native queued item with USED did not match and restore')
        if expect_owner_negative:
            if exits != {'host': 1, 'guest': 1} or not re.search(
                    r'missing_used=1 matching_duplicate=1 checksummed=1 rejected=1 preserved=0 captured=1', item_negative):
                raise RuntimeError('old item timer owner guard negative was not reproduced')
            if not re.search(r'mappings=0 bodies=(\d+)/\1 scripts=(\d+)/\2 queue=1 old_owner_present=0 duplicate_present=1 scripts_run=0 attacks=0 rng=0', item_negative):
                raise RuntimeError('old item owner replacement evidence is missing')
            print('NATIVE_MULTIPLAYER_SNAPSHOT_TIMERS_OWNER_NEGATIVE_CONFIRMED', flush=True)
            return
        if not expect_negative and not re.search(
                r'missing_used=1 matching_duplicate=1 checksummed=1 rejected=1 preserved=1 captured=1', item_negative):
            raise RuntimeError('item timer owner rejection did not preserve native state')
        request_counts = re.search(r'node=(\d+) nested_item=1 expected_requests=(\d+) requests=(\d+) faults=1', fault)
        if request_counts is None or int(request_counts[1]) != (fault_mode == 'node') or request_counts[2] != request_counts[3]:
            raise RuntimeError('native payload/node allocation request was not reached')
        if not re.search(r'queue=1 live_blocks=0 attempts=0 scripts_run=0 attacks=0 rng=0', fault):
            raise RuntimeError('staged native queue memory or execution effects remained')
        if expect_negative:
            if exits != {'host': 1, 'guest': 1} or not re.search(
                    r'requests=\d+ faults=1 rejected=1 preserved=0', fault):
                raise RuntimeError(f'old-code negative was not reproduced:\n{guest[-7000:]}')
            counts = re.search(
                r'bodies=(\d+)/(\d+) bodies_at_fault=(\d+) scripts=(\d+)/(\d+) scripts_at_fault=(\d+)', fault)
            registry = re.search(r'registry=(\d+)/(\d+) mappings=0', fault)
            if counts is None or registry is None:
                raise RuntimeError('old-code native body/script/registry evidence is missing')
            before_bodies, after_bodies, fault_bodies, before_scripts, after_scripts, fault_scripts = map(int, counts.groups())
            if (after_bodies != before_bodies + 4 or fault_bodies != after_bodies
                    or after_scripts != before_scripts + 1 or fault_scripts != after_scripts
                    or int(registry[2]) != int(registry[1]) + 4):
                raise RuntimeError('old-code rejection did not show the expected four bodies and one SID')
            print('NATIVE_MULTIPLAYER_SNAPSHOT_TIMERS_NEGATIVE_CONFIRMED', flush=True)
            return
        if exits != {'host': 0, 'guest': 0}:
            raise RuntimeError(f'fixture requires clean exits from both processes:\n{guest[-7000:]}')
        if not re.search(r'requests=\d+ faults=1 rejected=1 preserved=1 captured=1', fault):
            raise RuntimeError('native allocation rejection did not preserve state')
        if not re.search(r'applied=1 matched=1 exact=1 nested_holder=1 restored=1 attempts=0', placement):
            raise RuntimeError('authoritative placement/nested holder control did not pass')
        if 'NATIVE_SNAPSHOT_TIMERS_OWNER_REBIND_CONTROL staged=1 flags_untouched=1 rebound=1 committed=1 exact=1 restored=1 requests=0 faults=0 passed=1' not in guest:
            raise RuntimeError('prepared owner rebind/commit control did not pass')
        if any('MULTIPLAYER_SMOKE_TEST_PASS ' not in content for content in logs.values()):
            raise RuntimeError('normal native smoke checks did not complete for both processes')
        print('NATIVE_MULTIPLAYER_SNAPSHOT_TIMERS_PASS clean_exits=0/0', flush=True)
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
    parser.add_argument('--source-root', type=pathlib.Path,
                        help='native source matching the built game; defaults to this checkout')
    parser.add_argument('--expect-negative', action='store_true',
                        help='require old-code corruption and explicit host/guest exits 1/1')
    parser.add_argument('--expect-owner-negative', action='store_true',
                        help='require old-code item timer owner replacement and explicit exits 1/1')
    parser.add_argument('--fault', choices=('payload', 'node', 'both'), default='both')
    parser.add_argument('--port', type=int, default=0)
    args = parser.parse_args()
    if args.port != 0 and not 1024 <= args.port <= 65535:
        parser.error('port must be 0 for automatic selection or between 1024 and 65535')
    if args.expect_negative and args.expect_owner_negative:
        parser.error('select one negative control')
    checkout = pathlib.Path(__file__).resolve().parents[1]
    source = (args.source_root or checkout).resolve()
    root = pathlib.Path(tempfile.mkdtemp(prefix='fallout-native-snapshot-timers-', dir='/var/tmp'))
    print(f'NATIVE_SNAPSHOT_TIMERS_ARTIFACTS {root}', flush=True)
    binary = compile_fixture(args.build_directory.resolve(), source, checkout / 'tests', root)
    modes = ('payload', 'node') if args.fault == 'both' else (args.fault,)
    if args.expect_owner_negative:
        modes = (modes[0],)
    for offset, mode in enumerate(modes):
        port = args.port + offset if args.port else 0
        if port > 65535:
            parser.error('the selected pair of ports must be at most 65535')
        if port == 0:
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                port = reservation.getsockname()[1]
        run_root = root / mode
        run_root.mkdir()
        print(f'NATIVE_SNAPSHOT_TIMERS_RUN fault={mode} port={port}', flush=True)
        run_fixture(binary, args.data_root.resolve(), port, run_root, args.expect_negative, mode, args.expect_owner_negative)



if __name__ == '__main__':
    main()
