# Timer API

How race software uses a racetimer gate as its lap timer. It sets the
channels, starts and stops races, and gets every pass through the gate as a
**detection** with its verdict. It's written for a new FPVTrackside timer,
but any program can use it.

> **Status:** API version 1. On the Teensy 4.1 controller (`gate-teensy/`)
> and in the simulator (`tools/gate_sim.py`). Not yet on the ESP32-WROOM
> controller, and no FPVTrackside timer uses it yet.

- Code: `gate/src/gate_api.c` (protocol, target independent), the
  controller's web server (transport). Host tests: `gate/test/test_api`.
- Try it: `uv run tools/gate_sim.py`, then
  `uv run tools/timer_api.py ws://localhost:8058/api --race 30`.
  `tools/timer_api.py` is a complete client in Python: use it as the
  reference when writing one.

## Connection

- **WebSocket** at `ws://<gate>/api`, port 80. `<gate>` is
  `racegate-XXXX.local` or the gate's IP address (see
  [teensy.md](teensy.md#network)). The page uses `/ws` on the same port.
- **Text frames, one JSON object each,** UTF-8.
- **The gate speaks first:** a `hello` as soon as the connection is up.
  After that, a `status` every second: no message for 3 seconds means the
  connection is dead; close it and connect again.
- Requests on one connection are answered in order. Replies and events
  (detections, levels, status) interleave, so match replies by `id`.
- Several clients can connect at once. There is one race at a time, and
  every client is sent every detection.
- No passwords: keep the gates on your own network.

## Time

Every time in the API is on the **gate's clock**: milliseconds since the
gate booted, in `time_ms` fields. Race times are also given relative to the
race's start, in `race_ms`.

To turn gate times into your own, ask for the gate's time (`time`) about ten
times and keep the round trip that took the least time:

```
sent = local_now(); reply = request("time"); back = local_now()
offset = reply.time_ms - (sent + back) / 2       # gate = local + offset
```

Sync again before each race: two clocks drift apart by up to tens of
milliseconds an hour. Lap times are differences of gate times, so drift
never affects them; it only moves where the whole race sits on your clock.

## Requests

A request is a JSON object with `cmd` and, optionally, a whole-number `id`,
which the reply repeats:

```json
{"cmd": "start", "id": 12, "start_ms": 5123456.5}
```

The reply has the request's `cmd` and `id`, and a `type`: `ok`, or a data
type (`hello`, `time`, `config`), or `error`:

```json
{"type": "ok", "cmd": "start", "id": 12, "race": 7, "start_ms": 5123457, "ready": true}
{"type": "error", "cmd": "start", "id": 12, "error": "start_ms: more than a minute from the gate's clock (sync with time first)"}
```

An `error` changes nothing on the gate. Unknown fields in a request are
ignored.

| `cmd` | Fields | Reply | What it does |
|---|---|---|---|
| `hello` | | `hello` | The gate's identity again (it's also sent unasked on connecting). |
| `time` | | `time`: `time_ms` | The gate's clock now, in ms with three decimals. |
| `config` | | `config` | The channel plan and the gate's detection settings. |
| `frequencies` | `mhz`, `names` (optional) | `ok`: `changed` | Sets the channel plan. |
| `start` | `start_ms` (optional) | `ok`: `race`, `start_ms`, `ready` | Starts a race. |
| `stop` | | `ok`: `race`, `detections` | Stops the race. |
| `detections` | `from` | `ok`: `race`, `detections`, `from` | Sends the race's detections again, from one on. |
| `levels` | `hz` | `ok`: `hz` | Streams the live levels (0 to stop). |

### `frequencies`

```json
{"cmd": "frequencies", "id": 3, "mhz": [5658, 5732, 5843, 5917], "names": ["R1", "R3", "R6", "R8"]}
→ {"type": "ok", "cmd": "frequencies", "id": 3, "changed": true}
```

- `mhz`: 1 to 8 different frequencies, whole MHz, 5345 to 5945.
- `names`: one per frequency, 1 to 5 letters or digits each. Without it,
  the frequency is the name ("5658"). The gate's page shows them.
- `changed: true`: the receivers restart their scan on the new plan, and
  for about 0.6 s nothing is detected. `false`: it was the plan already,
  and nothing restarts. Send it before every race; that's cheap.
- The plan is used at once but not saved: after a reboot the gate scans the
  plan saved on its page until a client sets one.

### `start`

```json
{"cmd": "start", "id": 4, "start_ms": 5123456.5}
→ {"type": "ok", "cmd": "start", "id": 4, "race": 7, "start_ms": 5123457, "ready": true}
```

