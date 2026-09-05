# drcctl

CLI client for `drcd`.

Current commands:

- `drcctl status`
- `drcctl pair-start <iface> <ap-mac> <code>`
- `drcctl pair-complete`
- `drcctl pair-stop`
- `drcctl runtime-start`
- `drcctl runtime-stop`
- `drcctl set-connected <0|1>`
- `drcctl shutdown`

Examples:

```bash
drcd --socket /tmp/drcd.sock
drcctl --socket /tmp/drcd.sock pair-start wlan0 f8:54:f6:7a:5c:ae 2232
drcctl --socket /tmp/drcd.sock pair-complete
drcctl --socket /tmp/drcd.sock status
```
