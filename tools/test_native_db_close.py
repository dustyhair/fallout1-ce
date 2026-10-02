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
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--save-copy', action='store_true',
                      help='exercise native save backup copying, including its close failure')
    mode.add_argument('--save-backup', action='store_true',
                      help='exercise failed backup creation without changing original slot files')
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
    if args.save_copy or args.save_backup:
        main_objects += [arg for arg in link if arg.endswith('/game/loadsave.cc.o')]
    link = [arg for arg in link if arg not in main_objects
            and not arg.startswith('-Wl,--dependency-file=')]
    with tempfile.TemporaryDirectory(prefix='fallout-native-db-close-',
                                     dir=os.environ.get('TMPDIR')) as directory:
        root = pathlib.Path(directory)
        (root / 'FULL.TMP').symlink_to('/dev/full')
        if args.save_backup:
            (root / 'SAVEGAME' / 'SLOT01' / 'MAP.BAK').mkdir(parents=True)
            (root / 'SAVEGAME' / 'SLOT01' / 'MAP.BAK' / 'blocker').write_text('backup destination unavailable\n')
        obj = root / 'probe.o'
        binary = root / 'probe'
        if args.save_copy or args.save_backup:
            compile_line = next(line for line in commands.splitlines()
                                if ' -c ' in line and line.endswith('/src/game/loadsave.cc'))
            compile_args = shlex.split(compile_line)
            compile_args[compile_args.index('-o') + 1] = str(obj)
            test_name = 'native_save_backup_test.cc' if args.save_backup else 'native_save_copy_test.cc'
            compile_args[compile_args.index('-c') + 1] = str(source / 'tests' / test_name)
            compile_args[compile_args.index('-MF') + 1] = str(root / 'probe.d')
            # Prefer this checkout's loadsave.cc over the build's source tree.
            compile_args[1:1] = ['-I', str(source / 'src')]
        else:
            compile_args = [compiler, '-std=c++17', '-I', str(source / 'src'), '-c',
                            str(source / 'tests/native_db_close_test.cc'), '-o', str(obj)]
        subprocess.run(compile_args, cwd=build, check=True, timeout=60)
        link[link.index('-o') + 1] = str(binary)
        link.insert(1, str(obj))
        subprocess.run(link, cwd=build, check=True, timeout=60)
        subprocess.run([str(binary), str(root)], check=True, timeout=30)
    label = ('NATIVE_SAVE_BACKUP_PASS' if args.save_backup else
             'NATIVE_SAVE_COPY_PASS' if args.save_copy else 'NATIVE_DB_CLOSE_PASS')
    detail = ('backup_failure=reported originals=intact rollback=intact' if args.save_backup else
              'buffered_failure=reported subsequent_io=intact')
    print(f'{label} {detail}')


if __name__ == '__main__':
    main()
