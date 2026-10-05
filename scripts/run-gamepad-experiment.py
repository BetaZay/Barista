#!/usr/bin/env python3
"""Build Barista, capture a session, then launch Cemu after GamePad connects."""
import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time


def ring_bell():
    print('\a', end='', flush=True)
    # Tool output may be piped. Also ring an ancestor's terminal when available
    # so a Codex-launched test can notify the original interactive terminal.
    pid = os.getppid()
    for _ in range(16):
        try:
            process = Path('/proc') / str(pid)
            terminal = (process / 'fd/1').resolve()
            if str(terminal).startswith('/dev/pts/') and terminal.stat().st_uid == os.getuid():
                with terminal.open('w') as stream:
                    stream.write('\a')
                return
            fields = (process / 'stat').read_text().rsplit(')', 1)[1].split()
            pid = int(fields[1])
            if pid <= 1:
                return
        except (OSError, ValueError, IndexError):
            return


def status():
    reply = subprocess.run(['busctl', '--json=short', 'call', 'org.barista.Service1',
                            '/org/barista/Service1', 'org.barista.Service1', 'GetStatus'],
                           capture_output=True, text=True, check=True, timeout=10)
    return {key: value['data'] for key, value in json.loads(reply.stdout)['data'][0].items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cemu', type=Path, default=Path('/home/isaiah/Downloads/Cemu-2.8-x86_64.AppImage'))
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=90)
    parser.add_argument('--ap', default='wlan0')
    parser.add_argument('--radio', default='wlan1')
    parser.add_argument('--show-ui', action='store_true', help='Also open Barista’s frontend')
    parser.add_argument('--build-dir', type=Path, help='CMake build directory (default: repository build)')
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error('Run as the desktop user; capture alone uses Polkit')
    if not args.cemu.is_file() or not os.access(args.cemu, os.X_OK) or not args.game.is_file():
        parser.error('Choose an executable Cemu AppImage and existing game file')
    if not 10 <= args.seconds <= 300:
        parser.error('Choose 10..300 capture seconds')
    root = Path(__file__).resolve().parent.parent
    build_dir = (args.build_dir or root / 'build').resolve()
    subprocess.run(['cmake', '--build', str(build_dir), '--parallel', '4'], check=True)
    folder = Path(tempfile.mkdtemp(prefix='barista-experiment-' + datetime.now().strftime('%Y%m%d-%H%M%S-')))
    print(f'Experiment logs: {folder}', flush=True)
    if args.show_ui:
        with (folder / 'barista-ui.log').open('w') as log:
            subprocess.Popen([str(build_dir / 'app/barista')], cwd=root, stdout=log,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    capture = subprocess.Popen(['barista-capture', '--ap', args.ap, '--radio', args.radio,
                                '--seconds', str(args.seconds)], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, bufsize=1)
    selector = selectors.DefaultSelector()
    selector.register(capture.stdout, selectors.EVENT_READ)
    ready = launched = analyzing = False
    last_poll = 0
    cemu = None
    try:
        with (folder / 'capture.log').open('w') as log:
            while capture.poll() is None or selector.get_map():
                for key, _ in selector.select(0.25):
                    line = key.fileobj.readline()
                    if not line:
                        selector.unregister(key.fileobj)
                        continue
                    print(line, end='', flush=True)
                    log.write(line)
                    log.flush()
                    if line.startswith('ANALYZING:'):
                        analyzing = True
                    if line.startswith('READY:'):
                        ring_bell()
                        ready = True
                        print('Waiting for confirmed GamePad connection before launching Cemu.', flush=True)
                if ready and not launched and capture.poll() is None and time.monotonic() - last_poll > 1:
                    last_poll = time.monotonic()
                    current = status()
                    if current.get('running') and current.get('connected'):
                        env = dict(os.environ, BARISTA_MUG_SOCKET=f'/run/barista/media-{os.getuid()}.sock')
                        with (folder / 'cemu.log').open('w') as cemu_log:
                            cemu = subprocess.Popen([str(args.cemu), '-g', str(args.game)],
                                                    cwd=args.cemu.parent, env=env, stdout=cemu_log,
                                                    stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                                    start_new_session=True)
                        launched = True
                        print(f'GamePad connected. Launched {args.cemu.name} with {args.game.name}; PID {cemu.pid}.', flush=True)
                if launched and not analyzing and cemu.poll() is not None:
                    raise RuntimeError(f'Cemu exited early ({cemu.returncode}); inspect {folder / "cemu.log"}')
        result = capture.wait()
        if not launched:
            print('Cemu was not launched: no confirmed GamePad connection during this capture.', file=sys.stderr)
            return result or 1
        return result
    finally:
        if capture.poll() is None:
            capture.terminate()
            capture.wait(timeout=20)
        capture.stdout.close()
        selector.close()


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
