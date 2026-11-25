# Road course centerline

How `Source/RacingSim/Sim/RoadLayout.inl` was made from a circuit map (1920x1080, the track drawn
as red / cyan / yellow sector lines). Run in this folder with the map saved as `map.jpg`:

1. `javac Extract.java && java Extract map.jpg` - the colored line pixels, clustered (`pts.txt`).
2. `python order.py` - ordered into a path from the start line (`path.txt`).
3. `python process.py` - resampled, lightly smoothed, scaled to 7,004 m, map axes to Unreal
   axes (`world.txt`); prints each corner's radius.
4. `python shape.py` - the map draws corners too tight: the heading is smoothed with a width set
   per section (wide on the straights and fast corners, narrow at the hairpins and the last
   chicane), so every corner keeps the map's angle with a realistic radius; the loop is closed
   (`shaped.txt`). `python peaks.py` lists the corners (angle, tightest radius).
5. `python emit.py` - starts the lap at the start/finish line, writes a point every ~5 m
   (`RoadLayout.inl`) and `final_px.txt`; `java Overlay map.jpg final_px.txt overlay.png` draws
   the result over the map.

The height profile (102.2 m from the lowest point to the summit) is in `TrackSpline.cpp`
(`RoadElevation`).
