# Plan: racetimer — two-receiver "through the gate" validation

## Context

Normal FPV lap timers use one receiver per gate and count a lap on the RSSI peak,
so a quad flying close *over* or *under* the gate still counts. Idea: put two
Widscan58 receivers (ESP32-C5, https://github.com/CapnBry/widscan58) on each gate,
one on the top bar and one on the bottom bar, and only accept a pass when both see
the quad strongly at the same moment with a balanced top/bottom level. A cheap
ESP32-WROOM per gate is the "brain": it reads both C5s over UART, decides pass /
over / under, and serves a live web UI for testing. Channels: Raceband R1, R3, R6,
R8.

Widscan58 already does all the radio work (`METRICS RSSI` mode streams compact
`FRAME_LEVELS` per hop; `firmware/src/scan/lap.c` does RotorHazard-style pass
detection) but only talks over USB Serial/JTAG (`firmware/src/proto/link.c`),
which a WROOM can't host. So the work is: a small transport change in a fork, plus
a new project for the gate controller.

## Repos and licensing

1. **Fork** `CapnBry/widscan58` → `geekhostuk/widscan58`, branch `uart-link`.
   Minimal, upstream-friendly diff; offer it back as a PR to CapnBry.
2. **New repo** `geekhostuk/racetimer` (this directory, currently empty).
   - The fork is a git submodule at `third_party/widscan58` (pinned commit).
   - **GPL-3.0-or-later** for the whole repo: the gate firmware compiles
     widscan58's `lap.c`/`frames.h`, which are GPL.
   - Credit: README "Credits" section naming Widscan58 and CapnBry with a link,
     plus C5VRX / esp-sdr that Widscan58 credits; GPL headers kept on any
     copied/derived file ("Derived from Widscan58 …").

## Repo layout (racetimer)

```
racetimer/
├── README.md            # what it is, wiring, quick start, Credits
├── LICENSE              # GPL-3.0
├── PLAN.md              # this design, kept up to date
├── third_party/widscan58/   # submodule → fork, branch uart-link
├── gate/                # WROOM firmware, PlatformIO + ESP-IDF
│   ├── platformio.ini   # envs: esp32dev (WROOM), native (host tests)
│   ├── src/
│   │   ├── main.c
│   │   ├── c5_link.c/.h     # UART driver per C5, frame parser (CRC32), command sender
│   │   ├── gate_fusion.c/.h # pairing + verdict (pure C, host-testable)
│   │   ├── web.c/.h         # Wi-Fi AP/STA, esp_http_server + WebSocket
│   │   ├── settings.c/.h    # NVS-backed settings
│   │   └── CMakeLists.txt   # also builds ../../third_party/widscan58/firmware/src/scan/lap.c
│   ├── web/index.html       # single file, plain JS + canvas, embedded gzipped
│   └── test/test_fusion/    # Unity tests on the native env
├── tools/
│   ├── c5_frames.py     # read a C5 over a USB-UART adapter, decode FRAME_LEVELS
│   └── plot_log.py      # plot recorded CSV: top, bottom, delta per pass
└── docs/hardware.md     # wiring, power, antenna mounting
```

## Milestone 1 — C5 fork: UART link (`geekhostuk/widscan58@uart-link`)

- `firmware/src/proto/link.c`: add a build option `LINK_UART` (new PlatformIO env
  `esp32c5_uart`; Kconfig/`-D` for TX pin, RX pin, baud, default 2 000 000).
  Swap `usb_serial_jtag_driver_install / _write_bytes / _read_bytes` for
  `uart_driver_install / uart_write_bytes / uart_read_bytes`. The rest of
  `link.c` (ring buffer queue, `link_task`, drop counter) stays as is. Default
  build stays USB, so upstream behaviour is unchanged.
- Protocol is unchanged: ASCII commands in, ASCII replies + binary frames out
  (`frames.h`: `0xA5 'R' type ver len payload crc32`). The board boots waiting in
  RotorHazard mode; the WROOM sends `PROTOCOL MONITOR`, then the channel list,
  `METRICS RSSI`, RX gain, and turns spectrum/telemetry streaming off. (Exact
  command syntax to be confirmed from `proto/commands.c` while implementing.)
- **Throughput check:** a `FRAME_LEVELS` hop is ~46 bytes; with R1/R3/R6/R8
  (4 LOs, ~2 ms per sweep) that's ~2000 hops/s ≈ 90 KB/s. That's why the baud
  is 2 Mbaud, not 921600. If `link_drops()` climbs, add a `LEVELS EVERY n`
  decimation command (small, also useful upstream).
- Verify the XIAO ESP32-C5 pin numbers for the UART TX/RX (D6/D7) on hardware.

## Milestone 2 — gate controller reads both C5s + live web UI

- `c5_link.c`: one instance per C5 (top on UART2, bottom on UART1; avoid
  GPIO 6–11). Byte-wise frame parser with CRC32 check (`esp_rom_crc32_le`),
  decodes `frame_levels_t` / `frame_level_t` from the submodule's `frames.h`
  (no copied structs). Sends setup commands at boot and when settings change;
  re-sends if a board stops streaming for >1 s (C5 reboot/brown-out).
- Timestamps: put both boards on the WROOM's clock. Keep a running
  `offset = min(rx_time_us − t_ms·1000)` per board and convert each hop's
  `t_ms`. Good to ~1 ms, plenty for pairing.
- `web.c`: Wi-Fi AP by default (`racegate-XXXX`, works at a field with no
  internet), optional STA from settings. HTTP serves `index.html`; one
  WebSocket pushes level batches at ~30 Hz (max per channel per board over each
  window) and pass events as they happen. No CDN: plain JS + canvas.
- UI page: per channel (R1/R3/R6/R8) top and bottom traces plus the delta trace;
  pass list with verdict; settings panel (gain, enter/exit, delta limit, pair
  window, channels); "Record" button that saves the session as CSV in the
  browser; board status (frames/s, drops, saturation flag).

## Milestone 3 — pass detection + fusion

- Per board per channel: one widscan58 `lap.c` instance (median or EMA filter,
  enter_at/exit_at), fed from the decoded levels. Reuse it instead of writing a
  new detector.
- `gate_fusion.c` (pure C, no ESP-IDF calls):
  - Pair a top pass and a bottom pass on the same channel within ±75 ms
    (configurable).
  - Δ = (top peak − bottom peak) − calibrated offset, in dB (codes / 4).
  - **VALID** if both peaks ≥ enter_at and |Δ| ≤ delta_max (start ~6 dB);
    **OVER** if Δ > delta_max; **UNDER** if Δ < −delta_max; **SINGLE** if only one
    board saw it (rejected but shown).
  - Lap time = mean of the two pass times; minimum lap time rejection.
- Calibration: "Calibrate" button averages Δ over the next N centre passes into a
  per-gate offset, stored in NVS.

## Milestone 4 — field test (decides the antenna and thresholds)

Record in this order: omni antennas first, then inward-facing patches (top points
down, bottom points up). Fly centre, near the top bar inside, just over the top
bar, near the bottom bar inside, under the gate. Use `tools/plot_log.py` to see
whether VALID and OVER/UNDER separate, then set `delta_max` and the antenna
choice. Write the results into `docs/hardware.md`.

**Later, out of scope for now:** several gates sending to one server, clock sync
between gates, RotorHazard plugin.

## Verification

- **Fork:** build `esp32c5_uart`; connect a USB-UART adapter at 2 Mbaud;
  `tools/c5_frames.py` shows ~500 updates/s per channel with valid CRCs, and
  `STATUS` reports zero link drops over 10 min. The default USB build still works
  with the Widscan58 web UI.
- **Fusion:** `pio test -e native` with synthetic two-trace cases: centre pass →
  VALID, top-heavy → OVER, bottom-heavy → UNDER, one-sided → SINGLE, peaks
  100 ms apart → not paired, plus calibration offset applied.
- **Bench:** two C5s + WROOM wired on a desk, a low-power VTX on R1 moved by hand
  past each antenna. The UI shows both traces, Δ swings the right way, and the
  verdicts match.
- **Field:** Milestone 4 recordings; every deliberate over/under pass rejected and
  every centre pass accepted before calling it done.

## Status (2026-10-23)

- Milestone 1: done, not yet run on hardware. Fork branch `uart-link`
  (`LINK_UART`, env `esp32c5_uart`) builds; the default `esp32c5` build and
  upstream's host tests still pass.
- Milestones 2 and 3: written and build for the WROOM (`gate/`). Fusion host
  tests pass (`pio test -e native`, 14 cases). The web UI was checked against
  `tools/gate_sim.py` in headless Chromium (light, dark, 390 px phone width).
- Next: bench test with real boards (Verification), then Milestone 4.
- 2026-11-02: `gate-teensy/`, the controller on a Teensy 4.1 with Ethernet
  (docs/teensy.md): `gate/src`'s logic compiled as it is, serial ports,
  QNEthernet, its own HTTP/WebSocket server. Builds for the Teensy; host
  tests pass (16); ran on a PC against two simulated C5s and a browser. Not
  yet on hardware. Found there: a finished calibration's offset was never
  saved or shown, and the next settings change undid it (`gate.c`, fixed for
  both controllers).
- 2026-11-02: the timer API (docs/timer-api.md), for race software to use a
  gate as its lap timer, first as a new FPVTrackside timer: a WebSocket at
  `/api` (`gate/src/gate_api.c`; races, detections with verdicts and replay,
  clock sync, levels, status). On the Teensy controller and in
  `tools/gate_sim.py`; `tools/timer_api.py` is a client to follow. Host tests
  pass (11); ran against the Teensy firmware on a PC and the simulator. The
  WROOM's web server doesn't serve it yet; the FPVTrackside timer is to be
  written.
- 2026-11-04: `gate-teensy/` on a Teensy 4.1 with Ethernet, receivers not
  yet connected: DHCP and `racegate-XXXX.local`, the page and its
  WebSocket, the USB console and the timer API (a race, levels, replay) all
  work. Found there: the stall check's timestamp could be a millisecond in
  the future, so a client with a full send buffer was dropped at once and
  most page loads were cut off (`web.cpp`, fixed; the stall limit is now
  5 s); and QNEthernet 0.37.0's Teensy driver could hang the loop for
  seconds, and USB with it, depending on where the code landed in memory
  (patched at build time, `patch_qnethernet.py`; held across five layouts,
  including one that hung on every boot without it).
