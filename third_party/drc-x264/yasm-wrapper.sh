#!/usr/bin/env bash
if [[ "${1:-}" == "--version" ]]; then
    echo "yasm 1.3.0"
    exit 0
fi

args=()
while (($#)); do
    if [[ "$1" == "-m" && "${2:-}" == "amd64" ]]; then
        shift 2
        continue
    fi
    args+=("$1")
    shift
done
exec nasm "${args[@]}"
