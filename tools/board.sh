#!/usr/bin/env bash
# Board operations for the chess clock. Usage: tools/board.sh <command>
#
#   test              run the clock-logic unit tests on the Mac (no board needed)
#   build             compile the firmware
#   flash             build, then write the chess clock firmware to the board
#   ctl "steps"       drive the running clock from the Mac: taps, state, screenshots (see tools/ctl.py)
#   ticker            switch the board back to the positions ticker project (../esp32-positions-ticker)
#   reset             reboot the board
#   flash-micropython erase the board and flash a blank-slate MicroPython (for other experiments)
#   erase             erase the whole flash (board is blank until you flash something)
#   restore           write the ORIGINAL firmware backup back, byte for byte (full revert)
#   status            show whether the board is visible and which USB mode it is in
set -euo pipefail
cd "$(dirname "$0")/.."

PY=.venv/bin/python
ESPTOOL=.venv/bin/esptool.py
MPR=.venv/bin/mpremote
PIO=.pio-venv/bin/pio
ENV=amoled18v2
export PLATFORMIO_CORE_DIR="$PWD/.platformio"
BUILD=.pio/build/$ENV
BOOT_APP0=$PLATFORMIO_CORE_DIR/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin
MP_FW=firmware/ESP32_GENERIC_S3-v1.29.0.bin
MP_URL=https://micropython.org/resources/firmware/ESP32_GENERIC_S3-20260824-v1.29.0.bin
BACKUP=$(ls backup/original_flash_*.bin 2>/dev/null | head -1 || true)

port() {
  $PY -c "
from serial.tools import list_ports
c = sorted(p.device for p in list_ports.comports() if p.vid == 0x303A)
print(next((d for d in c if d.startswith('/dev/cu.')), c[0] if c else ''))"
}

# Port of a board in a state esptool may safely touch: USB PID 0x1001 (ROM bootloader, or firmware
# using the USB-Serial/JTAG console like this ticker). MicroPython's own USB shows as PID 0x4001;
# pointing esptool at that wedges the REPL, so never do it.
rom_port() {
  $PY -c "
from serial.tools import list_ports
c = sorted(p.device for p in list_ports.comports() if p.vid == 0x303A and p.pid == 0x1001)
print(next((d for d in c if d.startswith('/dev/cu.')), c[0] if c else ''))"
}

# Get an esptool-safe port: already there -> done; MicroPython running -> ask it to reboot into the
# ROM bootloader; else tell the human to use the buttons.
to_rom() {
  P=$(rom_port)
  if [ -z "$P" ]; then
    P=$(port)
    if [ -n "$P" ]; then
      $MPR connect "$P" exec "import machine; machine.bootloader()" >/dev/null 2>&1 || true
      for _ in 1 2 3 4 5 6 7 8; do
        sleep 1
        P=$(rom_port)
        [ -n "$P" ] && break
      done
    fi
  fi
  if [ -z "$P" ]; then
    cat >&2 <<'MSG'
Could not reach the board automatically. Do it by hand:
  hold BOOT, tap RESET (or unplug, hold BOOT while replugging), release BOOT, then re-run.
MSG
    exit 1
  fi
}

cmd=${1:-}
shift || true
case "$cmd" in
  test)
    mkdir -p .pio
    c++ -std=c++17 -Wall -Wextra -include string -o .pio/test_clock tests/test_clock.cpp
    .pio/test_clock
    ;;
  ctl)   exec $PY tools/ctl.py "$@" ;;
  ticker)
    T=../esp32-positions-ticker
    [ -x "$T/tools/board.sh" ] || { echo "no $T project found" >&2; exit 1; }
    exec "$T/tools/board.sh" flash
    ;;
  build) $PIO run -e $ENV ;;
  flash)
    $PIO run -e $ENV
    to_rom
    # After a ROM-download-mode reset, a plain hard reset can fall straight back into download
    # mode; a watchdog reset boots the new firmware reliably.
    $ESPTOOL --port "$P" --chip esp32s3 --after watchdog_reset write_flash -z --flash_size 16MB \
      0x0 $BUILD/bootloader.bin 0x8000 $BUILD/partitions.bin 0xe000 "$BOOT_APP0" 0x10000 $BUILD/firmware.bin
    ;;
  reset)
    to_rom
    $ESPTOOL --port "$P" --after watchdog_reset chip_id >/dev/null
    echo "reset"
    ;;
  flash-micropython)
    [ -f "$MP_FW" ] || curl -fsSL -o "$MP_FW" "$MP_URL"
    to_rom
    $ESPTOOL --port "$P" --after no_reset erase_flash
    $ESPTOOL --port "$P" --chip esp32s3 --after watchdog_reset write_flash -z 0x0 "$MP_FW"
    ;;
  erase)
    to_rom
    $ESPTOOL --port "$P" --after watchdog_reset erase_flash
    ;;
  restore)
    [ -n "$BACKUP" ] || { echo "no backup in backup/" >&2; exit 1; }
    shasum -a 256 -c backup/SHA256SUMS
    to_rom
    $ESPTOOL --port "$P" --after no_reset write_flash 0x0 "$BACKUP"
    $ESPTOOL --port "$P" --after watchdog_reset verify_flash 0x0 "$BACKUP"
    ;;
  status)
    $PY -c "
from serial.tools import list_ports
b = [(p.device, hex(p.pid)) for p in list_ports.comports() if p.vid == 0x303A]
print('board:', b or 'not found')
for _, pid in b:
    print({'0x1001': 'USB-Serial/JTAG (ROM bootloader or Arduino firmware): esptool-safe',
           '0x4001': 'MicroPython USB-CDC: do NOT point esptool at it'}.get(pid, 'unknown mode'))"
    ;;
  *) sed -n '2,15p' "$0"; exit 1 ;;
esac
