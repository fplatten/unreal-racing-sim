# Racing Sim bot protocol, version 1

A bot is a separate program that drives one car. Both games (this Java version and the Unreal
version) speak the same protocol, so a bot written once races in either.

- **Transport:** TCP to `127.0.0.1`, port **4510** by default. The game listens only on the
  local machine.
- **Framing:** one JSON object per line (UTF-8, `\n`-terminated). Lines from a bot are limited to
  256 KB.
- **Who speaks first:** the bot connects and sends `hello`. The game sends `welcome` and `track`
  when the race starts. After that it sends `state` every control tick, and the bot answers each
  one with `control`.

## Coordinates and signs

| Quantity | Meaning |
|---|---|
| `s` | Distance along the track's centerline, 0 .. `length` m, increasing in the direction of travel. It wraps at the start/finish line. |
| `d` | Lateral offset from the centerline, m. **Positive to the right.** |
| `curvature` | 1/m. **Positive for a left-hand bend.** |
| `headingErr`, `yawRate`, `steer` | **Positive to the right.** |
| `minD` / `maxD` | Where the walls are at `s`. This is how far a car can go; the tarmac's edges are inside it, with grass (and kerbs) between. On the road course the tarmac is about ±6.5 m. |

## Messages

### bot → game: `hello`
```json
{"type":"hello","protocol":1,"name":"MyBot","color1":"#1e88e5","color2":"#ffffff"}
```
`name` is shown in the standings. Up to 12 letters, digits, spaces and `_.-` are kept.

### game → bot: `welcome`
```json
{"type":"welcome","protocol":1,"carId":3,"name":"MyBot","mode":"race","controlHz":30,"physicsHz":240,
 "car":{"type":"formula","mass":640,"redline":12500,"gears":7,"wing":0.8,"maxBrakeDecel":58,"tireMu":1.9}}
```
`mode` is one of:
- `race`: realtime, in the game.
- `training`: lockstep, in a headless race.

### game → bot: `track`
The whole track, sent once per race:
```json
{"type":"track","layout":"road","length":7004,"step":5,
 "columns":"s,x,y,z,heading,curvature,minD,maxD,bank,grade",
 "samples":[[0,12.3,4.1,-80.2,1.5708,0.0,-16.5,13.5,0,0.002], ...]}
```
- `x`, `y`, `z` are world positions in metres, with `y` up.
- `heading` is the direction of travel (rad).
- `bank` (rad) is the banking angle; there is banking only on the oval.
- `grade` is the rise per metre.

Use these samples to plan your own racing line.

### game → bot: `state` (every control tick, 30 per second by default)
```json
{"type":"state","tick":812,"t":27.067,
 "car":{"s":1180.2,"d":-0.41,"speed":69.2,"vLong":69.1,"vLat":-0.3,"dRate":-0.1,"yawRate":0.066,
        "headingErr":0.007,"rpm":11250,"gear":6,"gLat":0.4,"gLon":0.2,"slipFront":0.01,"slipRear":0.02,
        "wheelsOnTrack":4,"draft":0,"steerAngle":0.01,"lap":1,"lapsCompleted":0,"lapTime":20.5,
        "lastLap":null,"bestLap":null,"position":4,"finished":false,"redline":12500},
 "ahead":[[10,-0.0013,-18.4,13.5],[20,-0.0012,-18.4,13.5], ... [300,0.0037,-13.5,19.0]],
 "rays":[13.2,14.0, ... 200,  ... 9.8],
 "opponents":[{"id":2,"ds":22.5,"d":0.3,"dd":0.7,"relSpeed":-1.2,"ttc":18.7,"headingErr":0.01}],
 "race":{"phase":"green","lightsOn":0,"lapsTotal":3,"cars":8,"raceTime":20.5}}
```

**`car`**

| Field | Meaning |
|---|---|
| `speed` | m/s |
| `vLong` / `vLat` | Velocity along / across the car (+ right) |
| `dRate` | Rate of change of `d` |
| `gLat` / `gLon` | g |
| `slipFront` / `slipRear` | Slip angles (rad) |
| `wheelsOnTrack` | 0–4 wheels on tarmac (fewer means on the grass) |
| `draft` | 0–1, the tow from a car ahead |
| `lapTime` | The current lap so far |
| `lastLap` / `bestLap` | Seconds, or `null` |

