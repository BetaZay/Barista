#!/usr/bin/env bash

set -euo pipefail

usage()
{
	cat <<'EOF'
Usage: sudo watch-gamepad-probes.sh [interface] [gamepad-mac] [dwell-ms] [pcap]

Passively hop Wii U 5 GHz channels and capture every frame sent by a GamePad.

Defaults:
  interface    wlan0
  gamepad-mac  40:d2:8a:ab:90:00
  dwell-ms     250
  pcap         /tmp/drc-gamepad-probes-<timestamp>.pcap

Set DRC_CHANNELS to override the channel list, for example:
  DRC_CHANNELS="36 40 44 48" sudo -E ./scripts/watch-gamepad-probes.sh
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

interface=${1:-wlan0}
gamepad_mac=${2:-40:d2:8a:ab:90:00}
dwell_ms=${3:-250}
output_path=${4:-/tmp/drc-gamepad-probes-$(date +%Y%m%d-%H%M%S).pcap}
channels=${DRC_CHANNELS:-"36 40 44 48 149 153 157 161 165"}

if [[ ! $interface =~ ^[a-zA-Z0-9_.:-]+$ ]]; then
	echo "error: invalid interface name: $interface" >&2
	exit 1
fi
if [[ ! $gamepad_mac =~ ^([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}$ ]]; then
	echo "error: invalid GamePad MAC address: $gamepad_mac" >&2
	exit 1
fi
if [[ ! $dwell_ms =~ ^[0-9]+$ ]] || ((dwell_ms < 50 || dwell_ms > 60000)); then
	echo "error: dwell-ms must be between 50 and 60000" >&2
	exit 1
fi

for required_command in ip iw tcpdump awk; do
	if ! command -v "$required_command" >/dev/null 2>&1; then
		echo "error: required command not found: $required_command" >&2
		exit 1
	fi
done

if ! iw dev "$interface" info >/dev/null 2>&1; then
	echo "error: wireless interface does not exist: $interface" >&2
	exit 1
fi

original_type=$(iw dev "$interface" info | awk '$1 == "type" { print $2; exit }')
if [[ -z $original_type ]]; then
	echo "error: could not determine interface type" >&2
	exit 1
fi

read -r interface_flags < "/sys/class/net/$interface/flags"
was_up=$((interface_flags & 1))
dwell_seconds=$(awk -v milliseconds="$dwell_ms" 'BEGIN { printf "%.3f", milliseconds / 1000 }')
capture_pid=
display_pid=

cleanup()
{
	trap - EXIT INT TERM
	if [[ -n $display_pid ]]; then
		kill "$display_pid" >/dev/null 2>&1 || true
		wait "$display_pid" >/dev/null 2>&1 || true
	fi
	if [[ -n $capture_pid ]]; then
		kill -INT "$capture_pid" >/dev/null 2>&1 || true
		wait "$capture_pid" >/dev/null 2>&1 || true
	fi

	ip link set "$interface" down >/dev/null 2>&1 || true
	iw dev "$interface" set type "$original_type" >/dev/null 2>&1 || true
	if ((was_up)); then
		ip link set "$interface" up >/dev/null 2>&1 || true
	fi

	echo
	echo "watch: stopped; capture saved to $output_path"
}

trap cleanup EXIT
trap 'exit 130' INT TERM

ip link set "$interface" down
iw dev "$interface" set type monitor
ip link set "$interface" up

# Do not restrict this to Probe Request frames. Pairing discovery may use a
# vendor-specific action or control frame, and the purpose of this watcher is
# to establish what the GamePad actually transmits while the sync UI is open.
capture_filter="wlan addr2 $gamepad_mac"

echo "watch: interface=$interface gamepad=$gamepad_mac dwell=${dwell_ms}ms"
echo "watch: channels=$channels"
echo "watch: pcap=$output_path"
echo "watch: press Ctrl-C to stop and restore type '$original_type'"

tcpdump -q -U -s 0 -i "$interface" -w "$output_path" "$capture_filter" \
	>/dev/null 2>&1 &
capture_pid=$!

# A second capture socket provides immediate readable output while the first
# preserves full radiotap headers and frame bytes for later analysis.
tcpdump -l -tttt -nn -e -i "$interface" "$capture_filter" &
display_pid=$!

sleep 0.25
if ! kill -0 "$capture_pid" >/dev/null 2>&1 || ! kill -0 "$display_pid" >/dev/null 2>&1; then
	echo "error: tcpdump failed to start" >&2
	exit 1
fi

current_channel=
while true; do
	for channel in $channels; do
		if [[ $channel != "$current_channel" ]]; then
			if iw dev "$interface" set channel "$channel" HT20; then
				current_channel=$channel
				printf 'watch: channel=%s\n' "$channel"
			else
				printf 'watch: channel=%s unavailable; skipping\n' "$channel" >&2
				continue
			fi
		fi
		sleep "$dwell_seconds"
	done
done
