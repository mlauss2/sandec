Single-file A/V decoder library for LucasArts SAN/SMUSH movie files (originally
intended only to play "Outlaws" videos, since they are awesome),
with an SDL2-based player application to demonstrate library use.

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
    - palette issues resulting in wrong colors and black squares in some videos.
    - sound issues in a lot of videos.
- Decodes almost all SMUSHv1/v2 (8bit-paletted) video codecs
  - frame interpolation for codec47/48 videos (default on).
  - missing codecs 31/32 (RA1 for SEGA), due to no samples available.
- Audio decoding
  - OK for COMI/Outlaws/MotS/SotE
  - acceptable for RA1/RA2/Full Throttle/The Dig
    - source is 11kHz/8bit/mono, but very crudely upsampled to 22kHz/16bit/stereo
    - volume/pan flags are currently ignored
- very good A/V sync in player
  - Outlaws IN_SHB.SAN (Level 1 Intro) the sound of the shovel hitting the ground perfectly matches the video (at around 5 minutes).
- player keyboard controls:
  - space  pause/unpause
  - f  to toggle fullscreen/windowed
  - n  to play next video in list
  - p  to toggle pause-at-every-frame
  - q  to quit
  - number keys 1-6 to display the original/double/triple/... width preserving aspect ratio
  - i  to toggle frame interpolation on codec47/48 on/off
  - s  to cycle between SDL2 texture smoothing (off/linear/best).
- tested on AMD64, ARM64, MIPS32el.
  - BE targets are untested, there are probably issues with the audio format and palette.

# What does **not** yet work:
- Bl16 Video / VIMA Audio

# Build:
- Have SDL2
- run "make"

# Use:
- sanplay [-f] [-v] [-s] [-[0..3]] <file1.san/anm> [file2.san] [file3.san]...
  - -f: start fullscreen
  - -v: be verbose
  - -s: no audio (silent)
  - -0..3: speedmode  0: normal  1: ignore frametimes (display as fast as possible)  2: just decode as fast as possible  3: pause after every frame

# Dev Notes
- look at RA1 palette code
- add the codec4/5 block smoothing code

20250228
