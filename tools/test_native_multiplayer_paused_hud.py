#!/usr/bin/env python3
"""Verify a changed HUD item refreshes while native multiplayer dialogue processing is paused."""
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
    parser.add_argument('--port', type=int, default=0)
    args = parser.parse_args()
    if args.port != 0 and not 1024 <= args.port <= 65535:
        parser.error('port must be 0 or between 1024 and 65535')
    checkout = pathlib.Path(__file__).resolve().parents[1]
    source = (args.source_root or checkout).resolve()
    root = pathlib.Path(tempfile.mkdtemp(prefix='fallout-native-paused-hud-', dir='/var/tmp'))
    print(f'NATIVE_PAUSED_HUD_ARTIFACTS {root}', flush=True)
    native.FIXTURES = {'src/multiplayer/network_world.cc': 'native_paused_hud_world.cc',
                       'src/game/intface.cc': 'native_paused_hud_intface.cc'}
    binary = native.compile_fixture(args.build_directory.resolve(), source, checkout / 'tests', root)
    port = args.port
    if not port:
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
    run_fixture(binary, args.data_root.resolve(), port, root, 'paused-hud', False,
                markers=('NATIVE_PAUSED_HUD_CONTROL changed_item=1 result=0 paused=1 animation_unchanged=1 refreshed=1',),
                result_label='NATIVE_PAUSED_HUD_PASS')


if __name__ == '__main__':
    main()
