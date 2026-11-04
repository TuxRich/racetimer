#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["websockets>=13"]
# ///
"""Run a race on a gate through the timer API (docs/timer-api.md), as race
software would: a working example for writing a client (FPVTrackside's
timer), and a check that a gate speaks the API.

    uv run tools/timer_api.py ws://racegate-51A0.local/api
    uv run tools/timer_api.py ws://192.168.1.50/api --mhz 5658 5732 5843 5917 --names R1 R3 R6 R8 --race 60
    uv run tools/timer_api.py ws://localhost:8058/api --race 30    # tools/gate_sim.py

Connects, maps the gate's clock onto this computer's (best of 10 round
trips), sets the channel plan if given, runs a race for --race seconds and
prints every detection with its lap time, then stops the race. With
--replay, reconnects afterwards and asks for the race's detections again.
GPL-3.0-or-later.
"""
import argparse
import asyncio
import itertools
import json
import time

from websockets.asyncio.client import connect


def now_ms():
    return time.perf_counter() * 1000  # not monotonic(): on Windows it moves in 15.6 ms steps


class Gate:
    """One connection: requests matched to replies by id, everything else
    (detections, levels, status) queued in `events`."""

    def __init__(self, ws):
        self.ws = ws
        self.ids = itertools.count(1)
        self.waiting = {}
        self.events = asyncio.Queue()
        self.hello = None
        self.reader = asyncio.create_task(self._read())

    @classmethod
    async def open(cls, url):
        gate = cls(await connect(url))
        gate.hello = await asyncio.wait_for(gate.events.get(), 5)  # the gate speaks first
        if gate.hello.get("type") != "hello" or gate.hello.get("api") != 1:
            raise RuntimeError(f"not a racetimer gate (API 1): {gate.hello}")
        return gate

    async def _read(self):
        async for text in self.ws:
            msg = json.loads(text)
            if "id" in msg and msg["id"] in self.waiting:
                self.waiting.pop(msg["id"]).set_result(msg)
            else:
                await self.events.put(msg)

    async def request(self, cmd, timeout=2, **fields):
        """Send a request and wait for its reply; an "error" reply raises."""
        rid = next(self.ids)
        reply = asyncio.get_running_loop().create_future()
        self.waiting[rid] = reply
        await self.ws.send(json.dumps({"cmd": cmd, "id": rid, **fields}))
        msg = await asyncio.wait_for(reply, timeout)
        if msg["type"] == "error":
            raise RuntimeError(f"{cmd}: {msg['error']}")
        return msg

    async def sync_clock(self, rounds=10):
        """Offset from this computer's clock to the gate's (gate = ours +
        offset), from the round trip with the shortest time: its reply is
        the one least held up, so the midpoint is the best guess."""
        best = None
        for _ in range(rounds):
            sent = now_ms()
            reply = await self.request("time")
            back = now_ms()
            rtt = back - sent
            offset = reply["time_ms"] - (sent + back) / 2
            if best is None or rtt < best[0]:
                best = (rtt, offset)
        return best

    async def close(self):
        await self.ws.close()
        self.reader.cancel()


async def race(args):
    gate = await Gate.open(args.url)
    h = gate.hello
    print(f"{h['name']}: {h['controller']} firmware {h['version']}, API {h['api']}, up to {h['max_channels']} channels")
    rtt, offset = await gate.sync_clock()
    print(f"clock: gate = ours {offset:+.1f} ms (best round trip {rtt:.1f} ms)")
    if args.mhz:
        fields = {"mhz": args.mhz}
        if args.names:
            fields["names"] = args.names
        reply = await gate.request("frequencies", **fields)
        print("channels set" + (": the receivers restart their scan" if reply["changed"] else " (unchanged)"))
        if reply["changed"]:
            await asyncio.sleep(1)  # until both receivers scan the new plan
    cfg = await gate.request("config")
    names = dict(zip(cfg["mhz"], cfg["names"]))
    print("channels: " + ", ".join(f"{n} {m} MHz" for m, n in names.items()))
    if args.levels:
        await gate.request("levels", hz=args.levels)

    start = await gate.request("start", start_ms=now_ms() + offset)
    print(f"race {start['race']} started" + ("" if start["ready"] else " (a receiver isn't streaming!)"))
    last_lap = {}  # per channel: time_ms of the last VALID detection
    seen = -1
    end = now_ms() + args.race * 1000
    while now_ms() < end:
        try:
            msg = await asyncio.wait_for(gate.events.get(), max(0.05, (end - now_ms()) / 1000))
        except asyncio.TimeoutError:
            break
        if msg["type"] == "detection":
            seen = msg["seq"]
            lap = ""
            if msg["valid"]:
                prev = last_lap.get(msg["mhz"])
                lap = f"lap {(msg['time_ms'] - prev) / 1000:.2f} s" if prev is not None else "first pass"
                last_lap[msg["mhz"]] = msg["time_ms"]
            delta = "" if msg["delta_db"] is None else f"{msg['delta_db']:+.2f} dB"
            print(f"  #{msg['seq']:<3} {msg['race_ms'] / 1000:7.2f} s  {msg['name']:>5} {msg['verdict']:<13} "
                  f"top {msg['top']:3} bottom {msg['bottom']:3} {delta:>9}  {lap}")
        elif msg["type"] == "levels" and args.levels:
            print("  levels " + " ".join(f"{n}:{t}/{b}{'*' if x else ''}"
                                         for n, t, b, x in zip(names.values(), msg["top"], msg["bottom"], msg["crossing"])))
        elif msg["type"] == "status" and not all(b["ok"] for b in msg["boards"]):
            print("  status: " + ", ".join(f"{b['name']} {'ok' if b['ok'] else 'NO DATA'}" for b in msg["boards"]))
    stop = await gate.request("stop")
    print(f"race {stop['race']} stopped: {stop['detections']} detections")
    await gate.close()

    if args.replay:  # what a client does after reconnecting mid-race
        gate = await Gate.open(args.url)
        reply = await gate.request("detections", **{"from": 0})
        got = []
        while len(got) < reply["detections"] - reply["from"]:
            msg = await asyncio.wait_for(gate.events.get(), 5)
            if msg["type"] == "detection":
                got.append(msg["seq"])
        print(f"replayed detections {got[0] if got else '-'}..{got[-1] if got else '-'} "
              f"of race {reply['race']}" + ("" if got == list(range(reply["from"], reply["detections"])) else " OUT OF ORDER"))
        await gate.close()
    return seen


def main():
    ap = argparse.ArgumentParser(description="Run a race on a gate through the timer API.")
    ap.add_argument("url", help="the gate's timer API, e.g. ws://racegate-51A0.local/api")
    ap.add_argument("--race", type=float, default=30, help="seconds (default 30)")
    ap.add_argument("--mhz", type=int, nargs="+", help="channel plan to set first, MHz")
    ap.add_argument("--names", nargs="+", help="their names (1 to 5 letters or digits each)")
    ap.add_argument("--levels", type=int, default=0, help="also print levels this many times a second")
    ap.add_argument("--replay", action="store_true", help="reconnect after the race and fetch its detections again")
    asyncio.run(race(ap.parse_args()))


if __name__ == "__main__":
    main()