- `start_ms`: when the race starts, on the gate's clock (your start time
  plus the offset). Left out: when the request arrives. More than a minute
  from the gate's clock is refused, as a client that hasn't synced.
- The gate forgets its laps, and any pass under way, and keeps every
  detection from now. They are numbered from 0 (`seq`). A start while a
  race runs starts a new race.
- `race`: the race's number, one up from the last since the gate booted.
- `ready`: both receivers are streaming levels. `false` means detections
  will be missed; the gate's page shows which receiver.

### `stop`

```json
{"cmd": "stop", "id": 5}
→ {"type": "ok", "cmd": "stop", "id": 5, "race": 7, "detections": 42}
```

No more detections are kept. Those kept stay until the next `start`, for
`detections`. Stop is the same for a normal end and an abort.

### `detections`

```json
{"cmd": "detections", "id": 6, "from": 17}
→ {"type": "ok", "cmd": "detections", "id": 6, "race": 7, "detections": 42, "from": 17}
```

The reply, then detections `from` to the latest, in order, then new ones
as they come. Use it after reconnecting mid-race, from the last `seq` you
had plus one. `race` says which race they are; if it isn't yours, your race
is gone (another `start` replaced it). The Teensy keeps a race's last 1024
detections; asked for older ones, it starts from the oldest it has (reply
`from`).

### `levels`

```json
{"cmd": "levels", "id": 7, "hz": 10}
→ {"type": "ok", "cmd": "levels", "id": 7, "hz": 10}
```

`hz`: 1 to 30 `levels` messages a second; 0 stops them (the default on
connecting).

## Messages from the gate

Fields may be added in later versions: ignore those you don't know, and
message types you don't know.

### `hello`

```json
{"type": "hello", "api": 1, "name": "racegate-51A0", "controller": "teensy41", "version": "eab98cb",
 "boards": 2, "max_channels": 8, "time_ms": 5120000.125}
```

| Field | |
|---|---|
| `api` | This API's version: 1. A gate that says otherwise has changed incompatibly. |
| `name` | The gate's name on the network. |
| `controller` | `teensy41`; `simulator` from `tools/gate_sim.py`. |
| `version` | The firmware (`git describe`). |
| `boards` | Receivers: 2 (top and bottom). |
| `max_channels` | Channels in a plan, at most: 8. |
| `time_ms` | The gate's clock now. |

On connecting, a client is sent only detections that come after its
`hello`. For earlier ones, ask with `detections`.

### `detection`

One pass through the gate (or a near one), once both receivers have had
their say:

```json
{"type": "detection", "race": 7, "seq": 12, "mhz": 5658, "name": "R1", "time_ms": 5145678, "race_ms": 22221,
 "verdict": "VALID", "valid": true, "counted": true, "peak": 152, "top": 152, "bottom": 148, "delta_db": 1.0}
```

| Field | |
|---|---|
| `race`, `seq` | The race's number, and this detection's number in it (0, 1, …, in order). |
| `mhz`, `name` | The channel. |
| `time_ms` | When the quad passed: the middle of the signal's peak, on the gate's clock. |
| `race_ms` | The same, from the race's `start_ms`. Can be slightly negative for a pass right at the start. |
| `verdict` | `VALID`, `OVER`, `UNDER`, `SINGLE_TOP` or `SINGLE_BOTTOM` (below). |
| `valid` | `verdict` is `VALID`: a pass through the gate. |
| `counted` | The gate's own lap rules (its minimum lap time) count it as a lap. Your software has its own rules; this is for reference. |
| `peak` | The higher of `top` and `bottom`. |
| `top`, `bottom` | Each receiver's peak level, 0 when it didn't see the pass. Level codes: 0.25 dB steps, dBFS = code / 4 − 63.75. |
| `delta_db` | Top − bottom in dB, less the gate's calibrated offset; `null` for a single. |

| `verdict` | Meaning | A lap? |
|---|---|---|
| `VALID` | Both receivers saw the pass, at levels within the gate's Δ limit of each other: through the gate. | Yes |
| `OVER` | Both saw it; the top much stronger: over the gate. | No |
| `UNDER` | Both saw it; the bottom much stronger: under it. | No |
| `SINGLE_TOP`, `SINGLE_BOTTOM` | Only one receiver saw it above EnterAt: past one bar, outside the gate. | No |

A detection comes 0.2 to 0.5 s after the pass (the level has to fall back
below ExitAt on both receivers first); a single comes after the gate's pair
wait (1 s by default). `time_ms` is the pass itself either way.

### `levels`

