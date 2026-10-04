# ESP32 chess clock

A stand-alone two-player chess clock on the **Waveshare ESP32-S3-Touch-AMOLED-1.8 (V2: CO5300
panel)**. No Mac, no Wi-Fi, no cloud: power it from USB (a power bank is fine), lay it flat between
the players, and play. The top half of the screen is drawn upside down for the player across the table.

```
tools/board.sh test      # unit-test the clock logic on the Mac (no board needed)
tools/board.sh flash     # build + flash the firmware onto the board
tools/board.sh ticker    # switch the board to a sibling project (see Revert / erase)
```

Built and tested on the Waveshare **V2** revision (CO5300 AMOLED, CST820 touch, 16 MB flash, 8 MB PSRAM).
The original revision uses a different display driver (SH8601) and touch chip and will not work unmodified.

## Setup (first time, macOS/Linux)

Needs `uv` (or any Python 3.12), a C++ compiler (`c++`, for the host tests) and the board on USB-C.

```
uv venv .venv --python 3.12 && uv pip install --python .venv/bin/python esptool==4.12.0 pyserial==3.5 mpremote==1.29.0
uv venv .pio-venv --python 3.12 && uv pip install --python .pio-venv/bin/python platformio==6.2.0
tools/board.sh flash     # the first build downloads several GB of toolchain into .platformio/
```

**Back up the board's factory firmware first**, so `tools/board.sh restore` can put it back:

```
mkdir -p backup
.venv/bin/esptool.py --port /dev/cu.usbmodemXXXX read_flash 0 0x1000000 backup/original_flash_16MB.bin
shasum -a 256 backup/original_flash_16MB.bin > backup/SHA256SUMS
```

## How to play

1. **Pick a time control** (a tile). The last one you used is highlighted and remembered across power-offs.
2. **Ready screen:** both clocks show the full time. Whoever is *not* moving first taps their own half
   to start the other's clock. (Like a physical clock: Black starts White's clock.) The first tap is
   not a move and earns no increment.
3. **Playing:** after your move, tap **your own half**. Your clock stops, your increment is added, your
   opponent's starts. Taps on the half whose clock is *not* running are ignored.
4. **Pause:** tap the pause symbol in the middle strip (only the symbol; a tap elsewhere in the strip
   counts for the nearer player). The pause panel offers **RESUME**, **RESTART** (same time, back to
   Ready) and **MENU** (back to the time picker).
5. **Flag:** when a clock reaches zero, that half turns red ("OUT OF TIME"), the other shows "WINNER",
   and taps on the halves do nothing. **AGAIN** replays the same time; **MENU** picks another.

On screen: the running player's digits are bright and a bar lights up on their outer edge (green; amber
under 30 s; red under 10 s); the waiting player's digits are dim. Each side shows its move count and its
increment (`+10s`). Under 20 s the display switches to seconds with tenths (`9.4`). Over an hour it
shows `1:30:00`. Times round up, like a real clock: it reads `00:01` until time is truly out.

Time controls (base minutes + increment seconds, Fischer increment):
bullet 1+0, 1+1, 2+1 · blitz 3+0, 3+2, 5+0 · rapid 10+0, 15+10, 25+10 · classical 30+0, 60+0, 90+30.
To change the list edit `PRESETS` in `src/clock.h` (keep it at 12 for the 3x4 grid).

Idle power policy: the screen never dims or blanks during a game, running **or paused**. On the time
picker, the ready screen and the game-over screen it dims after 60 s and blanks after 5 min; the first
touch only wakes it. (A game left paused for hours stays at full brightness: pause is a static picture,
so if you forget one, resume or hit MENU.)

Not included on purpose: sound (the board has an audio codec, but no speaker is known to be attached),
Bronstein/delay modes, different time per player, battery handling. A power cut loses the game in
progress; the chosen time control survives.

## Accuracy

Time comes from the chip's 64-bit microsecond timer (crystal-driven); each clock stores what it has left
and the running one subtracts `now - turn start`, so there's no tick counting to drift. A test plays 200
moves with odd microsecond amounts and checks the result to the exact microsecond. Drawing a frame takes
~65 ms, so touch is polled on its own task (every ~5 ms) and each tap is timestamped when the finger
first lands, then handled in order by the main loop; real taps measured `late=5ms` even while the
display redrew 10x a second. A tap that arrives after time ran out cannot save the player.

