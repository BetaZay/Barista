#!/usr/bin/env bash

set -euo pipefail

usage()
{
	cat <<'EOF'
Usage: sudo capture-wiiu-session.sh [interface] [gamepad-mac] [channel|auto] [pcap]

Passively capture a real Wii U <-> GamePad session with radiotap headers.
The selected base interface is temporarily released from NetworkManager. A
dedicated monitor child is created and removed when the script exits.

Defaults:
  interface    wlan0
  gamepad-mac  40:d2:8a:ab:90:00
  channel      auto
  pcap         /tmp/wiiu-gamepad-<timestamp>.pcap

Auto mode hops the 5 GHz channel plan until it sees a frame transmitted
by the GamePad, locks to that channel, and starts an unfiltered capture. An
unfiltered capture is intentional: it retains 802.11 ACK/BlockAck frames that
do not carry the GamePad MAC address.

Environment:
  DRC_CHANNELS       channels scanned by auto mode
                     (default: all standard 5 GHz 20 MHz channels)
  DRC_SCAN_DWELL_MS  time spent on each channel (default: 500)
  DRC_MONITOR_IFACE  temporary monitor interface name (default: drcsniff)

For the most complete trace, first use auto mode to discover the channel, then
stop and rerun with that fixed channel before powering on the GamePad.
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
requested_channel=${3:-auto}
output_path=${4:-/tmp/wiiu-gamepad-$(date +%Y%m%d-%H%M%S).pcap}
channels=${DRC_CHANNELS:-"36 40 44 48 52 56 60 64 100 104 108 112 116 120 124 128 132 136 140 144 149 153 157 161 165"}
dwell_ms=${DRC_SCAN_DWELL_MS:-500}
monitor_interface=${DRC_MONITOR_IFACE:-drcsniff}
metadata_path=${output_path}.txt

if [[ ! $interface =~ ^[a-zA-Z0-9_.:-]+$ ]]; then
	echo "error: invalid interface name: $interface" >&2
	exit 1
fi
if [[ ! $monitor_interface =~ ^[a-zA-Z0-9_.:-]+$ ]] || ((${#monitor_interface} > 15)); then
	echo "error: invalid DRC_MONITOR_IFACE name: $monitor_interface" >&2
	exit 1
fi
if [[ $monitor_interface == "$interface" ]]; then
	echo "error: DRC_MONITOR_IFACE must differ from the base interface" >&2
	exit 1
fi
if [[ ! $gamepad_mac =~ ^([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}$ ]]; then
	echo "error: invalid GamePad MAC address: $gamepad_mac" >&2
	exit 1
fi
if [[ $requested_channel != auto && ! $requested_channel =~ ^[0-9]+$ ]]; then
	echo "error: channel must be a positive number or 'auto'" >&2
	exit 1
fi
if [[ $requested_channel != auto ]] && ((requested_channel <= 0)); then
	echo "error: channel must be a positive number or 'auto'" >&2
	exit 1
fi
if [[ ! $dwell_ms =~ ^[0-9]+$ ]] || ((dwell_ms < 100 || dwell_ms > 60000)); then
	echo "error: DRC_SCAN_DWELL_MS must be between 100 and 60000" >&2
	exit 1
fi
if [[ -e $output_path || -e $metadata_path ]]; then
	echo "error: output already exists: $output_path or $metadata_path" >&2
	exit 1
fi
if [[ ! -d $(dirname "$output_path") ]]; then
	echo "error: output directory does not exist: $(dirname "$output_path")" >&2
	exit 1
fi

for required_command in ip iw tcpdump awk timeout; do
	if ! command -v "$required_command" >/dev/null 2>&1; then
		echo "error: required command not found: $required_command" >&2
		exit 1
	fi
done

if ! iw dev "$interface" info >/dev/null 2>&1; then
	echo "error: wireless interface does not exist: $interface" >&2
	exit 1
fi
if iw dev "$monitor_interface" info >/dev/null 2>&1; then
	echo "error: temporary monitor interface already exists: $monitor_interface" >&2
	exit 1
fi

read -r interface_flags < "/sys/class/net/$interface/flags"
was_up=$((interface_flags & 1))
capture_pid=
display_pid=
active_channel=
capture_started=0
started_utc=
restore_network_manager=0
monitor_created=0
tune_error=

packet_count()
{
	local filter=${1:-}
	if [[ ! -s $output_path ]]; then
		printf '0'
		return
	fi
	if [[ -n $filter ]]; then
		tcpdump -nn -r "$output_path" "$filter" 2>/dev/null | awk 'END { print NR + 0 }'
	else
		tcpdump -nn -r "$output_path" 2>/dev/null | awk 'END { print NR + 0 }'
	fi
}

tune_channel()
{
	local channel=$1
	local output

	# rtw89 needs the base interface up for monitor reception, but an active
	# managed base interface pins the PHY and rejects monitor channel changes.
	# Lower it only for tuning, then reactivate the receive path.
	ip link set "$interface" down
	if output=$(iw dev "$monitor_interface" set channel "$channel" HT20 2>&1); then
		ip link set "$interface" up
		ip link set "$monitor_interface" up
		return 0
	fi
	ip link set "$interface" up >/dev/null 2>&1 || true
	ip link set "$monitor_interface" up >/dev/null 2>&1 || true
	tune_error=$output
	return 1
}

cleanup()
{
	local exit_status=$?
	trap - EXIT INT TERM

	if [[ -n $display_pid ]]; then
		kill "$display_pid" >/dev/null 2>&1 || true
		wait "$display_pid" >/dev/null 2>&1 || true
	fi
	if [[ -n $capture_pid ]]; then
		kill -INT "$capture_pid" >/dev/null 2>&1 || true
		wait "$capture_pid" >/dev/null 2>&1 || true
	fi

	if ((monitor_created)); then
		ip link set "$interface" down >/dev/null 2>&1 || true
		ip link set "$monitor_interface" down >/dev/null 2>&1 || true
		iw dev "$monitor_interface" del >/dev/null 2>&1 || true
	fi
	if ((was_up)); then
		ip link set "$interface" up >/dev/null 2>&1 || true
	fi
	if ((restore_network_manager)); then
		nmcli device set "$interface" managed yes >/dev/null 2>&1 || true
	fi

	if ((capture_started)) && [[ -s $output_path ]]; then
		all_packets=$(packet_count)
		gamepad_packets=$(packet_count "wlan host $gamepad_mac")
		gamepad_tx_packets=$(packet_count "wlan addr2 $gamepad_mac")
		{
			printf 'base_interface=%s\n' "$interface"
			printf 'monitor_interface=%s\n' "$monitor_interface"
			printf 'gamepad_mac=%s\n' "$gamepad_mac"
			printf 'channel=%s\n' "$active_channel"
			printf 'started_utc=%s\n' "$started_utc"
			printf 'stopped_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
			printf 'packets_total=%s\n' "$all_packets"
			printf 'packets_with_gamepad_address=%s\n' "$gamepad_packets"
			printf 'packets_transmitted_by_gamepad=%s\n' "$gamepad_tx_packets"
			printf 'payload_encryption=expected_for_real_wiiu_runtime\n'
		} > "$metadata_path"

		echo
		echo "capture: stopped; packets=$all_packets gamepad=$gamepad_packets gamepad_tx=$gamepad_tx_packets"
		echo "capture: pcap=$output_path"
		echo "capture: metadata=$metadata_path"
		if ((gamepad_packets == 0)); then
			echo "capture: WARNING: no frames with GamePad MAC $gamepad_mac were observed" >&2
		fi
	elif [[ -e $output_path ]]; then
		echo
		echo "capture: stopped before any packets were saved: $output_path"
	else
		echo
		echo "capture: stopped before any packets were saved"
	fi

	exit "$exit_status"
}

trap cleanup EXIT
trap 'exit 130' INT TERM

if command -v nmcli >/dev/null 2>&1 &&
	[[ $(nmcli -g GENERAL.NM-MANAGED device show "$interface" 2>/dev/null || true) == yes ]]; then
	if nmcli device set "$interface" managed no >/dev/null 2>&1; then
		restore_network_manager=1
		echo "capture: temporarily released $interface from NetworkManager"
	fi
fi

ip link set "$interface" down
if ! iw dev "$interface" interface add "$monitor_interface" type monitor flags control otherbss; then
	echo "error: could not create monitor interface $monitor_interface from $interface" >&2
	exit 1
fi
monitor_created=1
# The RTL8852BE receive path remains dormant when only the monitor child is up.
# Keep the unmanaged base interface up as well, matching drcd's working AP plus
# drcdtsf monitor arrangement.
ip link set "$interface" up
ip link set "$monitor_interface" up
echo "capture: created radiotap monitor $monitor_interface from $interface"

if [[ $requested_channel == auto ]]; then
	dwell_seconds=$(awk -v milliseconds="$dwell_ms" 'BEGIN { printf "%.3fs", milliseconds / 1000 }')
	echo "capture: scanning channels=$channels dwell=${dwell_ms}ms for GamePad $gamepad_mac"
	echo "capture: turn on the real Wii U, then turn on the GamePad; press Ctrl-C to stop"
	while [[ -z $active_channel ]]; do
		for channel in $channels; do
			if [[ ! $channel =~ ^[0-9]+$ ]] || ((channel <= 0)); then
				echo "error: invalid channel in DRC_CHANNELS: $channel" >&2
				exit 1
			fi
			if ! tune_channel "$channel"; then
				printf 'capture: channel=%s unavailable; skipping (%s)\n' "$channel" "$tune_error" >&2
				continue
			fi
			printf '\rcapture: scanning channel=%-3s' "$channel"
			if timeout "$dwell_seconds" tcpdump -q -nn -i "$monitor_interface" -c 1 \
				"wlan addr2 $gamepad_mac" >/dev/null 2>&1; then
				active_channel=$channel
				break
			fi
		done
	done
	printf '\r%-72s\r' ''
	echo "capture: found GamePad transmission; locked to channel $active_channel"
	echo "capture: for association/EAPOL from the very beginning, rerun with channel $active_channel"
else
	active_channel=$requested_channel
	if ! tune_channel "$active_channel"; then
		echo "error: could not tune $monitor_interface to channel $active_channel: $tune_error" >&2
		exit 1
	fi
	echo "capture: locked to requested channel $active_channel"
	echo "capture: start the real Wii U and GamePad now"
fi

started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
{
	printf 'base_interface=%s\n' "$interface"
	printf 'monitor_interface=%s\n' "$monitor_interface"
	printf 'gamepad_mac=%s\n' "$gamepad_mac"
	printf 'channel=%s\n' "$active_channel"
	printf 'started_utc=%s\n' "$started_utc"
} > "$metadata_path"

# Capture everything on the locked channel. Filtering only by the GamePad MAC
# would discard ACK and BlockAck control frames, which are important when
# comparing successful Wii U delivery with drcd delivery.
tcpdump -q -U -B 4096 -s 0 -i "$monitor_interface" -w "$output_path" \
	>/dev/null 2>&1 &
capture_pid=$!
capture_started=1

# Show target traffic live without filtering the pcap itself.
tcpdump -l -tttt -nn -e -i "$monitor_interface" "wlan host $gamepad_mac" &
display_pid=$!

sleep 0.25
if ! kill -0 "$capture_pid" >/dev/null 2>&1 || ! kill -0 "$display_pid" >/dev/null 2>&1; then
	echo "error: tcpdump failed to start" >&2
	exit 1
fi

echo "capture: recording all channel traffic to $output_path"
echo "capture: press Ctrl-C after 20-30 seconds of normal GamePad video/audio"
wait "$capture_pid"
