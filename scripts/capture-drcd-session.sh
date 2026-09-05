#!/usr/bin/env bash

set -euo pipefail

usage()
{
	cat <<'EOF'
Usage: sudo capture-drcd-session.sh [pcap] [drcd arguments...]

Run drcd and capture its runtime radiotap monitor at the same time. This keeps
802.11 QoS, RTS/CTS, ACK, BlockAck, rate, and retry information for comparison
with a real Wii U capture.
Also records decrypted DRC UDP traffic to a companion *-ip.pcap file. This
reveals actual payloads and host send timing without the runtime Wi-Fi key.
Radio capture follows monitor recreation, recording complete packets into one
pcap. A companion *.pcap.radio.json records attachment events and capture gaps.

Defaults:
  pcap            /tmp/drcd-radio-<timestamp>.pcap
  drcd arguments  --np --play ./test1.mkv

Example:
  sudo ./scripts/capture-drcd-session.sh /tmp/drcd-qos-test.pcap
  sudo ./scripts/capture-drcd-session.sh /tmp/drcd-black.pcap --np --black

Press Ctrl-C after the GamePad succeeds or displays the connection error.
EOF
}

if [[ ${1:-} == "-h" || ${1:-} == "--help" ]]; then
	usage
	exit 0
fi

if [[ $EUID -ne 0 ]]; then
	echo "error: this script must run as root (use sudo)" >&2
	exit 1
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
output_path=${1:-/tmp/drcd-radio-$(date +%Y%m%d-%H%M%S).pcap}
ip_output_path=${output_path%.pcap}-ip.pcap
if (($# > 0)); then
	shift
fi
drcd_binary=$project_dir/build/drcd/drcd
monitor_interface=${DRC_TSF_MONITOR_IFACE:-drcdtsf}

if (($# == 0)); then
	set -- --np --play "$project_dir/test1.mkv"
fi
if [[ -e $output_path || -e $ip_output_path || -e $output_path.radio.json ]]; then
	echo "error: output or companion IP capture already exists: $output_path / $ip_output_path" >&2
	exit 1
fi
if [[ ! -x $drcd_binary ]]; then
	echo "error: drcd binary not found: $drcd_binary" >&2
	exit 1
fi
for required_command in iw tcpdump awk setsid python3; do
	if ! command -v "$required_command" >/dev/null 2>&1; then
		echo "error: required command not found: $required_command" >&2
		exit 1
	fi
done

drcd_pid=
capture_pid=
ip_capture_pid=
capture_started=0

packet_count()
{
	if [[ ! -s $output_path ]]; then
		printf '0'
		return
	fi
	tcpdump -nn -r "$output_path" 2>/dev/null | awk 'END { print NR + 0 }'
}

cleanup()
{
	local exit_status=$?
	trap - EXIT INT TERM

	if [[ -n $capture_pid ]]; then
		if kill -0 "$capture_pid" >/dev/null 2>&1; then
			kill -INT "$capture_pid" >/dev/null 2>&1 || true
		fi
		if ! wait "$capture_pid"; then
			echo "error: radio capture supervisor failed; inspect $output_path.radio.json" >&2
			if ((exit_status == 0)); then exit_status=1; fi
		fi
	fi
	if [[ -n $drcd_pid ]] && kill -0 "$drcd_pid" >/dev/null 2>&1; then
		kill -INT "$drcd_pid" >/dev/null 2>&1 || true
		wait "$drcd_pid" >/dev/null 2>&1 || true
	fi
	if [[ -n $ip_capture_pid ]] && kill -0 "$ip_capture_pid" >/dev/null 2>&1; then
		kill -INT "$ip_capture_pid" >/dev/null 2>&1 || true
		wait "$ip_capture_pid" >/dev/null 2>&1 || true
	fi
	if [[ -s $ip_output_path ]]; then
		echo "capture: decrypted UDP pcap=$ip_output_path"
	fi

	if ((capture_started)) && [[ -s $output_path ]]; then
		echo
		echo "capture: stopped; packets=$(packet_count)"
		echo "capture: pcap=$output_path"
	else
		echo
		echo "capture: stopped before any radio packets were saved" >&2
	fi
	exit "$exit_status"
}

trap cleanup EXIT
trap 'exit 130' INT TERM

# Start before drcd so even a GamePad reconnecting immediately is captured.
# 'any' survives AP interface setup; restrict collection to DRC endpoints/UDP.
setsid tcpdump -q -U -s 0 -i any -w "$ip_output_path" \
	'udp and host 192.168.1.10 and host 192.168.1.11 and portrange 50000-50199' &
ip_capture_pid=$!
sleep 0.2
if ! kill -0 "$ip_capture_pid" >/dev/null 2>&1; then
	echo "error: could not start decrypted UDP capture" >&2
	exit 1
fi

echo "capture: starting $drcd_binary $*"
# Keep the daemon and its hostapd child out of the terminal's foreground
# process group.  Ctrl-C must reach drcd first so it can restore rate/RTS
# settings before it deliberately stops hostapd and tears the interface down.
setsid "$drcd_binary" "$@" &
drcd_pid=$!

echo "capture: waiting for runtime monitor $monitor_interface"
setsid python3 "$script_dir/capture-radio-follow.py" "$monitor_interface" "$output_path" &
capture_pid=$!
capture_started=1
echo "capture: recording drcd radio traffic to $output_path"
echo "capture: turn on the GamePad; press Ctrl-C after the result is visible"

# The daemon can recreate drcdtsf without exiting. The radio follower tracks
# ifindex changes; watchdog both capture processes for the entire session.
while kill -0 "$drcd_pid" >/dev/null 2>&1; do
	if ! kill -0 "$capture_pid" >/dev/null 2>&1; then
		wait "$capture_pid" || true
		echo "error: radio capture stopped while drcd was running" >&2
		exit 1
	fi
	if ! kill -0 "$ip_capture_pid" >/dev/null 2>&1; then
		wait "$ip_capture_pid" || true
		echo "error: decrypted UDP capture stopped while drcd was running" >&2
		exit 1
	fi
	sleep 0.2
done

set +e
wait "$drcd_pid"
drcd_status=$?
set -e
drcd_pid=
exit "$drcd_status"
