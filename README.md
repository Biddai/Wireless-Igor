# Project Igor Wireless

The original Project IGOR, a small focus timer, is awesome. My main issue with
it was the cable.. But it all ends here!

This is a small remix using a nice!nano v2-compatible device for battery power
and Bluetooth instead of the original board's Wi-Fi. It also adds a Pomodoro
timer.

All credit for the original design and firmware goes to
[Urban Circles' Project IGOR](https://makerworld.com/en/models/1000048-project-igor-open-source-offline-focus-partner).

## Wiring

All peripherals must use **3.3 V logic**. Do not connect a 5 V I2C pull-up or
5 V encoder signal to the nRF52840.

| Device | Signal | nice!nano GPIO | Native Arduino pin |
| --- | --- | --- | --- |
| OLED | SDA | P0.11 | D7 / `P0_11` |
| OLED | SCL | P1.04 | D8 / `P1_04` |
| OLED | VCC | Switched 3.3 V (`EXT_VCC`) | — |
| OLED | GND | GND | — |
| Encoder | CLK | P1.11 | D12 / `P1_11` |
| Encoder | SW | P1.13 | D13 / `P1_13` |
| Encoder | DT | P0.10 | D11 / `P0_10` |
| Encoder | Common | GND | — |
| Battery switch | In series | Battery `+` → switch → board `B+` / `BAT` | — |

Connect OLED VCC to regulated 3.3 V and GND to GND. Connect the encoder
common terminal to GND. The sketch enables input pull-ups, so encoder signals
must rest high and pull low when activated. The OLED reset argument is `-1`;
do not share a reset line with the encoder switch.

## Power and sleep behavior

After a minute of inactivity in the menu or a duration-selection screen, the
sketch enters `IDLE` when USB VBUS is present; the display stays on and any
encoder action returns to the menu. On battery power it turns off the OLED and
enters nRF52840 System OFF instead. Unplugging USB while idle immediately
enters System OFF. It never sleeps while a timer is running. CLK, DT, or SW
wakes the device; the first action wakes it and the next action operates the
UI.

Before System OFF, the sketch releases I2C and turns the OLED display off.
`EXT_VCC` stays enabled because the encoder wake path may depend on that rail.
Cutting OLED power without depowering the encoder requires a separate load
switch. Flow is stored in internal flash when it changes, so it survives
sleep, reset, and battery removal.

## Rotary encoder tuning

The firmware uses a quadrature decoder rather than a time-based rotary
debounce of the original firmware. `ROTARY_TRANSITIONS_PER_TICK` is set to `2`
for more responsive movement. Change it back to `4` if each physical detent
starts producing too many menu/countdown steps; no wiring changes are needed.

## Pomodoro mode

Choose `POMO` in the menu, set Focus (40 minutes by default), then set Rest
(5 minutes by default). It repeats Focus then Rest until the button stops it.
Flow credits Focus time only; Rest time is excluded.

## Arduino IDE setup

Install the community `Adafruit nRF52 Arduino` package described in the
[nice!nano v2 package guide](https://github.com/selimmeric/Adafruit_nRF52_Arduino_Nice-NanoV2).
Then select **Tools → Board → Adafruit nRF52 Boards → nice!nano v2**.

### UF2 bootloader upload

Some nice!nano v2 boards use the UF2 mass-storage bootloader (the USB drive is
named `NICENANO`). That bootloader does not accept the serial-DFU `.zip` upload
produced by some Arduino board packages. Use **Sketch > Export Compiled
Binary**, then convert the generated `.hex` file to UF2 and copy it to the
mounted `NICENANO` drive:

```powershell
py .\tools\hex_to_uf2.py (hex_file_path) (output_path)
```

Copy the .uf2 to the NICENANO drive that should pop up.
Double-press reset first if the `NICENANO` drive is not already mounted. The board restarts after the
copy finishes.

### Debugging

Open Serial Monitor at 115200 baud, then reset the board to see the I2C and
encoder diagnostics. The release default is `DEBUG_SERIAL = 0`; find that line
and temporarily change it to `1` when debugging.

## Bluetooth dashboard

This was added mainly as a proof-of-concept rather than an actual feature.

The firmware advertises as **Project IGOR Wireless** while awake and exposes a read-only
BLE Flow Status service. It sends a new status packet after UI changes, each
timer-minute change, and battery samples. Advertising stops before System OFF,
so reconnect after waking the timer with the encoder.

On battery power, the OLED and dashboard show an estimated single-cell LiPo
percentage. The firmware samples VDDH every 30 seconds and applies an
approximate LiPo discharge curve; voltage sag under load can make the estimate
temporarily read lower.

On this board, VDDH reports USB VBUS instead of the LiPo while USB is plugged
in. The menu therefore shows `USB`, and the dashboard reports battery voltage
as unavailable. It does not claim a battery percentage or charge state while
externally powered.

Use Chrome or Edge on a desktop PC. From the repository root, serve the static
dashboard on localhost:

```powershell
py -m http.server 8000 --directory web
```

Open [http://localhost:8000](http://localhost:8000), click **Connect**, and
choose `Project IGOR Wireless`. Web Bluetooth requires a secure context; localhost is
allowed, but opening `web/index.html` directly from disk is not. iPhone Safari
is not supported by this browser client.

## Hardware test checklist
Debug output is disabled by default. Find:
```C
#define DEBUG_SERIAL 0
```
and change it to `1` for serial debug printing.

More debugging tips:
1. At startup, the debug I2C scan should report the OLED address, normally
   `0x3C` or `0x3D`.
2. Verify the OLED displays the menu and the encoder turns/clicks correctly.
3. Verify each encoder input can wake the board after one idle minute.
4. Confirm active up/down timers do not sleep.
5. Complete a timer, reset/reboot, and confirm Flow persists.
6. Measure idle current after System OFF and confirm encoder wake works.
