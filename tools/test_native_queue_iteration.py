#!/usr/bin/env python3
"""Check callback-safe native queue traversal with AddressSanitizer on queue.cc."""
import argparse
import os
import pathlib
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_directory', type=pathlib.Path)
    args = parser.parse_args()
    build = args.build_directory.resolve()
    source = pathlib.Path(__file__).resolve().parents[1]
    commands = subprocess.check_output(
        ['ninja', '-C', str(build), '-t', 'commands', 'fallout-ce'], text=True, timeout=30).splitlines()
    compile_line = next(line for line in commands
                        if ' -c ' in line and line.endswith('/src/game/queue.cc'))
    link = shlex.split(commands[-1])
    if link[:2] == [':', '&&']:
        link = link[2:]
    if link[-2:] == ['&&', ':']:
        link = link[:-2]
    if '&&' in link or '-o' not in link:
        parser.error('unsupported Ninja link command')
    excluded = [arg for arg in link
                if arg.endswith(('/game/queue.cc.o', '/plib/gnw/winmain.cc.o'))]
    if len(excluded) != 2:
        parser.error('could not identify native queue and desktop entry objects')
    link = [arg for arg in link if arg not in excluded
            and not arg.startswith('-Wl,--dependency-file=')]
    with tempfile.TemporaryDirectory(prefix='fallout-native-queue-iteration-',
                                     dir=os.environ.get('TMPDIR')) as directory:
        root = pathlib.Path(directory)
        obj = root / 'probe.o'
        binary = root / 'probe'
        compile_args = shlex.split(compile_line)
        compile_args[1:1] = ['-I', str(source / 'src')]
        compile_args[compile_args.index('-c') + 1] = str(source / 'tests/native_queue_iteration_test.cc')
        compile_args[compile_args.index('-o') + 1] = str(obj)
        compile_args[compile_args.index('-MF') + 1] = str(root / 'probe.d')
        compile_args += ['-fsanitize=address', '-g', '-O1']
        subprocess.run(compile_args, cwd=build, check=True, timeout=60)
        link[link.index('-o') + 1] = str(binary)
        link.insert(1, str(obj))
        link.append('-fsanitize=address')
        subprocess.run(link, cwd=build, check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=30,
                       env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
    print('NATIVE_QUEUE_ITERATION_PASS sanitizer_scope=native_queue callback_mutation=safe')


if __name__ == '__main__':
    main()
