#!/usr/bin/env python3
"""Run one prepared A/B/C trial with the installed engine and Polkit capture."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import selectors
import socket
import subprocess
import sys
import tempfile
import time


spec = importlib.util.spec_from_file_location('experiment', Path(__file__).with_name('run-gamepad-experiment.py'))
experiment = importlib.util.module_from_spec(spec)
spec.loader.exec_module(experiment)

ENGINE = Path('/usr/local/libexec/barista/barista-engine')
HOSTAPD = Path('/usr/local/libexec/barista/barista-hostapd')
CAPTURE = Path('/usr/libexec/barista/barista-capture-helper')
INPUTS = {'original': 'original.drcrep', 'reencoded': 'reencoded.drcrep', 'live': 'source.mkv'}


def engine_command(trial, source, control, seconds):
    settings = ['PATH=/usr/sbin:/usr/bin:/sbin:/bin', 'LANG=C.UTF-8',
                f'SUDO_UID={os.getuid()}', f'SUDO_GID={os.getgid()}',
                f'DRCD_HOSTAPD_BIN={HOSTAPD}', 'DRCD_AP_TSF_CLOCK=1',
                'DRCD_AP_CHANNEL=149', 'DRCD_LOG_STDERR=1']
    media = ['--play', str(source)] if trial == 'live' else ['--black']
    if trial != 'live':
        settings.append(f'DRCD_REAL_REPLAY={source}')
    # The control socket normally stops the engine; the timeout also bounds
    # its lifetime if the client dies before it can send shutdown.
    return ['pkexec', '/usr/bin/timeout', '--signal=INT', '--kill-after=15s',
            f'{seconds + 360}s', '/usr/bin/env', '-i', *settings, str(ENGINE),
            '--socket', str(control), '--interface', 'wlan0', '--np', *media]


def shutdown(control):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(3)
        connection.connect(str(control))
        connection.sendall(b'shutdown\n')
        connection.shutdown(socket.SHUT_WR)
        return connection.recv(4096)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('--trial', choices=INPUTS, required=True)
    parser.add_argument('--seconds', type=int, choices=range(10, 301), default=60)
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error('Run as the desktop user; privileged processes use Polkit')
    current = experiment.status()
    if current.get('running'):
        parser.error('Stop the current Barista session before a direct replay trial')
    for binary in (ENGINE, HOSTAPD, CAPTURE):
        if not binary.is_file():
            parser.error(f'Missing installed component: {binary}')
    bundle = args.bundle.resolve()
    report = json.loads((bundle / 'comparison.json').read_text())
    name = INPUTS[args.trial]
    source = bundle / name
    if hashlib.sha256(source.read_bytes()).hexdigest() != report['sha256'][name]:
        parser.error('Prepared input changed; regenerate the comparison bundle')
    folder = Path(tempfile.mkdtemp(prefix=f'barista-replay-{args.trial}-'))
    control = folder / 'control.sock'
    (folder / 'trial.json').write_text(json.dumps({'trial': args.trial,
        'bundle': str(bundle), 'input_sha256': report['sha256'][name],
        'engine_sha256': hashlib.sha256(ENGINE.read_bytes()).hexdigest(),
        'seconds': args.seconds}, indent=2) + '\n')
    print(f'Trial {args.trial}; logs: {folder}. Keep GamePad off until READY.', flush=True)
    capture = None
    selector = selectors.DefaultSelector()
    with (folder / 'engine.log').open('x') as engine_log:
        engine = subprocess.Popen(engine_command(args.trial, source, control, args.seconds),
                                  stdout=engine_log, stderr=subprocess.STDOUT)
        try:
            # Wait for the control endpoint, avoiding simultaneous Polkit prompts.
            deadline = time.monotonic() + 90
            while not control.exists():
                if engine.poll() is not None:
                    raise RuntimeError(f'Engine exited ({engine.returncode}); see {folder / "engine.log"}')
                if time.monotonic() > deadline:
                    raise RuntimeError('Engine startup timed out')
                time.sleep(0.25)
            capture = subprocess.Popen(['pkexec', str(CAPTURE), '--ap', 'wlan0',
                                        '--radio', 'wlan1', '--seconds', str(args.seconds)],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, bufsize=1)
            selector.register(capture.stdout, selectors.EVENT_READ)
            with (folder / 'capture.log').open('x') as log:
                while capture.poll() is None or selector.get_map():
                    for key, _ in selector.select(0.25):
                        line = key.fileobj.readline()
                        if not line:
                            selector.unregister(key.fileobj)
                            continue
                        log.write(line)
                        log.flush()
                        if line.startswith('READY:'):
                            experiment.ring_bell()
                            print('READY: Turn on the GamePad. Replay starts automatically; no Cemu needed.', flush=True)
                        else:
                            print(line, end='', flush=True)
                    if engine.poll() is not None:
                        raise RuntimeError(f'Engine exited early; see {folder / "engine.log"}')
            return capture.wait()
        finally:
            if capture is not None:
                # The installed helper watches stdin EOF and restores the radio.
                capture.stdin.close()
                try:
                    capture.wait(timeout=25)
                except subprocess.TimeoutExpired:
                    print('Capture cleanup pending; inspect its log.', file=sys.stderr)
                capture.stdout.close()
            selector.close()
            if engine.poll() is None:
                try:
                    shutdown(control)
                    engine.wait(timeout=30)
                except (OSError, subprocess.TimeoutExpired):
                    print('Engine cleanup pending; its privileged timeout remains active.', file=sys.stderr)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
