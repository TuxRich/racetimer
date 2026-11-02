#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["websockets>=13"]
# ///
"""A pretend gate controller for working on the web UI without hardware.

    uv run tools/gate_sim.py          # then open http://localhost:8058/
    uv run tools/gate_sim.py --eth    # as the Teensy 4.1 controller (Ethernet)

Serves gate/web/index.html and a /ws WebSocket that speaks the gate's
messages (src/web.h): levels at ~30 per second, and on each channel a pass
every 6-10 s that goes through the middle, near a bar, over, under, or past
one bar only. Verdicts follow gate_fusion.c's rules. Commands from the page
change the simulated settings. GPL-3.0-or-later.
"""
import argparse
import asyncio
import json
import math
import pathlib
import random
import time

from websockets.asyncio.server import serve
from websockets.datastructures import Headers
from websockets.http11 import Response

PAGE = pathlib.Path(__file__).resolve().parent.parent / "gate" / "web" / "index.html"
cfg = {"t": "cfg", "names": ["R1", "R3", "R6", "R8"], "channels": "R1=5658 R3=5732 R6=5843 R8=5917",
       "gain": 45, "enter": 125, "exit": 105, "delta": 24, "offset": 0, "pair": 75, "wait": 1000,
       "minlap": 5000, "ssid": ""}
# --eth: the Teensy controller's network message (gate-teensy/src/net.cpp)
net = {"t": "net", "if": "eth", "st": "up", "ip": "127.0.0.1", "name": "racegate-51A0", "dhcp": 1, "mbps": 100,
       "sip": "", "sgw": ""}
eth = False
BOARD_BIAS = 6  # the top board reads 1.5 dB hotter: what calibration removes
clients = set()
cal = {"left": 0, "sum": 0, "n": 0}
last_lap = {}


def now_ms():
    return int(time.monotonic() * 1000)


# Kinds of pass: (name, top peak above the noise, bottom peak above the noise)
KINDS = [("centre", 120, 120), ("centre", 115, 112), ("near top", 135, 105), ("near bottom", 104, 132),
         ("over", 125, 70), ("under", 68, 122), ("past top only", 100, 20)]


class Channel:
    def __init__(self, i):
        self.i = i
        self.next_pass = now_ms() + random.randint(1500, 6000)
        self.active = None  # (t0, top, bottom)

    def level(self, board, t):
        noise = 28 + random.gauss(0, 2)
        if not self.active:
            return noise
        t0, top, bot = self.active
        peak = top + BOARD_BIAS if board == 0 else bot
        x = (t - t0) / 180.0  # ~0.4 s wide
        return min(254, noise + peak * math.exp(-x * x))


async def broadcast(msg):
    text = json.dumps(msg)
    for ws in list(clients):
        try:
            await ws.send(text)
        except Exception:
            clients.discard(ws)


async def pass_event(ch, t, top, bot):
    """Each board's pass, then the fused verdict, as the gate sends them."""
    seen = [top + BOARD_BIAS >= cfg["enter"] - 28, bot >= cfg["enter"] - 28]
    pk = [int(min(254, 28 + top + BOARD_BIAS)), int(min(254, 28 + bot))]
    for b in (0, 1):
        if seen[b]:
            await broadcast({"t": "seat", "ms": t, "b": b, "ch": ch, "pk": pk[b]})
    if not any(seen):
        return
    ev = {"t": "pass", "ms": t, "ch": ch, "n": 0, "cal": 0, "top": pk[0] if seen[0] else 0,
          "bot": pk[1] if seen[1] else 0, "d": 0, "lap": 0}
    if all(seen):
        raw = pk[0] - pk[1]
        d = raw - cfg["offset"]
        ev["d"] = d
        ev["v"] = "OVER" if d > cfg["delta"] else "UNDER" if d < -cfg["delta"] else "VALID"
        if cal["left"]:
            ev["cal"] = 1
            cal["sum"] += raw
            cal["n"] += 1
            cal["left"] -= 1
            if not cal["left"]:
                cfg["offset"] = round(cal["sum"] / cal["n"])
                await broadcast(cfg)
    else:
        ev["v"] = "SINGLE_TOP" if seen[0] else "SINGLE_BOTTOM"
    if ev["v"] == "VALID":
        prev = last_lap.get(ch)
        if prev is None or t - prev >= cfg["minlap"]:
            ev["n"] = 1
            ev["lap"] = t - prev if prev is not None else 0
            last_lap[ch] = t
    await broadcast(ev)


