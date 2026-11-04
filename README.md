# racetimer

**Two-receiver FPV race gates that know whether the quad flew *through* the gate.**

A normal lap timer has one receiver per gate and counts a lap at the signal
peak, so a quad flying close over or under the gate still counts. racetimer
puts two receivers on each gate, in opposite corners: one at a top corner and
one at the bottom corner diagonally across from it. It only counts a pass when
both see the quad strongly at the same moment with balanced levels:

| Top vs bottom at the pass | Verdict |
|---|---|
| within ±Δ limit (default 6 dB) | **VALID**, through the gate: a lap |
| top much stronger | **OVER** |
| bottom much stronger | **UNDER** |
| only one receiver saw it | **SINGLE_TOP / SINGLE_BOTTOM** |

> **Status: a work in development.** The timers are installed on the RaceGOW
> track's gates, with the receivers in opposite corners, and they detect laps
> correctly. Antennas, placement and thresholds are still being worked out
> (PLAN.md, Milestone 4): many passes are still seen by only one receiver,
> and OVER and UNDER haven't been flown on purpose yet. Expect the firmware
> and these docs to change.

## How it works

Per gate:

```
  top corner:    XIAO ESP32-C5 (Widscan58, UART build) ──UART 2 Mbaud──┐
                                                                       ├─► ESP32-WROOM ──Wi-Fi──► phone / laptop
  bottom corner: XIAO ESP32-C5 (Widscan58, UART build) ──UART 2 Mbaud──┘    gate controller        web UI
```

- **Receivers:** [Widscan58](https://github.com/CapnBry/widscan58) turns the
  ESP32-C5's Wi-Fi radio into a 5.8 GHz FPV receiver. Each one scans the
  race channels (default Raceband R1, R3, R6, R8, ~500 updates per second per
  channel) and streams levels to the gate controller. We use our fork,
  [geekhostuk/widscan58](https://github.com/geekhostuk/widscan58) branch
  `uart-link`, which adds a UART host link (firmware env `esp32c5_uart`),
  since the WROOM can't be a USB host. It's a git submodule at
  `third_party/widscan58`.
- **Gate controller** (`gate/`, ESP32-WROOM, ESP-IDF): sets up both
  receivers, runs Widscan58's own pass detection (`lap.c`) per receiver and
  channel, pairs the top and bottom passes, decides the verdict, and serves a
  live web UI from its own Wi-Fi access point.
- **Or a Teensy 4.1 with Ethernet** (`gate-teensy/`): the same gate logic
  and web UI, compiled from `gate/src`, on a wired network instead of the
  gate's own Wi-Fi. Runs on a Teensy 4.1 on Ethernet; not yet tried with
  the receivers: see [docs/teensy.md](docs/teensy.md).
- **Race software** (FPVTrackside, as its own new timer) can use a gate as
  its lap timer through the **timer API**, a WebSocket at `/api`: it sets
  the channels, starts and stops races, and gets every pass with its
  verdict. Spec: [docs/timer-api.md](docs/timer-api.md). On the Teensy
  controller and the simulator so far.

## Quick start

```sh
git clone --recurse-submodules https://github.com/geekhostuk/racetimer
uv tool install platformio          # or pip install platformio

# Receivers (twice: top and bottom). XIAO ESP32-C5 on USB:
cd racetimer/third_party/widscan58/firmware
pio run -e esp32c5_uart -t upload

# Gate controller. ESP32-WROOM dev board on USB:
cd ../../../gate
pio run -e esp32dev -t upload

# Or a Teensy 4.1 controller (Ethernet; docs/teensy.md):
cd ../gate-teensy
pio run -t upload
```

A printable case for the XIAO ESP32-C5 and its antenna is in
`cad/seeed-esp32-antenna-case.3mf`.

For the whole build on one page (plan, parts, wiring diagrams, where
everything goes on the gate) see [docs/build-guide.md](docs/build-guide.md).

Get the parts in [docs/parts.md](docs/parts.md), wire them as in
[docs/hardware.md](docs/hardware.md) (power the receivers from the 5 V
supply, not from the WROOM, and fit the capacitors), power up, join the
Wi-Fi network `racegate-XXXX` (password `racegate`) and open
http://192.168.4.1/.

The web UI shows each channel's top and bottom levels live, every pass with
its verdict and top − bottom in dB, laps, the settings (gain, EnterAt/ExitAt,
Δ limit, offset, pair window, minimum lap), a **Calibrate** button (fly a few
passes through the middle and it learns the two receivers' offset), a
console to the receivers, and **Record**, which saves the session as CSV for
`tools/plot_log.py`.

## Tools

| | |
|---|---|
| `uv run tools/gate_sim.py` | A pretend gate: open http://localhost:8058/ to try the web UI with no hardware. Speaks the timer API too (`ws://localhost:8058/api`). |
| `uv run tools/timer_api.py ws://racegate-XXXX.local/api` | Run a race through the timer API, as race software would, and print each detection and lap. The example to follow when writing a client. |
| `uv run tools/c5_frames.py /dev/ttyUSB0` | Check one receiver's UART link through a USB-UART adapter (2 Mbaud): updates per second per channel, levels, CRC errors. |
| `uv run tools/plot_log.py gate-….csv` | Plot a recording: levels with pass markers, and top − bottom per verdict, to set the Δ limit. |

## Tests

```sh
cd gate && pio test -e native       # pairing, verdicts, laps, calibration, timer API
cd ../gate-teensy && pio test -e native   # receiver frame parser, WebSocket
```

## Credits

This project stands on **[Widscan58](https://github.com/CapnBry/widscan58)**
by **CapnBry** (Bryan Mayland): the ESP32-C5 5.8 GHz receiver firmware, its
frame protocol, and the lap detection (`lap.c`) the gate controller runs.
racetimer would not exist without it. Go look at it, and its live web UI at
https://capnbry.github.io/widscan58/.

Widscan58 itself builds on [C5VRX](https://github.com/Twotoz/C5VRX) and
[esp-sdr](https://github.com/ESPARGOS/esp-sdr), which found the debug path
that lets an ESP32 capture raw I/Q samples from its own radio.

## License

GPL-3.0-or-later (see [LICENSE](LICENSE)), the same as Widscan58, whose code
the gate firmware compiles in.
