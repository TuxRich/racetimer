# Hardware

Per gate: two Seeed XIAO ESP32-C5 receivers, one ESP32-WROOM dev board, a
5 V supply, and antennas for the receivers. Full parts list:
[parts.md](parts.md). For a Teensy 4.1 controller on Ethernet instead of
the WROOM, see [teensy.md](teensy.md).

## Wiring

Each receiver connects to the controller through one 4-core cable (up to
2 m, 28 AWG or thicker):

| Core | C5 end | Controller end |
|---|---|---|
| 1 | D6 (GPIO11, TX) | WROOM RX: RX2/GPIO16 (top), D26 (bottom) |
| 2 | D7 (GPIO12, RX) | WROOM TX: TX2/GPIO17 (top), D27 (bottom) |
| 3 | VBUS (the XIAO's 5 V pin) | +5 V from the supply, **not** the WROOM's VIN |
| 4 | GND | supply GND and WROOM GND |

Plus, at each C5: a **220–470 µF capacitor (10 V or higher) from VBUS to GND**,
soldered right at the XIAO, stripe (−) to GND.

Power, in short:

- The 5 V supply (power bank or charger) feeds each C5's VBUS through its
  cable, and the WROOM's VIN (or power the WROOM from its own USB).
- A 470 µF capacitor sits across VBUS and GND at each C5.
- Every GND is joined: supply, WROOM, both C5s. The UART needs it too.

- Pins: D6 = GPIO11 (TX) and D7 = GPIO12 (RX) match Seeed's XIAO ESP32-C5
  pin map and were confirmed on hardware (a C5 streamed ~1900 updates/s
  over this link). They are set in
  `third_party/widscan58/firmware/platformio.ini` (env `esp32c5_uart`); the
  WROOM pins are in `gate/src/main.c`.
- Labels: the XIAO's 5 V pin can be printed **VBUS**. On 30-pin WROOM
  boards, VIN is the pin at the USB end next to GND, and can look like
  "VN". The real VN (GPIO39) is at the other end, next to VP and EN; it is
  an input and must never get 5 V.
- The link runs at 2 Mbaud; with GND in the same cable, 2 m is fine. If
  the UI's status chips show `crc` errors climbing, check the joints first.
- Both chips are 3.3 V logic, so no level shifting is needed.
- The C5's USB port still works for flashing (`pio run -e esp32c5_uart -t
  upload`), but in this build the receiver's commands and data go over the
  UART only, so Widscan58's web UI can't talk to it over USB. Disconnect
  VBUS from the supply before plugging a C5 into a computer (keep GND).

## Power

Power each C5 straight from the 5 V supply, with the capacitor at the C5.

**Don't feed the C5s from the WROOM board's VIN pin.** On a 30-pin
DevKit, VIN is the USB 5 V after a diode (~4.5 V). Fed from it through
jumper wires, a C5 reset every ~200 ms with `RTC_PWR_GLITCH` (its supply dipped
when the radio started), while the same C5 on its own USB ran fine.

To check a receiver's power, type `baud 0 115200` in the web UI's receiver
console (`baud 1 …` for the bottom one), reset the C5, and read its boot ROM
lines: one boot, then silence, is good. `rst:0x19 (RTC_PWR_GLITCH)`
repeating means it is still browning out. `baud 0 0` switches back.

## Antennas

The level difference between the top and bottom bars is what tells a pass
through the gate from one over or under it.

- **Omni antennas** on both bars: the simplest. Through the middle reads
  ~0 dB, 1 m over a 2 m gate ~+10 dB. Close to a bar, inside or outside reads
  about the same, so passes near the frame are ambiguous.
- **Patch antennas pointing into the gate** (top patch pointing down, bottom
  pointing up): a quad over the gate is behind the top patch (about 15–20 dB
  weaker), so the boundary lines up with the frame. Expected to work better.
  The field test should confirm this.

Use the same antenna type and cable on both bars, then **Calibrate** in the
web UI to remove the rest of the difference between the two receivers.

## RX gain

Start at Widscan58's default, 45. A close pass must not clip: if levels
reach 254 or the UI's status shows "clipped", lower the gain by 5–10. When
both receivers clip, the top − bottom difference is lost. Then set EnterAt
and ExitAt again (Widscan58's docs/RotorHazard.md, "RX gain").

## Field test results

*To be filled in from the Milestone 4 recordings (PLAN.md).*
