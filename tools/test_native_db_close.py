#!/usr/bin/env python3
"""Check native buffered-write failure and subsequent I/O using a built Ninja game."""
import argparse
import os
import pathlib
import shlex
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_directory', type=pathlib.Path)
    args = parser.parse_args()
    if not sys.platform.startswith('linux') or not pathlib.Path('/dev/full').exists():
        parser.error('this native kernel failure probe requires Linux /dev/full')
    build = args.build_directory.resolve()
    source = pathlib.Path(__file__).resolve().parents[1]
    commands = subprocess.check_output(
        ['ninja', '-C', str(build), '-t', 'commands', 'fallout-ce'], text=True, timeout=30)
    link = shlex.split(commands.splitlines()[-1])
    if link[:2] == [':', '&&']:
        link = link[2:]
    if link[-2:] == ['&&', ':']:
        link = link[:-2]
    if '&&' in link or '-o' not in link:
        parser.error('unsupported Ninja link command')
    main_objects = [arg for arg in link if arg.endswith('/plib/gnw/winmain.cc.o')]
    if len(main_objects) != 1:
        parser.error('could not identify the desktop entry point')
    compiler = link[0]
    link = [arg for arg in link if arg not in main_objects
            and not arg.startswith('-Wl,--dependency-file=')]
    with tempfile.TemporaryDirectory(prefix='fallout-native-db-close-',
                                     dir=os.environ.get('TMPDIR')) as directory:
        root = pathlib.Path(directory)
        (root / 'FULL.TMP').symlink_to('/dev/full')
        obj = root / 'probe.o'
        binary = root / 'probe'
        subprocess.run([compiler, '-std=c++17', '-I', str(source / 'src'), '-c',
                        str(source / 'tests/native_db_close_test.cc'), '-o', str(obj)],
                       check=True, timeout=30)
        link[link.index('-o') + 1] = str(binary)
        link.insert(1, str(obj))
        subprocess.run(link, cwd=build, check=True, timeout=60)
        subprocess.run([str(binary), str(root)], check=True, timeout=30)
    print('NATIVE_DB_CLOSE_PASS buffered_failure=reported subsequent_io=intact')


if __name__ == '__main__':
    main()
