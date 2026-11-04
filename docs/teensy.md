# Teensy 4.1 controller (Ethernet)

The gate controller can be a Teensy 4.1 with Ethernet instead of the
ESP32-WROOM. The receivers, their cables and the web page stay the same,
and so does the gate logic: `gate/src`'s pairing, verdicts, laps,
calibration and settings, and Widscan58's pass detection, are compiled as
they are. What changes is the network: wired Ethernet instead of the gate's
own Wi-Fi access point. The firmware is in `gate-teensy/`.

> **Status:** runs on a Teensy 4.1 on Ethernet: the page, the USB console
> and the timer API work (2026-11-04). The receivers haven't been connected
> to it yet; on a PC, the firmware has run against two simulated receivers
> (real frames at ~1900 updates/s).

|  | ESP32-WROOM (`gate/`) | Teensy 4.1 (`gate-teensy/`) |
|---|---|---|
| Network | Its own Wi-Fi network, `racegate-XXXX`, at http://192.168.4.1/ | Ethernet: an address from DHCP, or a fixed one |
| Finding the gate | Join its Wi-Fi | http://racegate-XXXX.local/, the router's client list, or the USB console |
| Top receiver | UART2: RX2/GPIO16, TX2/GPIO17 | Serial1: RX1 pin 0, TX1 pin 1 |
| Bottom receiver | UART1: GPIO26, GPIO27 | Serial2: RX2 pin 7, TX2 pin 8 |
| Settings kept in | NVS | EEPROM (flash) |
| Console | The page's | The page's, and USB serial with the same commands |

## Before you buy

- **No Wi-Fi.** Phones and laptops reach the gate through a router: one
  with Wi-Fi and a DHCP server (a USB-powered travel router is enough), a
  switch, and an Ethernet cable to each gate (up to 100 m). One router
  serves every gate and puts them all on one network, which the later
  "several gates to one server" work needs anyway.
- **Keep the router's Wi-Fi off Raceband.** Use 2.4 GHz, or 5 GHz channels
  36–64 (5.15–5.35 GHz). Channels 100 and up sit on or next to R1–R8
  (5658–5917 MHz), and the receivers will see them.
- **Get the Teensy 4.1 with the Ethernet chip.** PJRC also sells one
  without it ("NE"). The board has the chip but no socket: add PJRC's
  Ethernet kit (the RJ45 jack on a small board, its cable, and a header to
  solder on the Teensy).
- **PSRAM is not needed.** The Teensy's own 1 MB of RAM is plenty (this
  firmware leaves about 650 KB free). Chips fitted on the PSRAM pads do no
  harm.
- **3.3 V logic, not 5 V tolerant.** The C5s are 3.3 V too, so their UARTs
  connect directly, as on the WROOM. Never put 5 V on a Teensy pin.
- **Power:** VIN takes 3.6–5.5 V. The Teensy draws about 100 mA, a little
  more with Ethernet up: less than the WROOM's Wi-Fi peaks. VIN and the USB
  5 V are joined on the board. To power it from the 5 V rail and plug in USB
  at the same time, cut the trace between the VIN and VUSB pads on the
  underside first (then it runs, and flashes, only with the rail on), or
  unplug the rail from VIN before plugging in USB.
- **Power over Ethernet** (optional): the Teensy can't take PoE itself, but
  a PoE switch and an 802.3af splitter with a 5 V, 2 A output at the gate
  can feed the 5 V rail: one cable per gate for power and data.
- **The page is open to anyone on the network.** The WROOM's access point
  had a password; on Ethernet, keep the gates on your own network.
- **Licence:** the Ethernet library, QNEthernet, is AGPL-3.0, which works
  with racetimer's GPL-3.0. Because the gate serves a web page, the page
  links to its source code, as the AGPL asks.

## Parts (per gate, in place of the WROOM)

| Qty | Part | Notes |
|---:|---|---|
| 1 | **Teensy 4.1**, with Ethernet | Not the "NE" version. |
| 1 | **PJRC Ethernet kit for Teensy 4.1** | Or another jack with magnetics that fits the Teensy's Ethernet header. |
| 1 | Ethernet cable | To the switch or router. |
| — | Router with Wi-Fi and DHCP, and a switch | One for the track, not per gate. |
| — | PoE switch and a 5 V PoE splitter (optional) | Instead of a 5 V supply at each gate. |

The receivers, capacitors, cables and 5 V supply are as in
[parts.md](parts.md).

## Wiring

The receiver cables are as for the WROOM ([hardware.md](hardware.md)); only
the controller end changes.

