"""Turns a movie recorded with -Movie=<dir> (frame_00000.jpg ... and movie.wav, 30 fps) into an
H.264 / AAC MP4 with Blender's built-in FFmpeg, so no separate ffmpeg install is needed:

    blender --background --factory-startup --python Scripts/frames_to_mp4.py -- <dir> <out.mp4> [QUALITY]

QUALITY is Blender's constant rate factor (default MEDIUM, which keeps a two-minute 1600 x 900
movie well under 100 MB; HIGH for more detail). Same settings as the Java version's
blender/avi_to_mp4.py.
"""
import glob
import os
import sys

import bpy

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGS) < 2:
    sys.exit("usage: blender --background --factory-startup --python Scripts/frames_to_mp4.py -- <dir> <out.mp4> [QUALITY]")
SRC = os.path.abspath(ARGS[0])
DST = os.path.abspath(ARGS[1])
FRAMES = sorted(glob.glob(os.path.join(SRC, "frame_*.jpg")))
if not FRAMES:
    sys.exit("no frame_*.jpg in " + SRC)

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.fps = 30                       # set before adding strips so the sound strip length matches
scene.render.fps_base = 1.0
scene.sequence_editor_create()
seqs = scene.sequence_editor.strips if hasattr(scene.sequence_editor, "strips") else scene.sequence_editor.sequences
images = seqs.new_image("video", FRAMES[0], channel=1, frame_start=1)
for f in FRAMES[1:]:
    images.elements.append(os.path.basename(f))
wav = os.path.join(SRC, "movie.wav")
if os.path.exists(wav):
    sound = seqs.new_sound("audio", wav, channel=2, frame_start=1)
    print("image frames", images.frame_final_duration, "sound frames", sound.frame_duration)

w, h = images.elements[0].orig_width, images.elements[0].orig_height
scene.render.resolution_x = w or 1600
scene.render.resolution_y = h or 900
scene.render.resolution_percentage = 100
scene.frame_start = 1
scene.frame_end = images.frame_final_duration

# pass the pictures through unchanged (the default AgX view transform darkens them)
scene.view_settings.view_transform = "Standard"
scene.view_settings.look = "None"
r = scene.render
if hasattr(r.image_settings, "media_type"):  # Blender 5: video output is a separate media type
    r.image_settings.media_type = "VIDEO"
r.image_settings.file_format = "FFMPEG"
r.ffmpeg.format = "MPEG4"
r.ffmpeg.codec = "H264"
r.ffmpeg.constant_rate_factor = ARGS[2] if len(ARGS) > 2 else "MEDIUM"
r.ffmpeg.ffmpeg_preset = "GOOD"
r.ffmpeg.gopsize = 30
r.ffmpeg.audio_codec = "AAC"
r.ffmpeg.audio_channels = "STEREO"
r.ffmpeg.audio_mixrate = 44100
r.ffmpeg.audio_bitrate = 192
r.use_sequencer = True
r.filepath = DST
r.use_file_extension = False
bpy.ops.render.render(animation=True)
print("MP4 DONE", DST)