**The other parts of `state`**

| Field | Meaning |
|---|---|
| `ahead` | `[distance, curvature, minD, maxD]` at +10, 20 … 100, then 120 … 300 m |
| `rays` | 19 distances (m, up to 200) from the car to the nearest wall, from 90° left to 90° right of the car's nose, 10° apart (index 9 is straight ahead). They are worked out from the track's geometry, not ray-cast against meshes, so both games give the same values. |
| `opponents` | The 8 nearest cars within ±200 m. Each has `ds` (+ ahead), its `d`, `dd` (its `d` minus yours), `relSpeed` (its speed minus yours), `ttc` (time to contact if closing, else -1) and its `headingErr`. |
| `race.phase` | One of:<ul><li>`lights`: the standing start; the game holds the brakes.</li><li>`green`: racing.</li><li>`finished`: you have taken the flag.</li><li>`grid`.</li></ul> |

### bot → game: `control`
```json
{"type":"control","tick":812,"steer":0.12,"throttle":1,"brake":0,"shift":0,
 "debug":{"lines":[[1180.2,-0.41,1212.4,0.0,"#00ff00"]]}}
```

| Field | Meaning |
|---|---|
| `tick` | Echo the state's tick. |
| `steer` | −1 (full left) .. 1 (full right). The steering lock gets smaller as speed rises. |
| `throttle`, `brake` | 0 .. 1 |
| `shift` | −1, 0 or +1. Only used if the gearbox is manual; it is automatic by default. |
| `debug.lines` | Optional. Up to 64 lines in track coordinates `[s1,d1,s2,d2,"#rrggbb"]`, drawn over the track in the game. |

The game clamps every value. A missing value, or one that is not a number (NaN, inf, text), keeps the previous value.

### game → bot: `event`
```json
{"type":"event","event":"lapComplete","tick":3611,"lap":1,"lapTime":118.24,"position":3,"finished":false}
{"type":"event","event":"contact","tick":402,"other":5,"impulse":830}
{"type":"event","event":"recovered","tick":990,"s":312.0,"d":0.0}
```
`recovered` means your car crashed, got stuck for 5 s, or went beyond the walls, and was towed
back onto the track.

### game → bot: `bye`
The classification at the end:
```json
{"type":"bye","track":"road","laps":3,"order":[{"carId":1,"name":"Crimson","bot":false,"position":1,
  "laps":3,"finished":true,"finishTime":345.2,"bestLap":113.9,"distance":21012,"contacts":0,"lost":false}, ...]}
```

## Timing

- **Realtime (the game):**
  - The game never waits for a bot. Between ticks your newest `control` holds, through the 8 physics steps of each 30 Hz tick.
  - Answer within a few milliseconds.
  - If a bot sends nothing for 2 s, or disconnects, it is **lost**: the built-in AI takes its car over (or you do, if the bot was driving your car).
- **Lockstep (`botRace`, `mode: training`):**
  - The game waits for your answer to every `state`. The race is repeatable and runs as fast as your bot and the CPU allow.
  - A bot that takes longer than 30 s to answer is lost.

## Where a bot can race

- **Java, headless:**
  ```
  ./gradlew botRace -Ptrack=road -Plaps=3 -Popponents=7 -PbotCount=1 [-Plockstep=false] [-Pstart=1] [-Pseed=42] [-Presults=out.json]
  ```
  Start your bot once the race prints "Waiting for 1 bot(s)...".
- **Java, the game:**
  ```
  ./gradlew run -Pbots=4510 [-Ptrack=road]
  ```
  Or set `RACINGSIM_BOTS=4510` for the packaged game.
  - Bots that are connected when the race starts take AI cars, from the back of the grid forwards.
  - With `RACINGSIM_BOT_PLAYER=1` (or `-DbotPlayer=true`) the first bot drives your car.
- **Unreal:**
  ```
  RacingSim.exe -Bots=4510 [-BotPlayer]
  ```
  Same rules.