async def simulate():
    chans = [Channel(i) for i in range(len(cfg["names"]))]
    status_at = 0
    while True:
        await asyncio.sleep(0.033)
        t = now_ms()
        lv = [[], []]
        for c in chans:
            if c.active and t - c.active[0] > 600:
                t0, top, bot = c.active
                c.active = None
                asyncio.create_task(pass_event(c.i, t0, top, bot))
            if not c.active and t >= c.next_pass:
                _, top, bot = random.choice(KINDS)
                c.active = (t + 400, top + random.gauss(0, 6), bot + random.gauss(0, 6))
                c.next_pass = t + random.randint(6000, 10000)
            for b in (0, 1):
                lv[b].append(int(max(1, max(c.level(b, t - k * 8) for k in range(4)))))
        await broadcast({"t": "lv", "ms": t, "l": lv})
        if t - status_at >= 1000:
            status_at = t
            board = {"ok": 1, "fps": 2010, "crc": 0, "bad": 0, "setups": 1, "drops": 0, "fill": 3, "sat": 0, "run": 1}
            await broadcast({"t": "st", "up": t // 1000, "cal": int(cal["left"] > 0), "b": [board, dict(board)]})
            if eth:
                await broadcast(net)


async def command(ws, text):
    parts = text.split(" ", 2)
    if parts[0] == "set" and len(parts) == 3:
        key, val = parts[1], parts[2]
        if eth and key in ("ip", "gw"):
            net["sip" if key == "ip" else "sgw"] = val.strip()
            await broadcast(net)
        elif key in ("channels", "ssid"):
            cfg[key] = val
            if key == "channels":
                cfg["names"] = [c.split("=")[0] for c in val.split()]
        elif key in cfg:
            cfg[key] = int(val)
        await broadcast(cfg)
    elif parts[0] == "cal":
        cal.update(left=int(parts[1]), sum=0, n=0)
    elif parts[0] == "clear":
        last_lap.clear()
    elif parts[0] == "c5":
        who = parts[1] if len(parts) > 1 else "all"
        for b in ((0, 1) if who == "all" else (int(who),)):
            await broadcast({"t": "line", "b": b, "s": "OK (simulated)"})
    else:
        await broadcast({"t": "line", "b": 2, "s": f"OK gate: {text} (simulated)"})


async def handler(ws):
    clients.add(ws)
    await ws.send(json.dumps(cfg))
    if eth:
        await ws.send(json.dumps(net))
    try:
        async for msg in ws:
            await command(ws, msg)
    finally:
        clients.discard(ws)


def http(connection, request):
    if request.path == "/ws":
        return None
    if request.path in ("/", "/index.html"):
        body = PAGE.read_bytes()
        return Response(200, "OK", Headers([("Content-Type", "text/html"), ("Content-Length", str(len(body)))]), body)
    return Response(404, "Not Found", Headers([("Content-Length", "0")]), b"")


async def main():
    async with serve(handler, "127.0.0.1", 8058, process_request=http):
        print("gate simulator: http://localhost:8058/")
        await simulate()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="A pretend gate controller for the web UI.")
    ap.add_argument("--eth", action="store_true", help="as the Teensy 4.1 controller: Ethernet, not Wi-Fi")
    eth = ap.parse_args().eth
    asyncio.run(main())
