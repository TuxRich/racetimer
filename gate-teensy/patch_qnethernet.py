"""Hardens QNEthernet 0.37.0's Teensy 4.1 driver before it is compiled. A
PlatformIO pre-build script: it edits the library's copy in .pio/libdeps,
once (each edit carries a "racetimer:" note, and a file that has them is
left alone).

Without these the gate's loop could hang for seconds at a time, and USB
with it: the driver waits for a free transmit descriptor in a tight loop on
DTCM, with no limit; it rings the doorbells (TDAR, RDAR) with no barrier
after writing the descriptor; and its ISR could run again after clearing
its flag. Whether that happened depended on where the code landed in
memory, so an unrelated change to the firmware could bring it on
(docs/teensy.md). The edits: a barrier before each doorbell and at the end
of the ISR, a transmit wait that rings the doorbell again and pauses
between looks, and both doorbells rung from the driver's poll().
GPL-3.0-or-later; the library itself is AGPL-3.0."""
import json
import pathlib
import sys

Import("env")  # noqa: F821 (SCons)

VERSION = "0.37.0"
DSB = '__asm__ volatile("dsb" ::: "memory");'
EDITS = [
    ("""    ENET::EIR::RXF = 1;
    std::atomic_flag_clear(&s_rxNotAvail);
  }
}""", f"""    ENET::EIR::RXF = 1;
    std::atomic_flag_clear(&s_rxNotAvail);
  }}
  // racetimer: let the clear reach the ENET before returning, or the ISR
  // runs again at once (as NXP's SDK_ISR_EXIT_BARRIER)
  {DSB}
}}"""),
    ("""  pBD->status = (pBD->status & rx_bd_status::kWrap) | rx_bd_status::kEmpty;

  ENET::RDAR::RDAR = 1;""", f"""  pBD->status = (pBD->status & rx_bd_status::kWrap) | rx_bd_status::kEmpty;

  {DSB}  // racetimer: the descriptor first, then the doorbell
  ENET::RDAR::RDAR = 1;"""),
    ("""  while ((pBD->control & tx_bd_control::kReady) != 0) {
    // Wait until a free buffer is available
    // TODO: Limit count?
  }""", """  while ((pBD->control & tx_bd_control::kReady) != 0) {
    // Wait until a free buffer is available. racetimer: ring the doorbell
    // in case the MAC missed it, and pause between looks so that this loop
    // doesn't take the DTCM bandwidth the ENET's DMA needs to free the buffer
    ENET::TDAR::TDAR = 1;
    delayNanoseconds(200);
  }"""),
    ("""                 tx_bd_control::kReady;

  ENET::TDAR::TDAR = 1;""", f"""                 tx_bd_control::kReady;

  {DSB}  // racetimer: the descriptor first, then the doorbell
  ENET::TDAR::TDAR = 1;"""),
    ("""void poll(struct netif* const netif) {
  s_checkLinkStatusState = check_link_status(netif, s_checkLinkStatusState);
}""", """void poll(struct netif* const netif) {
  s_checkLinkStatusState = check_link_status(netif, s_checkLinkStatusState);
  // racetimer: ring both doorbells now and then, so that one the MAC
  // missed can't leave a ring stalled
  if (s_initState == InitStates::kInitialized) {
    ENET::RDAR::RDAR = 1;
    ENET::TDAR::TDAR = 1;
  }
}"""),
]


def fail(why):
    sys.exit(f"patch_qnethernet.py: {why}")


lib = pathlib.Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"] / "QNEthernet"  # noqa: F821
driver = lib / "src" / "qnethernet" / "drivers" / "driver_teensy41.cpp"
if not driver.exists():
    fail(f"{driver} is missing: is QNEthernet installed?")
version = json.loads((lib / "library.json").read_text()).get("version")
if version != VERSION:
    fail(f"QNEthernet is {version}; these edits are for {VERSION} (platformio.ini's lib_deps)")
text = open(driver, newline="").read()
if "racetimer:" not in text:
    for old, new in EDITS:
        if text.count(old) != 1:
            fail(f"{driver.name} doesn't read as expected, near:\n{old}")
        text = text.replace(old, new)
    open(driver, "w", newline="").write(text)
    print(f"patch_qnethernet.py: hardened QNEthernet {VERSION}'s Teensy 4.1 driver")
