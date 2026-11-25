# Unreal Racing Sim

An Unreal Engine 5.8 port of [racing-sim](https://github.com/fplatten/racing-sim), the jMonkeyEngine open-wheel oval racing sim: a 2.5-mile banked oval, up to 33 cars with AI drivers, drafting, pit stops and a rolling start, three abreast.

The oval is raced in a 1995-style Indy car (the Java game's car): 780 kg, ~870 hp at speedway boost, ~235 mph flat out without a tow. Its liveries are baked textures.

The road course is raced in a 1995-style Formula car, modeled in Blender by `Scripts/make_f1_car.py` (raised nose with the front wing hung beneath it, tall airbox, sidepods, two-element rear wing, 13" wheels with tall slick sidewalls): 4.42 m (14.5 ft) long, 2.90 m wheelbase, 2.00 m wide, 600 kg, ~700 hp. It reaches 215 mph flat out without a tow at minimum wing, does 0-100 mph in about 3.5 s, makes 1,000-1,500 kg of downforce at 165-200 mph, and stops from 200 mph in 118-129 m (5-5.5 g at the start of the stop). Each car's paint (primary, secondary and accent colors) is applied at run time. Both cars are presets in `Sim/VehicleModel` (`FVehicleSpec::Indy()`, `FVehicleSpec::Formula()`); `-Car=indy` or `-Car=f1` races either car on either track.

A second track, the **Road** course: a 4.352-mile (7,004 m) clockwise road course traced from a circuit map, with 102.2 m of elevation (down into a valley, up a steep climb and a long straight to the summit, then down through the hairpins and sweeps to the lowest point), kerbs, grass run-off, barriers, wooded hills, and a standing start behind five red lights. Choose OVAL or ROAD on the menu's TRACK row (the level reloads) or with `-Track=road`. The AI drives a minimum-curvature racing line with braking points worked out from grip, downforce, the turn and the slope; the Indy car (with `-Car=indy`) runs shorter road-course gearing there, and every car runs more wing. The road course has no pit lane, so pit stops are oval only. How the layout was made: `Scripts/road_track/`.

The vehicle dynamics are a line-by-line C++ port of the Java model (raycast suspension, Pacejka tires, aero with drafting, gearbox, crash energy absorption), run at a fixed 240 Hz on Unreal's physics thread. The race director steps the whole field each tick, in the same order as the Java version.

## Play

Build the Windows game (Shipping) into `Package/`:

```
"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun -project="%CD%\RacingSim.uproject" -noP4 -platform=Win64 -clientconfig=Shipping -build -cook -stage -pak -archive -archivedirectory="%CD%\Package"
```

Then run `Package\Windows\RacingSim.exe`, keeping the whole `Windows` folder together. Or open `RacingSim.uproject` in Unreal Editor 5.8 and press Play.

Controls: `W`/`S` throttle and brake, `A`/`D` steer (click the mouse wheel for mouse steering), `E`/`Q` shift, `T` auto gearbox, `R` reset, `C` camera, `H` help, `G` graphics quality, `Esc` menu.

Racing wheels and pedals work out of the box: DirectInput wheels (Logitech, Thrustmaster, Fanatec and most others) and XInput devices (Xbox pads and wheels in Xbox mode). The wheel steers, the pedals are throttle and brake, the paddles shift, and the wheel's buttons and D-pad work the menus. Wheels with force feedback get a centering spring that stiffens with speed. To remap axes or buttons, invert the steering or change the force feedback, copy `controller.properties.example` to `controller.properties` next to `RacingSim.exe` and edit it; the wheel and its axes are written to the log when it connects.

Command line: `-Track=road` races on the road course, `-Opponents=7 -Start=6 -Laps=5 -Pits=0 -Downforce=0` preset the menu, `-QuickStart` skips it, `-Autopilot` lets the AI drive your car, `-MouseSensitivity=1.5` scales mouse steering, and `-AudioDebug` logs the sound mixer's state once a second (in Development builds).

No sound? If the log says `InitializeHardware() failed. Falling back to null device`, Windows refused the speakers: untick *Allow applications to take exclusive control of this device* in the speaker's Advanced properties, or restart the Windows Audio service.

## Build from source

Requirements: Unreal Engine 5.8, and Visual Studio 2022 Build Tools with the C++ workload and the .NET Framework 4.8 SDK.

```
"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" RacingSimEditor Win64 Development -Project="%CD%\RacingSim.uproject"
```

`Content/` is generated from `SourceArt/` by `Scripts/setup_content.py`. Rerun it after changing the art:

```
"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" RacingSim.uproject -run=pythonscript -script="Scripts/setup_content.py" -unattended -nosplash
```

- `SourceArt/Models`, `SourceArt/Textures`: the Java game's Blender models and textures
- `SourceArt/Liveries`: the Indy car's livery and helmet paint, baked by the Java game's `gradlew bakeLiveries`
- `SourceArt/Generated`: sponsor boards, smoke puffs, tire and body detail maps, made by `java Scripts/BakeTextures.java SourceArt/Generated`

## Headless checks

```
UnrealEditor-Cmd.exe RacingSim.uproject /Game/Maps/Oval -game -nullrhi -nosound -SimCheck -Cars=8 -Laps=5
UnrealEditor-Cmd.exe RacingSim.uproject /Game/Maps/Oval -game -nullrhi -nosound -WallCheck
```

`-SimCheck` races an all-AI field, as the Java `gradlew simCheck -Pplayer=ai` does, and prints the lap times, contacts and classification, plus the AI's race craft: time off the racing line (and why), time three wide, passes started, completed and abandoned, and emergency moves. Add `-Seconds=600` for longer races (the default stops after 260 s). For tuning, `-AiGrip=`, `-AiGripSpread=`, `-AiDownforce=`, `-AiWingMax=`, `-AiSling=`, `-AiStallGain=`, `-AiPullOut=`, `-AiDirtyAir=`, `-AiLookBase=`, `-AiLookK=`, `-AiLatGain=`, `-AiLatDamp=`, `-AiRunGap=`, `-AiRunDecel=`, `-AiRunPull=`, `-AiRoomClose=` and `-AiSetupGap=` override the AI's settings (see `Sim/AIDriver.cpp`), `-AiBrakeGrip=` sets how much of the grip the AI brakes with on a road course, `-Track=road` runs the check on the road course, and `-PowerScale=` scales every car's engine. `-CarCheck` measures a car on the oval's front straight (the Indy car, or the Formula car with `-Car=f1`) at minimum and maximum wing: 0-60 and 0-100 mph, the stop from 200 mph and the top speed. `-WallCheck` fires a car into the wall 54 ways, as the Java `gradlew wallCheck` does.

## Layout

| Folder | Contents |
|---|---|
| `Source/RacingSim/Sim` | Track geometry, vehicle model, AI driver, race logic: plain C++ ports of the Java classes |
| `Source/RacingSim/Track` | The procedural track, ground shading, scenery and the wall contact filter |
| `Source/RacingSim/Car` | The car pawn and tire smoke |
| `Source/RacingSim/Game` | Race director, game flow (menu, race, results) and the HUD |
| `Source/RacingSim/Audio` | Procedural engine, tire and wind sound, and the mixer |