| Core | C5 end | Top receiver → Teensy | Bottom receiver → Teensy |
|---|---|---|---|
| 1 | D6 (GPIO11, TX) | RX1, pin 0 | RX2, pin 7 |
| 2 | D7 (GPIO12, RX) | TX1, pin 1 | TX2, pin 8 |
| 3 | VBUS (5 V) | +5 V rail (supply) | +5 V rail (supply) |
| 4 | GND | GND rail | GND rail |

```
                          ┌─────── USB ───────┐
       GND rail ───────── │ GND           VIN │ ──── +5 V rail
   top C5 D6 ───────────► │ 0  RX1        GND │
   top C5 D7 ◄─────────── │ 1  TX1       3.3V │
                          │ 2              23 │
                          │ 3              22 │
                          │ 4              21 │
                          │ 5              20 │
                          │ 6              19 │
bottom C5 D6 ───────────► │ 7  RX2         18 │
bottom C5 D7 ◄─────────── │ 8  TX2         17 │
                          │ 9 … (unused)      │
                          │    Ethernet header│ ──── cable to the RJ45 jack
                          └───────────────────┘
```

Teensy 4.1 seen from the top, USB at the top; only the pins used are
labelled. The receivers still take their 5 V from the supply, not from the
Teensy, with the capacitor at each C5.

## Network

- **DHCP** by default. The gate's name is `racegate-XXXX` (the last four
  hex digits of its MAC address, as the WROOM's access point): the router
  lists it under that name, and http://racegate-XXXX.local/ works from
  computers and iPhones. Some Android phones don't resolve `.local` names:
  use the address there.
- **A fixed address:** Settings → IP address, e.g. `192.168.1.50/24`
  (`/24` if left out), and a gateway only if the phones are on another
  subnet. It takes effect at once, so the page loses its connection: open
  the gate at its new address and press Save there. Until it is saved, a
  power cycle brings back the old address. Blank goes back to DHCP.
- **A laptop straight on the Teensy**, no router: give both fixed
  addresses on one subnet. Over USB: `set ip 192.168.4.1/24`, then `save`;
  on the laptop, 192.168.4.2/24.

## USB serial console

Open the Teensy's USB serial port with any terminal (`pio device monitor`
in `gate-teensy/`; the speed doesn't matter). It prints the gate's name and
address, then takes the page's commands, one per line: `set ip …`,
`set gw …`, `set gain 40`, `save`, `defaults`, `cal 5`, `clear`,
`c5 0 STATUS?`, `baud 0 115200`, and `net` (show the address again).
Receiver replies print as text, passes as JSON.

## Flashing and tests

```bash
cd gate-teensy
pio run -t upload
```

This uses the Teensy Loader (a small window opens), which does the erase a
Teensy 4.1 needs; PJRC's command-line loader gave up partway through here.
The first upload may need a press of the Teensy's button, and so does one
after the board has stopped answering on USB. The host tests (WebSocket
handshake and framing, the receiver frame parser):

```bash
cd gate-teensy
pio test -e native
```

To work on the page without hardware, `uv run tools/gate_sim.py --eth`
pretends to be a Teensy controller.

## How the firmware is put together

- `src/shared_*.c` compile `gate/src`'s `gate.c`, `gate_fusion.c` and
  `settings.c`, and Widscan58's `lap.c`, as they are. `compat/` stands in
  for the ESP-IDF calls they make: the clock, FreeRTOS locks (nothing to
  lock: everything runs from `loop()`), and NVS, here on the EEPROM.
- `c5_link.cpp`: the receivers on Serial1 and Serial2 at 2 Mbaud, with
  16 KB of receive buffer each; `c5_frame.c` splits their streams into
  reply lines and CRC-checked frames.
- `net.cpp`: Ethernet (QNEthernet), DHCP or a fixed address, the name and
  mDNS. `web.cpp` and `ws.c`: a small HTTP and WebSocket server for the
  page, and the commands, also from USB serial; and the timer API for race
  software (FPVTrackside) at `/api`, `gate/src/gate_api.c`
  ([timer-api.md](timer-api.md)).
- The page is `gate/web/index.html`, gzipped into the firmware at build
  time (`embed_page.py`).
- `patch_qnethernet.py` edits QNEthernet 0.37.0's Teensy driver before it
  is compiled. Unpatched, the driver could hang the loop for seconds at a
  time, and USB with it (pings mostly lost, the page stuck), on some builds
  and not others: it waits for a free transmit descriptor in a tight loop
  with no limit, and rings the MAC's doorbells without a memory barrier.
  The patch adds the barriers, makes that wait ring the doorbell again and
  pause between looks, and rings both doorbells from the driver's poll.
  Moving to another QNEthernet version means checking these again (the
  script stops the build if the driver isn't the one it was written for).
- Saving settings writes flash with interrupts off, so each receiver loses
  a frame or two (its `crc` count goes up by a few). Harmless.
