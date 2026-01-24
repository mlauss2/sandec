Single-file A/V decoder library for LucasArts SAN/SMUSH movie files,
with an SDL3-based player application to demonstrate library use.

# What works:
- Linux/Unix.
  - Windows untested, but should build.
- Can successfully parse and decode all .SAN, .SNM and .NUT files from the following LucasArts games:
  - Outlaws
  - Curse of Monkey Island
    - Text rendering is not implemented, which is most noticeable in the intro file.
  - Shadows of the Empire
  - Mysteries of the Sith
  - Full Throttle
  - The Dig
  - Rebel Assault II
  - Rebel Assault I (DOS, 3DO and SEGA-CD versions)
  - Star Wars: Making Magic
  - Mortimer and the Riddles of the Medallion
  - Droidworks
  - X-Wing Alliance
  - Indiana Jones and the Infernal Machine
  - Grim Fandango
  - Force Commander (need Zlib or unpack the .znm files)
- player keyboard controls:
  - space to pause/unpause
  - f     to toggle fullscreen/windowed
  - n     to play next video in list
  - .     to advance by 1 frame and pause
  - q     to quit
  - l     to toggle ANIMv1 (RA1) viewport size: Most videos have 320x200 content, but some gameplay videos have content on the full 384x242 buffer.
  - number keys 1-6 to display the original/double/triple/... width preserving aspect ratio
  - i  to toggle frame interpolation on codec47/48 on/off
  - s  to cycle between SDL3 texture smoothing (off/pixelized and smoothed).
- tested on AMD64, ARM64, MIPS32el.
  - BE targets are untested, there are probably issues with the audio format and palette.

# What does not work:
- Mortimer and the Riddles of the Medallion
  - Most videos play, but issues with transparency and objects with only half width/height.

# Build:
- Have SDL3
- Zlib (optional)
- run "make" or "make zlib" (to support the .znm files from Force Commander without unpacking)

# Use:
- sanplay [-f] [-v] [-s] [-[0..3]] <file1.san/anm> [file2.san] [file3.san]...
  - -f: start fullscreen
  - -v: be verbose
  - -s: no audio (silent)
  - -0..3: speedmode  0: normal  1: ignore frametimes (display as fast as possible)  2: just decode as fast as possible  3: pause after every frame

# Notes
- The following Tags won't be implemented:
  - TEXT: maybe later; this requires a lot of additional infrastructure to fully support, esp. font loading/parsing/fontstore handling (since some texts have commands to switch to another font, ...) which doesn't belong here.
  - LOAD: preload bits of another ANM/SAN, used in RA1/2 for branchpoints (i.e. fly left for easy, stay right for hard).
  - VMSK: masked blit; it's in the RA2 code, stubbed out in later engines, but never encountered in any SAN file.
  - GAME/GAM2: RA1 game progress feedback.
  - RAW!/SBL /SBL2/Crea: Raw PCM/VOC file support, it's in the (RA1) code but not used in any ANM/SAN file.
- FADE (RA1) is still a bit bugged.

20260124
