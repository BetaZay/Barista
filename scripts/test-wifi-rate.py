#!/usr/bin/env python3
"""One bounded post-association rate test; existing drcd defaults stay unchanged.

Starts the baseline capture launcher, waits for an active video session, records
a baseline, applies one HT rate mask, then shuts down through the capture launcher's
normal cleanup. No credentials, register probes, queue flushes or channel changes.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time


class StopTest(Exception):
    pass


class StartupRetry(Exception):
    pass


def streaming_ready(log_path):
    """Current-session readiness only, not a guarantee of visible playback.

    Authorization can precede a normal drcd startup retry. Never reuse readiness
    markers from an earlier AP/session in this capture's log.
    """
    try:
        lines = log_path.read_text(errors="replace").splitlines()
    except FileNotFoundError:
        return False
    ready = video = active = False
    for line in lines:
        if any(marker in line for marker in ("stop-session:", "AP-STA-DISCONNECTED",
                "AP-STA-CONNECTED", "runtime-start: AP-ENABLED", "media: stopped")):
            ready = video = active = False
        elif "DRC session ready:" in line:
            ready = True
        elif "Media first frame:" in line:
            video = ", sends OK" in line
        elif "GamePad streaming state:" in line:
            active = "GamePad streaming state: active" in line
    return ready and video and active


def rate_command(interface, mcs):
    # Deliberately the same mask syntax as drcd, but applied to a live station.
    # Legacy rates remain allowed; success is not proof of a fixed on-air rate.
    return ["iw", "dev", interface, "set", "bitrates", "ht-mcs-5", str(mcs), "lgi-5"]


def valid_link(info, station):
    ap = bool(re.search(r"^\s*type AP\s*$", info, re.M))
    five = bool(re.search(r"channel\s+\d+\s+\(5\d{3}\s+MHz\)", info))
    authorized = bool(re.search(r"^\s*authorized:\s*yes\s*$", station, re.M))
    return ap and five and authorized


def commands(args):
    launcher = Path(__file__).with_name("test-media-profile.sh")
    return (["bash", str(launcher), "baseline", "apphook", str(args.output)],
            rate_command(args.interface, args.mcs))


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mcs", type=int, choices=(4, 5, 6))
    parser.add_argument("output", type=Path, help="new PC pcap path")
    parser.add_argument("--interface", default="wlan0", choices=("wlan0",),
                        help="this launcher targets the existing wlan0 setup")
    parser.add_argument("--pad", default="40:d2:8a:ab:90:00")
    parser.add_argument("--baseline-seconds", type=int, default=20, choices=range(5, 121), metavar="5..120")
    parser.add_argument("--seconds", type=int, default=30, choices=range(5, 121), metavar="5..120")
    parser.add_argument("--connect-timeout", type=int, default=120, choices=range(5, 301), metavar="5..300")
    parser.add_argument("--dry-run", action="store_true", help="print plan; no processes, files or Wi-Fi changes")
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,15}", args.interface) or args.interface.startswith("-"):
        parser.error("invalid interface name")
    if not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", args.pad):
        parser.error("invalid GamePad MAC")
    if args.output.suffix != ".pcap":
        parser.error("output must end in .pcap")
    args.output = args.output.absolute()
    return args


def run(args):
    launch, change = commands(args)
    event_path = args.output.with_suffix(".rate.jsonl")
    if args.dry_run:
        print(json.dumps(dict(capture=launch, rate_command=change,
            wait_for="authorized 5 GHz GamePad, DRC handshake, sent video and active streaming flag",
            baseline_seconds=args.baseline_seconds,
            test_seconds=args.seconds, event_file=str(event_path),
            cleanup="stop capture launcher normally; no automatic-rate phase"), indent=2))
        return 0
    if os.geteuid() != 0:
        raise RuntimeError("run with sudo; no hardware changed")
    for tool in ("iw", "bash", "pgrep"):
        if not shutil.which(tool):
            raise RuntimeError("missing tool: " + tool)
    running = subprocess.run(["pgrep", "-x", "barista-engine|drcd"], capture_output=True, timeout=3)
    if running.returncode != 1:
        raise RuntimeError("stop the existing drcd/capture first (or resolve pgrep failure)")
    companions = [args.output, args.output.with_suffix(".log"), event_path,
        args.output.with_name(args.output.stem + "-ip.pcap"),
        args.output.with_name(args.output.stem + "-artifacts"), Path(str(args.output) + ".radio.json")]
    if any(p.exists() for p in companions):
        raise RuntimeError("output or companion already exists; choose a new capture name")
    # Exclusive creation: never overwrite an earlier test's timeline.
    with event_path.open("x") as events:
        def record(event, **details):
            row = dict(event=event, unix_ns=time.time_ns(), monotonic_ns=time.monotonic_ns(),
                       utc=datetime.now(timezone.utc).isoformat(), **details)
            events.write(json.dumps(row) + "\n")
            events.flush()
            print("rate-test: " + json.dumps(row), flush=True)

        def query(command):
            result = subprocess.run(command, capture_output=True, text=True, timeout=3,
                                    env={**os.environ, "LC_ALL": "C"})
            return result.stdout if result.returncode == 0 else ""

        def link_snapshot():
            return (query(["iw", "dev", args.interface, "info"]),
                    query(["iw", "dev", args.interface, "station", "get", args.pad]))

        child = None
        failure = None
        old_handlers = {}
        def interrupted(signum, frame):
            raise StopTest("interrupted by signal " + str(signum))
        try:
            for sig in (signal.SIGINT, signal.SIGTERM):
                old_handlers[sig] = signal.signal(sig, interrupted)
            env = dict(os.environ, LC_ALL="C")
            env.pop("DRCD_TEST_DRY_RUN", None)
            record("capture_start", command=launch, requested_mcs=args.mcs)
            child = subprocess.Popen(launch, env=env, start_new_session=True)

            def check_capture():
                if child.poll() is not None:
                    raise StopTest("capture exited with status " + str(child.returncode))

            deadline = time.monotonic() + args.connect_timeout
            log_path = args.output.with_suffix(".log")

            def phase(name, seconds, startup=False):
                record(name + "_start", seconds=seconds)
                end = time.monotonic() + seconds
                next_sample = time.monotonic()
                while time.monotonic() < end:
                    check_capture()
                    info, station = link_snapshot()
                    if startup and (not valid_link(info, station) or not streaming_ready(log_path)):
                        raise StartupRetry("startup readiness lost; wait for reconnect and restart baseline")
                    if not valid_link(info, station):
                        raise StopTest("GamePad disconnected or AP/channel changed; ending test")
                    if time.monotonic() >= next_sample:
                        record("station_sample", phase=name, station=station)
                        next_sample = time.monotonic() + 2
                    time.sleep(min(.5, max(0, end - time.monotonic())))
                record(name + "_end")

            # Before a rate has changed, allow drcd's existing startup retries.
            # Once a rate is applied, a disconnect still ends the experiment.
            while True:
                while time.monotonic() < deadline:
                    check_capture()
                    info, station = link_snapshot()
                    if valid_link(info, station) and streaming_ready(log_path):
                        record("streaming_ready", interface_info=info, station=station)
                        break
                    time.sleep(.25)
                else:
                    raise StopTest("timed out waiting for an active video session; no test rate applied")
                try:
                    phase("baseline", args.baseline_seconds, startup=True)
                    if not valid_link(*link_snapshot()) or not streaming_ready(log_path):
                        raise StartupRetry("readiness lost at baseline end")
                    break
                except StartupRetry as error:
                    record("baseline_restart", reason=str(error))
            check_capture()
            if not valid_link(*link_snapshot()):
                raise StopTest("association lost before rate change")
            record("rate_request", command=change)
            result = subprocess.run(change, capture_output=True, text=True, timeout=5)
            record("rate_result", returncode=result.returncode, stdout=result.stdout, stderr=result.stderr,
                   note="Request acceptance only; verify actual PHY rates in independent Mac capture")
            if result.returncode:
                raise StopTest("driver rejected requested rate")
            phase("mcs" + str(args.mcs), args.seconds)
        except (StopTest, OSError, subprocess.SubprocessError) as error:
            failure = str(error)
            record("test_stopped", reason=failure)
        finally:
            # Let the existing launcher restore RTS/rate settings and stop drcd.
            # Do not SIGKILL it while it may be restoring interface state.
            for sig in old_handlers:
                signal.signal(sig, signal.SIG_IGN)
            try:
                if child is not None and child.poll() is None:
                    record("cleanup_start")
                    child.send_signal(signal.SIGINT)
                    try:
                        child.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        failure = "capture cleanup still running; do not start another test yet"
                        record("cleanup_pending", pid=child.pid, reason=failure)
                if child is not None and child.poll() is not None:
                    record("capture_exit", returncode=child.returncode)
                    if child.returncode not in (0, 130) and failure is None:
                        failure = "capture cleanup exited unsuccessfully"
            finally:
                for sig, handler in old_handlers.items():
                    signal.signal(sig, handler)
        record("finished", success=failure is None)
        if failure:
            print("rate-test: " + failure, file=sys.stderr)
        return int(failure is not None)


if __name__ == "__main__":
    try:
        raise SystemExit(run(parse_args()))
    except (OSError, RuntimeError) as error:
        raise SystemExit("rate-test: " + str(error))
