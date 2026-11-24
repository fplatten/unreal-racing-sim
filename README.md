# Unreal Racing Sim

An Unreal Engine 5.8 port of [racing-sim](https://github.com/fplatten/racing-sim), the jMonkeyEngine open-wheel oval racing sim: a 2.5-mile banked oval, up to 33 cars with AI drivers, drafting, pit stops and a rolling start, three abreast.

The vehicle dynamics are a line-by-line C++ port of the Java model (raycast suspension, Pacejka tires, aero with drafting, gearbox, crash energy absorption), run at a fixed 240 Hz on Unreal's physics thread. The race director steps the whole field each tick, in the same order as the Java version.

## Play

Build the Windows game (Shipping) into `Package/`:

```
"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun -project="%CD%\RacingSim.uproject" -noP4 -platform=Win64 -clientconfig=Shipping -build -cook -stage -pak -archive -archivedirectory="%CD%\Package"
```

Then run `Package\Windows\RacingSim.exe`, keeping the whole `Windows` folder together. Or open `RacingSim.uproject` in Unreal Editor 5.8 and press Play.

Controls: `W`/`S` throttle and brake, `A`/`D` steer (click the mouse wheel for mouse steering), `E`/`Q` shift, `T` auto gearbox, `R` reset, `C` camera, `H` help, `G` graphics quality, `Esc` menu. An XInput racing wheel or gamepad works too.

Command line: `-Opponents=7 -Start=6 -Laps=5 -Pits=0 -Downforce=0` preset the menu, `-QuickStart` skips it, `-Autopilot` lets the AI drive your car, `-MouseSensitivity=1.5` scales mouse steering, and `-AudioDebug` logs the sound mixer's state once a second (in Development builds).

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
- `SourceArt/Liveries`: car and helmet paint, baked by the Java game's `gradlew bakeLiveries`
- `SourceArt/Generated`: sponsor boards, smoke puffs, tire and body detail maps, made by `java Scripts/BakeTextures.java SourceArt/Generated`

## Headless checks

```
UnrealEditor-Cmd.exe RacingSim.uproject /Game/Maps/Oval -game -nullrhi -nosound -SimCheck -Cars=8 -Laps=5
UnrealEditor-Cmd.exe RacingSim.uproject /Game/Maps/Oval -game -nullrhi -nosound -WallCheck
```

`-SimCheck` races an all-AI field, as the Java `gradlew simCheck -Pplayer=ai` does, and prints the lap times, contacts and classification. `-WallCheck` fires a car into the wall 54 ways, as the Java `gradlew wallCheck` does.

## Layout

| Folder | Contents |
|---|---|
| `Source/RacingSim/Sim` | Track geometry, vehicle model, AI driver, race logic: plain C++ ports of the Java classes |
| `Source/RacingSim/Track` | The procedural track, ground shading, scenery and the wall contact filter |
| `Source/RacingSim/Car` | The car pawn and tire smoke |
| `Source/RacingSim/Game` | Race director, game flow (menu, race, results) and the HUD |
| `Source/RacingSim/Audio` | Procedural engine, tire and wind sound, and the mixer |