## Revert / erase (the board is yours to reuse)

| Goal | Command |
|---|---|
| Back to another project in a sibling folder | `tools/board.sh ticker` (flashes `../esp32-positions-ticker` if present; edit the path in `board.sh` for your own) |
| Blank the whole chip | `tools/board.sh erase` |
| Blank-slate MicroPython for other experiments | `tools/board.sh flash-micropython` |
| **Back to the original Waveshare factory firmware, byte for byte** | `tools/board.sh restore` |

`backup/` is gitignored and **not in this repo**: make your own copy before the first flash (see Setup).
A factory image may contain Wi-Fi credentials in its NVS partition, so never commit or share it. To remove
the toolchain too, delete `.platformio/`, `.pio/`, `.pio-venv/`, `.venv/` (or just the whole folder).

## Driving it from the Mac (for testing)

The board listens on USB serial (115200): `STATE`, `TOUCH x y`, `LEFT player secs`, `CAL`, `DUMP`.
`tools/ctl.py` wraps them so one run can tap, wait, read state and take a **screenshot** of exactly what
the frame buffer holds (`state` also reports `draw_ms`; `waitfile F` hands over to a human first):

```
tools/board.sh ctl "touch 184 170; touch 184 100; wait 2; state; shot /tmp/running.png"
tools/board.sh ctl "log 60"        # print real taps the board sees: "RAW rx,ry late=Nms" then "TAP x= y= phase="
tools/board.sh ctl "cal; log 120"  # touch calibration screen (9 targets); prints raw vs mapped
```

Opening the serial port restarts the board, so one `ctl` run is one fresh session, and only one program
can hold the port: kill a running `ctl ... log` before flashing.

## Gotchas learned the hard way

- **The touch panel reports a window of the picture, not all of it.** The CST820's raw 0..367 x 0..447
  covers screen x 33..329, y 31..397 and is clamped at the ends, so the outer 30-50 px read as the extreme
  value. `main.cpp` maps raw to screen with a fitted scale and offset (`TOUCH_SCALE_*`, `TOUCH_OFF_*`,
  from two runs of the `CAL` screen, ~9 px rms). Touch targets are therefore big; a precise button near
  the very edge can't be resolved. Re-run `CAL` if you change panels.
- **Never point esptool at a board running MicroPython** (USB PID `0x4001`): it wedges the REPL until you
  replug. `board.sh` checks the USB PID first; `board.sh status` shows the mode.
- Flash with a **watchdog reset** (`--after watchdog_reset`): a plain hard reset can fall back into
  download mode. If all else fails: hold **BOOT**, tap **RESET**, release BOOT, re-run.
- `platformio.ini` needs `board_upload.flash_size = 16MB` or the bootloader rejects the partition table.
- Touch chip: only answers after the expander reset pulse and a *write* as its first access; reads need
  a full I2C STOP (no repeated start).
- `HEX` is an Arduino macro; don't name a variable that.

## Layout

```
src/clock.h        game logic + time formatting (pure C++, unit-tested)
src/main.cpp       display (seven-segment digits drawn as bars), touch, power policy, serial test hooks
tests/test_clock.cpp  host tests (270 checks; mutation-checked: breaking increment/side/rounding fails them)
tools/board.sh     build/flash/revert wrapper;  tools/ctl.py  serial test driver + screenshots
lib/ src/fonts/    vendored Arduino_GFX 1.6.4 (CO5300 driver) and GFX fonts
.platformio/       toolchain PlatformIO downloads on first build (gitignored)
```

## Third-party code

- `lib/GFX_Library_for_Arduino/`: GFX Library for Arduino 1.6.4 (by moononournation, BSD licence, includes
  Adafruit GFX code; see its `license.txt`), as shipped in Waveshare's V2 examples. It has the CO5300 driver.
- `src/fonts/`: FreeSans bitmap fonts from the Adafruit GFX Library (derived from GNU FreeFont).
