Single-file A/V decoder library for LucasArts SAN/SMUSH movie files (originally
intended only to play "Outlaws" videos, since they are awesome),
with a simple SDL2-based player application to demonstrate library use.

Can also play game movies from Curse of Monkey Island, Shadows of the Empire,
Mysteries of the Sith, The Dig, Full Throttle.  Some of Rebel Assault 1 and most
of Rebel Assault II.

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
- Video decoding works for all SMUSH codec1/codec37/codec47/codec48 videos
  - frame interpolation for codec47/48 videos (default on).
- Audio decoding
  - OK for COMI/Outlaws/MotS/SotE
  - alright for Full Throttle and The Dig
    - it's far from perfect: lots of hiccups, upsampling is terrible, plays too slowly for some unknown reason.
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
- Rebel Assault I/II
  - all codecs used in RA1/RA2 are implemented, but the results look terrible. More reversing work is underway. 

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
- STOR/FTCH is implemented wrongly for RA1:  On STOR, the game EXE just caches the complete next FOBJ chunk in a temporary buffer, renders it,
  and an FTCH, replays it (i.e. decodes the image again; STOR does NOT store the current front buffer, like it is implemented now). The current
  scheme works more or less by accident with FT/Dig/+ because they really only render one FOBJ per frame.
  I.e. see L8PLAY.ANM where the walker is only visible if FTCH is diabled.
- RA1 has more sound chunks ("Crea" indicates a VOC file dumped into the stream, RAW!/SBL1/SBL2 for I guess, raw PCM.
- codec4/5 tilegen is still buggy, there are miscolored edges on tiles (L11PLAY.ANM)

20250221

