# drcd Control Protocol (v0)

Transport: Unix domain stream socket (default path: `/tmp/drcd.sock`).

- Client sends one line request (`\n` terminated).
- Server processes one command and closes the connection.
- Response starts with `OK ` or `ERR `, followed by key/value lines.

Example response:

```text
OK runtime active
phase=runtime
interface=wlan0
ap_mac=f8:54:f6:7a:5c:ae
pair_code=2232
pair_symbols=♦ ♦ ♣ ♦
connected=1
backend_phase=runtime
backend_base_interface=wlan0
backend_ap_interface=wlan0
backend_virtual_ap=0
backend_last_error=-
```

## Commands

- `status`
- `pair-start <iface> <ap-mac> <code>`
- `pair-complete` (manual fallback; successful WPS transitions automatically)
- `pair-stop`
- `runtime-start` (alias of `pair-complete`)
- `runtime-stop` (alias of `pair-stop`)
- `set-connected <0|1>`
- `shutdown`

## Backend Fields

Each response includes backend diagnostics:

- `backend_phase`
- `backend_base_interface`
- `backend_ap_interface`
- `backend_virtual_ap`
- `backend_last_error`