```json
{"type": "levels", "time_ms": 5145700, "mhz": [5658, 5732, 5843, 5917],
 "top": [152, 31, 30, 29], "bottom": [148, 30, 33, 30], "crossing": [true, false, false, false]}
```

Per channel, in the plan's order: each receiver's highest level over the
last ~33 ms (level codes, as above), and whether either receiver is in a
crossing (above EnterAt, not yet below ExitAt).

### `status`

Every second, to every client:

```json
{"type": "status", "time_ms": 5146000,
 "race": {"no": 7, "running": true, "start_ms": 5123457, "detections": 13},
 "boards": [{"name": "top", "ok": true, "scanning": true, "hops_per_s": 1897, "crc_errors": 0,
             "bad_frames": 0, "setups": 1, "drops": 0, "clipped": 0},
            {"name": "bottom", "ok": true, "scanning": true, "hops_per_s": 1897, "crc_errors": 0,
             "bad_frames": 0, "setups": 1, "drops": 0, "clipped": 0}]}
```

| Field | |
|---|---|
| `race` | The current or last race: its number (0: none yet), whether it runs, its start, its detections so far. |
| `ok` | The receiver's levels are coming in (within the last second). |
| `scanning` | The receiver reports that it is scanning. |
| `hops_per_s` | Level updates a second over the last second: about 1900 for 4 channels. |
| `crc_errors`, `bad_frames` | Damaged data on the receiver's cable, since boot. A few is normal; climbing means a bad joint. |
| `setups` | Times the gate set the receiver up (more than one: it restarted, or the plan changed). |
| `drops` | Updates the receiver couldn't send. |
| `clipped` | Updates with the receiver's input clipping: lower its gain. |

## The settings

`config` shows them:

```json
{"type": "config", "cmd": "config", "id": 2, "mhz": [5658, 5732, 5843, 5917], "names": ["R1", "R3", "R6", "R8"],
 "enter": 125, "exit": 105, "gain": 45, "delta_db": 6.0, "offset_db": 0.0, "pair_ms": 75, "wait_ms": 1000,
 "min_lap_ms": 5000}
```

`enter`, `exit`: EnterAt and ExitAt, level codes. `delta_db`: the Δ limit
between VALID and OVER/UNDER. `offset_db`: the calibrated top − bottom.
`pair_ms`: how far apart the two receivers' passes may be. `wait_ms`: how
long a pass waits for the other receiver's before it is a single.
`min_lap_ms`: the gate's own minimum lap (`counted`). Set them on the
gate's page; the API doesn't change them (version 1).

## Writing the FPVTrackside timer

