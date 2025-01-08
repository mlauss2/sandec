Single-file A/V decoder library for LucasArts Outlaws game "SAN"/SMUSH movie files,
with a simple SDL2-based player application to demonstrate library use.

Can also play game movies from Curse of Monkey Island, Shadows of the Empire,
Mysteries of the Sith, The Dig and some of Full Throttle.
Will expand with new features as I see fit.

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
- Audio decoding works for all SMUSH codec47/codec48 videos
  - the subchunk-less 22kHz/16bit/stereo IACT variant in use since COMI.
- good enough A/V sync in player
- player keyboard controls:
  - space  pause/unpause
  - q  to quit
  - number keys 1-6 to display the original/double/triple/... width preserving aspect ratio
- tested on AMD64, ARM64, MIPS32el.
  - BE targets are untested, there are probably issues with the audio format and palette.

# What does **not** yet work:
- fullscreen toggle
- Audio in The Dig, Full Throttle
  - IACT with iMUSE subchunks
  - PSAD/SAUD with multiple streams requiring software mixing.

# Build:
- Have SDL2
- run "make"

# Use:
- invoke with SAN file name:
  - sanplay /path/to/Outlaws/OP_CR.SAN
  - sanplay /path/to/SOTE/L00INTRO.SAN
  - sanplay /path/to/COMI/OPENING.SAN
  - sanplay /path/to/JKM/Resource/VIDEO/FINALE.SAN
  - sanplay /path/to/throttle/resource/video/introd_8.san
  - sanplay /path/to/dig/dig/video/pigout.san

20250108
