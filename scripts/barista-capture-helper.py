#!/usr/bin/python3 -I
"""Installed Polkit helper: bounded paired capture, authenticated offline analysis."""
import argparse
import ctypes
import fcntl
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace

# -I excludes user/site imports. Only the root-owned installation is imported.
sys.path.insert(0, str(Path(__file__).resolve().parent / 'capture-analysis'))
import decrypt_capture as decrypt
import analyze_delivery as delivery
import analyze_format_clock as timing

BASE = Path('/var/lib/barista-captures')
CREDENTIALS = Path('/var/lib/drcd/credentials.conf')
ENV = {'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'LANG': 'C.UTF-8'}
MONITOR = 'baristasniff'


def command(*args, check=True):
    return subprocess.run(args, env=ENV, check=check, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)


def validate_interfaces(ap, radio):
    for name in (ap, radio):
        if not re.fullmatch(r'[a-zA-Z0-9_.:-]{1,15}', name):
            raise ValueError('Invalid interface name')
        if not Path('/sys/class/net', name, 'phy80211').exists():
            raise ValueError('Choose existing wireless interfaces')
    if Path('/sys/class/net', ap, 'phy80211').resolve() == Path('/sys/class/net', radio, 'phy80211').resolve():
        raise ValueError('AP and capture adapters must use different radios')


def stop(process):
    if process is None:
        return
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def save_report(path, result):
    with path.open('x') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')


def watch_client():
    # QProcess keeps a stdin pipe open. EOF also catches pkexec implementations
    # which fork a supervisor, where the direct parent can outlive the client.
    try:
        while os.read(0, 1):
            pass
    except OSError:
        pass
    os.kill(os.getpid(), signal.SIGTERM)


def analyze(folder, ap_mac, pad_mac, pmk):
    radio = folder / 'radio.pcap'
    host = folder / 'host-ip.pcap'
    args = SimpleNamespace(capture=radio, output=folder / 'decrypted-radio.pcap',
                           ip_output=folder / 'decrypted-ip.pcap')
    if decrypt.decrypt_with_pmk(args, pmk) != 0:
        raise ValueError('No authenticated handshake; keep GamePad off until capture is ready')
    args = SimpleNamespace(radio=radio, host=host, credentials=CREDENTIALS, ap=ap_mac)
    delivered = delivery.analyze(args)
    save_report(folder / 'delivery.json', delivered)
    args = SimpleNamespace(radio=radio, decrypted_radio=folder / 'decrypted-radio.pcap',
                           credentials=None, ap=ap_mac, pad=pad_mac)
    clock = timing.analyze(args)
    save_report(folder / 'format-clock.json', clock)
    summary = dict(complete_radio_frames=delivered['matched_video_frames_complete'],
                   incomplete_radio_frames=delivered['matched_video_frames_incomplete'],
                   fully_block_acked_frames=delivered['block_ack_evidence']['video_frames_fully_block_acked'],
                   recovery_requests=delivered['counts'].get('unique_air_recovery_requests', 0),
                   format_age_us=clock['format_age_us'],
                   formats_at_least_8000us=clock['format_age_at_least_8000us'],
                   format_to_video_lead_us=clock['format_to_same_timestamp_video_first_us'],
                   limitations=delivered['limitations'])
    save_report(folder / 'summary.json', summary)
    print('SUMMARY: ' + json.dumps(summary), flush=True)
    # Keep the established protocol and PHY summaries alongside delivery evidence.
    for script, source, target in [('analyze-media-capture.py', host, 'host-media.json'),
                                   ('summarize-wifi-rates.py', radio, 'wifi-rates.json')]:
        argv = ['/usr/bin/python3', '-I', str(Path(__file__).resolve().parent / 'capture-analysis' / script), str(source)]
        if script == 'summarize-wifi-rates.py':
            argv += ['--ap', ap_mac, '--pad', pad_mac]
        result = subprocess.run(argv, env=ENV, capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise ValueError('Capture summary failed: ' + script)
        save_report(folder / target, json.loads(result.stdout))


def run(args):
    if os.geteuid() != 0 or not os.environ.get('PKEXEC_UID', '').isdigit():
        raise ValueError('Use barista-capture through the installed Polkit action')
    uid = int(os.environ['PKEXEC_UID'])
    if uid == 0:
        raise ValueError('Run the capture client as your desktop user')
    # If the client exits or is interrupted, restore the radio instead of orphaning it.
    parent = os.getppid()
    if ctypes.CDLL(None, use_errno=True).prctl(1, signal.SIGTERM, 0, 0, 0) != 0:
        raise OSError('Cannot bind capture lifetime to client')
    if os.getppid() != parent or parent == 1:
        raise ValueError('Capture client disconnected')
    threading.Thread(target=watch_client, daemon=True).start()
    os.umask(0o077)
    validate_interfaces(args.ap, args.radio)
    # Serialize adapter changes. Never accept caller-controlled output paths or commands.
    lock_dir = Path('/run/barista-capture')
    lock_dir.mkdir(mode=0o700, exist_ok=True)
    with (lock_dir / 'capture.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return capture(args, uid)


def capture(args, uid):
    fields = dict(line.split('=', 1) for line in CREDENTIALS.read_text().splitlines() if '=' in line)
    secret, pad = fields.get('psk', ''), fields.get('gamepad_mac', '')
    if not re.fullmatch(r'[0-9a-fA-F]{64}', secret) or not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}', pad):
        raise ValueError('Saved pairing credentials are missing or invalid')
    pmk = bytes.fromhex(secret)
    del secret, fields
    deadline = time.monotonic() + 60
    while True:
        info = command('iw', 'dev', args.ap, 'info').stdout
        channel = re.search(r'channel (\d+) ', info)
        ap_mac = re.search(r'addr ([0-9a-f:]{17})', info)
        if 'type AP' in info and channel and ap_mac:
            break
        if time.monotonic() >= deadline:
            raise ValueError('Barista AP did not become ready within 60 seconds')
        time.sleep(0.25)
    ap_mac = ap_mac[1]
    if Path('/sys/class/net', MONITOR).exists():
        raise ValueError('Capture monitor already exists; refusing to modify it')
    BASE.mkdir(mode=0o755, parents=True, exist_ok=True)
    os.chmod(BASE, 0o755)
    folder = Path(tempfile.mkdtemp(prefix=time.strftime('%Y%m%d-%H%M%S-'), dir=BASE))
    was_up = int(Path('/sys/class/net', args.radio, 'flags').read_text(), 16) & 1
    managed = command('nmcli', '-g', 'GENERAL.NM-MANAGED', 'device', 'show', args.radio, check=False).stdout.strip() == 'yes'
    released = created = False
    processes, streams = [], []
    success = False
    began = time.time()
    try:
        if managed:
            command('nmcli', 'device', 'set', args.radio, 'managed', 'no')
            released = True
        command('ip', 'link', 'set', args.radio, 'down')
        command('iw', 'dev', args.radio, 'interface', 'add', MONITOR, 'type', 'monitor', 'flags', 'control', 'otherbss')
        created = True
        # rtw89 requires both receive interfaces activated before the tuning
        # cycle, matching capture-wiiu-session.sh's proven setup order.
        command('ip', 'link', 'set', args.radio, 'up')
        command('ip', 'link', 'set', MONITOR, 'up')
        command('ip', 'link', 'set', args.radio, 'down')
        command('iw', 'dev', MONITOR, 'set', 'channel', channel[1], 'HT20')
        command('ip', 'link', 'set', args.radio, 'up')
        command('ip', 'link', 'set', MONITOR, 'up')
        for interface, name, filters in [(MONITOR, 'radio', []), (args.ap, 'host-ip', ['udp', 'and', 'portrange', '50000-50199'])]:
            output = (folder / (name + '.pcap')).open('xb')
            log = (folder / (name + '-tcpdump.log')).open('xb')
            streams += [output, log]
            processes.append(subprocess.Popen(['tcpdump', '-nn', '-U', '-B', '4096', '-s', '0', '-i', interface, '-w', '-', *filters],
                                              stdout=output, stderr=log, env=ENV))
        time.sleep(0.5)
        if any(p.poll() is not None for p in processes):
            raise ValueError('tcpdump could not start; see capture logs')
        print('READY: Turn on the GamePad, then launch Cemu and your game. Waiting up to 120 seconds for GamePad traffic.', flush=True)
        probe = subprocess.Popen(['tcpdump', '-nn', '-i', MONITOR, '-c', '1', 'wlan', 'addr2', pad, 'and', 'type', 'data'],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=ENV)
        try:
            probe.wait(timeout=120)
            if probe.returncode:
                raise ValueError('GamePad detection failed')
        finally:
            stop(probe)
        print(f'RECORDING: {args.seconds} seconds; keep the game running.', flush=True)
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            if any(p.poll() is not None for p in processes):
                raise ValueError('A capture process stopped early')
            time.sleep(0.25)
        success = True
    finally:
        for process in processes:
            stop(process)
        for stream in streams:
            stream.close()
        if created:
            command('ip', 'link', 'set', args.radio, 'down', check=False)
            command('iw', 'dev', MONITOR, 'del', check=False)
        command('ip', 'link', 'set', args.radio, 'up' if was_up else 'down', check=False)
        if released:
            command('nmcli', 'device', 'set', args.radio, 'managed', 'yes', check=False)
        save_report(folder / 'session.json', dict(ap_interface=args.ap, radio_interface=args.radio,
                    ap_mac=ap_mac, gamepad_mac=pad, channel=int(channel[1]), started=began,
                    stopped=time.time(), capture_complete=success, seconds=args.seconds))
        # Files stay root-owned until all privileged writes and analyses finish.
        try:
            if success:
                print('ANALYZING: authenticating handshake, decrypting, matching delivery and timing.', flush=True)
                analyze(folder, ap_mac, pad, pmk)
        finally:
            for file in folder.iterdir():
                os.chown(file, uid, -1)
            os.chown(folder, uid, -1)
            print(f'RESULT: {folder}', flush=True)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ap', default='wlan0')
    parser.add_argument('--radio', default='wlan1')
    parser.add_argument('--seconds', type=int, choices=range(10, 301), default=60, metavar='10..300')
    args = parser.parse_args()
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    try:
        return run(args)
    except KeyboardInterrupt:
        print('Capture interrupted.', file=sys.stderr)
        return 130
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f'Capture failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
