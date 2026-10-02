#!/usr/bin/env python3
"""Verify local multiplayer Pip-Boy/inventory windows keep native map processes active."""
import argparse
import pathlib
import socket
import tempfile

import test_native_multiplayer_snapshot_creation as native
from test_native_multiplayer_inventory_events import run_fixture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_directory', type=pathlib.Path)
    parser.add_argument('data_root', type=pathlib.Path)
    parser.add_argument('--source-root', type=pathlib.Path)
    parser.add_argument('--expect-negative', action='store_true')
    parser.add_argument('--port', type=int, default=0)
    args = parser.parse_args()
    if args.port != 0 and not 1024 <= args.port <= 65535:
        parser.error('port must be 0 or between 1024 and 65535')
    checkout = pathlib.Path(__file__).resolve().parents[1]
    source = (args.source_root or checkout).resolve()
    root = pathlib.Path(tempfile.mkdtemp(prefix='fallout-native-local-ui-', dir='/var/tmp'))
    print(f'NATIVE_LOCAL_UI_ARTIFACTS {root}', flush=True)
    native.FIXTURES = {
        'src/multiplayer/network_world.cc': 'native_local_ui_world.cc',
        'src/game/map.cc': 'native_local_ui_map.cc',
        'src/game/pipboy.cc': 'native_local_ui_pipboy.cc',
        'src/game/inventry.cc': 'native_local_ui_inventory.cc',
    }
    binary = native.compile_fixture(args.build_directory.resolve(), source, checkout / 'tests', root)
    port = args.port
    if not port:
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
    during = 0 if args.expect_negative else 1
    markers = (f'screen=pipboy before=1 opened=1 during={during} after=1',
               f'screen=inventory before=1 during={during} after=1')
    run_fixture(binary, args.data_root.resolve(), port, root, 'local-ui', args.expect_negative,
                markers=markers, result_label='NATIVE_LOCAL_UI_BACKGROUND_PASS')


if __name__ == '__main__':
    main()
