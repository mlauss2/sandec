Single-file A/V decoder library for LucasArts SAN/SMUSH movie files (originally
intended only to play "Outlaws" videos, since they are awesome),
with an SDL3-based player application to demonstrate library use.

Can also play game movies from Curse of Monkey Island, Shadows of the Empire,
Mysteries of the Sith, The Dig, Full Throttle,  Rebel Assault II and Rebel Assault I.

If you find this useful, I'd be very happy if you dropped me a line!

# What works:
- Linux/Unix.
  - Windows untested, but should build.
- Can successfully parse all .SAN and .NUT files from the following LucasArts games:
  - Outlaws
  - Curse of Monkey Island
    - Text rendering is not implemented, which is most noticeable in the intro file.
  - Shadows of the Empire
  - Mysteries of the Sith
  - Full Throttle
  - The Dig
  - Rebel Assault II
    - few sound issues
  - Rebel Assault I
    - sound issues in a lot of videos.
- Video decoding
  - Handles all 8-bit codecs found in LucasArts DOS/Windows titles
  - missing codecs 31/32 (RA1 for SEGA), due to no samples available.
  - BL16 video is implemented, but still buggy.
  - frame interpolation for codec47/48 videos (default on).
- Audio decoding
  - OK for COMI/Outlaws/MotS/SotE
  - acceptable for RA1/RA2/Full Throttle/The Dig
    - source is 11kHz/8bit/mono, but very crudely upsampled to 22kHz/16bit/stereo
    - volume/pan flags are currently ignored
- very good A/V sync in player
  - Outlaws IN_SHB.SAN (Level 1 Intro) the sound of the shovel hitting the ground perfectly matches the video (at around 5 minutes).
- player keyboard controls:
  - space to pause/unpause
  - f     to toggle fullscreen/windowed
  - n     to play next video in list
  - .     to advance by 1 frame and pause
  - q     to quit
  - number keys 1-6 to display the original/double/triple/... width preserving aspect ratio
  - i  to toggle frame interpolation on codec47/48 on/off
  - s  to cycle between SDL3 texture smoothing (off/linear).
- tested on AMD64, ARM64, MIPS32el.
  - BE targets are untested, there are probably issues with the audio format and palette.

# What does **not** yet work:
- Bl16 Video / VIMA Audio

# Build:
- Have SDL3
- run "make"

# Use:
- sanplay [-f] [-v] [-s] [-[0..3]] <file1.san/anm> [file2.san] [file3.san]...
  - -f: start fullscreen
  - -v: be verbose
  - -s: no audio (silent)
  - -0..3: speedmode  0: normal  1: ignore frametimes (display as fast as possible)  2: just decode as fast as possible  3: pause after every frame

# Dev Notes
- BL16 mvecs are somtimes wrong

20250326