FPVTrackside's timers implement `ITimingSystem` (and `ITimingSystemWithRSSI`
for the live levels) in its `Timing` project
([FPVTracksideCore](https://github.com/uewepuep/FPVTracksideCore),
`Timing/ITimingSystem.cs`). The RotorHazard timer there
(`Timing/RotorHazard/RotorHazardTimingSystem.cs`) is the closest model: a
network timer with a clock sync and blocking requests. The members map onto
this API like this:

| `ITimingSystem` | With this API |
|---|---|
| `Connect()` | Open the WebSocket, read `hello` and check `api` is 1. Sync the clock. If you show RSSI, send `levels` (`hz` 10). Start a thread that reads messages. Return whether all that worked. |
| `Connected` | The socket is open and a message came within the last 3 s. |
| `Disconnect()` | Close the socket. |
| `SetListeningFrequencies(list)` | `frequencies`: `mhz` from each `Frequency`, `names` from `Band` and `Channel` (e.g. "R1"; letters and digits only, at most 5). Return true on `ok`, false on `error` or no reply in 2 s. If `changed`, wait until `status` shows both boards `ok` (about a second). |
| `StartDetection(ref time, meta)` | Sync the clock again, then `start` with `start_ms` = `time` on the gate's clock. Set `time` to the reply's `start_ms` on yours. Return true on `ok`; return `ready` instead if a race shouldn't start with a receiver down. |
| `EndDetection(type)` | `stop`. |
| `OnConnected(raceRunning)` | Not running: `stop` (a race left from before). Running: `detections` from your last `seq` + 1, if `status` still shows your race number. |
| `OnDetectionEvent` | Raise it for each `detection` with `valid` true: frequency `mhz`, time `time_ms` on your clock, peak `peak`. Log the rest. |
| `MaxPilots` | `hello.max_channels` (8). |
| `Status` | From `status`: e.g. "top 1897/s" with `ok`, "bottom …" likewise. |
| `GetRSSI()` | From the latest `levels`: per channel, `Frequency` = `mhz`, `CurrentRSSI` = the higher of `top` and `bottom`, `Detected` = `crossing`, and a scale around EnterAt/ExitAt from `config`, e.g. `exit` − 20 to `enter` + 60. |
| `Name` | Something short, e.g. "RG". |
| `Settings` | Its own `TimingSystemSettings` subclass: the gate's host name or address (`HostName`, as RotorHazard's and LapRF's have). |
| `Type` | A new `TimingSystemType` value. |

`ListeningFrequency.SensitivityFactor` has no equivalent: the gate's
thresholds are set on its page.

Where a new timer plugs into FPVTrackside (as of its October 2026 source):

1. `Timing/RaceTimer/`: the timer class and its settings class.
2. `Timing/ITimingSystem.cs`: the new `TimingSystemType` value, and
   `[XmlInclude(typeof(…Settings))]` on `TimingSystemSettings`, so the
   settings save.
3. `Timing/TimingSystemManager.cs`, `CreateTimingSystem`: an
   `else if (settings is …Settings)` that makes the timer.
4. `UI/Nodes/TimingSystemEditor.cs`: a `mouseMenu.AddItem(…)` to add one,
   and the settings type in `Hostnames` and `IsTestable`, as the other
   network timers are.

The clock and the conversion in C#:

```csharp
// gate = local + offset, in ms. local: a monotonic clock, as RotorHazard's timer uses.
static double LocalMs => Environment.TickCount64;   // or a Stopwatch for sub-ms
double offsetMs;
DateTime epoch;   // the DateTime of local ms 0

void SyncClock()
{
    double bestRtt = double.MaxValue;
    for (int i = 0; i < 10; i++)
    {
        double sent = LocalMs;
        JsonElement reply = Request(new { cmd = "time" });   // blocks for the reply with its id
        double back = LocalMs;
        if (back - sent < bestRtt)
        {
            bestRtt = back - sent;
            offsetMs = reply.GetProperty("time_ms").GetDouble() - (sent + back) / 2;
        }
    }
    epoch = DateTime.Now - TimeSpan.FromMilliseconds(LocalMs);
}

double GateMs(DateTime t) => (t - epoch).TotalMilliseconds + offsetMs;
DateTime LocalTime(double gateMs) => epoch + TimeSpan.FromMilliseconds(gateMs - offsetMs);

// StartDetection:  start_ms = GateMs(time);  ...  time = LocalTime(reply.start_ms);
// detection:       OnDetectionEvent?.Invoke(this, mhz, LocalTime(time_ms), peak);
```

`Environment.TickCount64` has a resolution of about 15 ms on Windows; a
`Stopwatch` (`Stopwatch.GetTimestamp()` / `Stopwatch.Frequency`) gives a
better clock sync.

## Example session

`→` from the client, `←` from the gate:

```
← {"type":"hello","api":1,"name":"racegate-51A0","controller":"teensy41","version":"eab98cb","boards":2,"max_channels":8,"time_ms":5120000.125}
→ {"cmd":"time","id":1}
← {"type":"time","cmd":"time","id":1,"time_ms":5120004.512}
   … nine more …
→ {"cmd":"frequencies","id":11,"mhz":[5658,5732,5843,5917],"names":["R1","R3","R6","R8"]}
← {"type":"ok","cmd":"frequencies","id":11,"changed":false}
→ {"cmd":"start","id":12,"start_ms":5123456.5}
← {"type":"ok","cmd":"start","id":12,"race":7,"start_ms":5123457,"ready":true}
← {"type":"status","time_ms":5124000,"race":{"no":7,"running":true,"start_ms":5123457,"detections":0},"boards":[…]}
← {"type":"detection","race":7,"seq":0,"mhz":5658,"name":"R1","time_ms":5131011,"race_ms":7554,"verdict":"VALID","valid":true,"counted":true,"peak":153,"top":153,"bottom":149,"delta_db":1.0}
← {"type":"detection","race":7,"seq":1,"mhz":5843,"name":"R6","time_ms":5133520,"race_ms":10063,"verdict":"OVER","valid":false,"counted":false,"peak":179,"top":179,"bottom":144,"delta_db":8.75}
   …
→ {"cmd":"stop","id":13}
← {"type":"ok","cmd":"stop","id":13,"race":7,"detections":42}
```

## Limits

- One race at a time, on one gate. With several gates, each is its own
  timer (FPVTrackside: primary and split timers), each with its own clock
  sync.
- 8 channels, one quad on each.
- The Teensy keeps 1024 detections a race for `detections`; live ones are
  sent whatever the count.
- A detection that can't be sent waits on the gate until the connection
  takes it. A client that takes nothing for 5 seconds, while the gate has
  something for it, is disconnected.
