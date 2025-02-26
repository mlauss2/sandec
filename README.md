Single-file A/V decoder library for LucasArts SAN/SMUSH movie files (originally
intended only to play "Outlaws" videos, since they are awesome),
with a simple SDL2-based player application to demonstrate library use.

Can also play game movies from Curse of Monkey Island, Shadows of the Empire,
Mysteries of the Sith, The Dig, Full Throttle,  almost all of Rebel Assault II
and roughly half of Rebel Assault I.

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
- Decodes almost all SMUSHv1/v2 (8bit-paletted) video codecs
  - frame interpolation for codec47/48 videos (default on).
  - missing codecs 31/32 (RA1 for SEGA), due to no samples available.
- Audio decoding
  - OK for COMI/Outlaws/MotS/SotE
  - so-so for Full Throttle and The Dig
    - it's far from perfect: upsampling is terrible, plays too slowly for some unknown
      reason, some tracks should overlap but don't, ...
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
- PSAD/iMUSE audio is still very imperfect (All titles up to inclunding The Dig)
- Rebel Assault I
  - all codecs used in the DOS Version of RA1 have been implemented, there
    are still visual glitches in codecs4/5.
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
- RA1 has more sound chunks ("Crea" indicates a VOC file dumped into the stream, RAW!/SBL1/SBL2 for I guess, raw PCM.
- codec4/5 tilegen is still buggy, there are miscolored edges on tiles (L11PLAY.ANM)

20250226
