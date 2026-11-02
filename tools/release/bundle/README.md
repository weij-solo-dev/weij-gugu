# gugu firmware bundle

Flash an M5Stack StopWatch from any Mac or Linux machine. Nothing but Python 3 is required: the first
run of `flash.sh` installs esptool and pyserial into a private virtualenv (`~/.venvs/gugu-flash`), which
needs internet once. Plug the StopWatch in with a data cable; it shows up as `/dev/cu.usbmodem*` with no
driver.

```bash
./flash.sh                 # bootloader + partition table + app + content image; Wi-Fi, keys and recorded calls are kept
./flash.sh --app-only      # only the application (a few seconds)
./flash.sh --erase         # factory reset first: Wi-Fi, keys and all data are wiped
./flash.sh --port /dev/cu.usbmodemXXXX
```

After flashing (first time only; the values live in the device's NVS):

```bash
./console.sh "wifi <ssid> <password>" "key minimax <api key>" "stat"
./console.sh "replay /content/test/zh_01.wav 3"     # one call, three turns, latencies on the `turn:` lines
./console.sh help
./monitor.sh 30                                      # 30 s of logs from a fresh boot
```

The version is in `flash_manifest.sh`; `bin/` holds the images and their offsets come from the build.

Troubleshooting

- *no StopWatch found*: use a data cable, unplug and replug, check `ls /dev/cu.usbmodem*`, pass `--port`.
- *Permission denied* on Linux: add yourself to the `dialout` group and log in again.
- Device unresponsive: a short press of the power button resets it; two quick presses power it off.
- Secrets: the Wi-Fi password and API key are typed on this machine's command line, so they end up in
  the shell history. Clear it or prefix the command with a space if the shell is configured to skip those.
