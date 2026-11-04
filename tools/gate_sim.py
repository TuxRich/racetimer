#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["websockets>=13"]
# ///
"""A pretend gate controller for working on the web UI, or on race
software's timer, without hardware.

    uv run tools/gate_sim.py          # then open http://localhost:8058/
    uv run tools/gate_sim.py --eth    # as the Teensy 4.1 controller (Ethernet)

Serves gate/web/index.html and a /ws WebSocket that speaks the gate's
messages (src/web.h): levels at ~30 per second, and on each channel a pass
every 6-10 s that goes through the middle, near a bar, over, under, or past
one bar only. Verdicts follow gate_fusion.c's rules. Commands from the page
change the simulated settings. ws://localhost:8058/api is the timer API
(docs/timer-api.md), as gate/src/gate_api.c speaks it, on the simulated
passes. GPL-3.0-or-later.
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
# The timer API: the race and its detections, and each client's place in them
API_COMMANDS = ("hello", "time", "config", "frequencies", "start", "stop", "detections", "levels")
race = {"no": 0, "running": False, "start_ms": 0, "kept": []}
api_clients = {}


def now_ms():
    return int(time.monotonic() * 1000)


def plan():
    """The channel plan: [(name, MHz)]."""
    return [(c.split("=")[0], int(c.split("=")[1])) for c in cfg["channels"].split()]


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
    if race["running"] and ch < len(plan()):
        name, mhz = plan()[ch]
        race["kept"].append({"type": "detection", "race": race["no"], "seq": len(race["kept"]), "mhz": mhz,
                             "name": name, "time_ms": t, "race_ms": t - race["start_ms"], "verdict": ev["v"],
                             "valid": ev["v"] == "VALID", "counted": bool(ev["n"]), "peak": max(ev["top"], ev["bot"]),
                             "top": ev["top"], "bottom": ev["bot"],
                             "delta_db": None if ev["v"].startswith("SINGLE") else ev["d"] / 4})


async def simulate():
    chans = []
    status_at = 0
    while True:
        await asyncio.sleep(0.033)
        t = now_ms()
        if len(chans) != len(plan()):  # a new channel plan
            chans = [Channel(i) for i in range(len(plan()))]
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
            await broadcast({"t": "st", "up": t // 1000, "cal": int(cal["left"] > 0),
                             "race": race["no"] if race["running"] else 0, "b": [board, dict(board)]})
            if eth:
                await broadcast(net)
        await api_poll(t, lv)


# ---- The timer API (docs/timer-api.md), as gate/src/gate_api.c speaks it

def api_hello():
    return {"api": 1, "name": net["name"], "controller": "simulator", "version": "gate_sim", "boards": 2,
            "max_channels": 8, "time_ms": round(time.monotonic() * 1000, 3)}


def api_status(t):
    board = {"ok": True, "scanning": True, "hops_per_s": 2010, "crc_errors": 0, "bad_frames": 0, "setups": 1,
             "drops": 0, "clipped": 0}
    return {"type": "status", "time_ms": t,
            "race": {"no": race["no"], "running": race["running"], "start_ms": race["start_ms"],
                     "detections": len(race["kept"])},
            "boards": [{"name": "top", **board}, {"name": "bottom", **board}]}


def api_request(st, text):
    try:
        req = json.loads(text)
    except ValueError:
        req = None
    if not isinstance(req, dict):
        return {"type": "error", "error": "not a JSON object"}
    cmd = req.get("cmd")
    rid = req.get("id") if isinstance(req.get("id"), int) and not isinstance(req.get("id"), bool) else None

    def reply(kind, **fields):
        msg = {"type": kind}
        if cmd in API_COMMANDS:
            msg["cmd"] = cmd
        if rid is not None:
            msg["id"] = rid
        return {**msg, **fields}

    def number(key):
        v = req.get(key)
        return v if isinstance(v, (int, float)) and not isinstance(v, bool) else None

    if cmd is None:
        return reply("error", error="no cmd")
    if cmd not in API_COMMANDS:
        return reply("error", error="unknown cmd")
    if cmd == "hello":
        return reply("hello", **api_hello())
    if cmd == "time":
        return reply("time", time_ms=round(time.monotonic() * 1000, 3))
    if cmd == "config":
        return reply("config", mhz=[m for _, m in plan()], names=[n for n, _ in plan()], enter=cfg["enter"],
                     exit=cfg["exit"], gain=cfg["gain"], delta_db=cfg["delta"] / 4, offset_db=cfg["offset"] / 4,
                     pair_ms=cfg["pair"], wait_ms=cfg["wait"], min_lap_ms=cfg["minlap"])
    if cmd == "frequencies":
        mhz = req.get("mhz")
        if not isinstance(mhz, list) or not 1 <= len(mhz) <= 8:
            return reply("error", error="mhz: 1 to 8 frequencies")
        if any(not isinstance(m, (int, float)) or isinstance(m, bool) or not 5345 <= m <= 5945 or m != int(m)
               for m in mhz):
            return reply("error", error="mhz: 5345 to 5945, whole MHz")
        if len(set(mhz)) != len(mhz):
            return reply("error", error="mhz: the same frequency twice")
        names = req.get("names", [str(int(m)) for m in mhz])
        if not isinstance(names, list) or len(names) != len(mhz):
            return reply("error", error="names: one per frequency")
        if any(not isinstance(n, str) or not n.isascii() or not n.isalnum() or not 1 <= len(n) <= 5 for n in names):
            return reply("error", error="names: 1 to 5 letters or digits each")
        channels = " ".join(f"{n}={int(m)}" for n, m in zip(names, mhz))
        changed = channels != cfg["channels"]
        if changed:
            cfg["channels"], cfg["names"] = channels, list(names)
            last_lap.clear()
            asyncio.get_running_loop().create_task(broadcast(cfg))
        return reply("ok", changed=changed)
    if cmd == "start":
        now = time.monotonic() * 1000
        start = now if "start_ms" not in req else number("start_ms")
        if start is None:
            return reply("error", error="start_ms: a number")
        if abs(start - now) > 60000:
            return reply("error", error="start_ms: more than a minute from the gate's clock (sync with time first)")
        race.update(no=race["no"] + 1, running=True, start_ms=round(start), kept=[])
        last_lap.clear()
        st.update(race=race["no"], next=0)
        return reply("ok", race=race["no"], start_ms=race["start_ms"], ready=True)
    if cmd == "stop":
        race["running"] = False
        return reply("ok", race=race["no"], detections=len(race["kept"]))
    if cmd == "detections":
        first = number("from")
        if first is None or first < 0 or first != int(first):
            return reply("error", error="from: a detection number")
        st.update(race=race["no"], next=int(first))
        return reply("ok", race=race["no"], detections=len(race["kept"]), **{"from": int(first)})
    hz = number("hz")  # levels
    if hz is None or not 0 <= hz <= 30 or hz != int(hz):
        return reply("error", error="hz: 0 (off) to 30")
    st["hz"] = int(hz)
    return reply("ok", hz=int(hz))


async def api_poll(t, lv):
    """Each API client's due messages: detections, levels, status."""
    for ws, st in list(api_clients.items()):
        msgs = []
        if st["race"] != race["no"]:  # a new race: from its first detection
            st.update(race=race["no"], next=0)
        msgs += race["kept"][st["next"]:]
        st["next"] = len(race["kept"])
        period = 1000 // st["hz"] if st["hz"] else 0
        if period and t - st["levels_at"] >= period:
            st["levels_at"] = t if t - st["levels_at"] >= 2 * period else st["levels_at"] + period
            msgs.append({"type": "levels", "time_ms": t, "mhz": [m for _, m in plan()], "top": lv[0], "bottom": lv[1],
                         "crossing": [max(a, b) >= cfg["enter"] for a, b in zip(lv[0], lv[1])]})
        if t - st["status_at"] >= 1000:
            st["status_at"] = t
            msgs.append(api_status(t))
        try:
            for m in msgs:
                await ws.send(json.dumps(m))
        except Exception:
            api_clients.pop(ws, None)


async def api_handler(ws):
    api_clients[ws] = {"race": race["no"], "next": len(race["kept"]), "hz": 0, "levels_at": 0,
                       "status_at": now_ms()}
    await ws.send(json.dumps({"type": "hello", **api_hello()}))
    try:
        async for text in ws:
            await ws.send(json.dumps(api_request(api_clients[ws], text)))
    finally:
        api_clients.pop(ws, None)


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
    if ws.request.path == "/api":
        return await api_handler(ws)
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
    if request.path in ("/ws", "/api"):
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
