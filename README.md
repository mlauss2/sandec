Single-file A/V decoder library for LucasArts SAN/SMUSH movie files (originally
intended only to play "Outlaws" videos, since they are awesome),
with an SDL3-based player application to demonstrate library use.

Can also play game movies from Curse of Monkey Island, Shadows of the Empire,
Mysteries of the Sith, The Dig, Full Throttle, Rebel Assault II, Rebel Assault,
X-Wing Alliance.

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
  - Rebel Assault I (DOS, 3DO and SEGA-CD versions)
  - Star Wars: Making Magic
  - Mortimer and the Riddles of the Medallion
  - Droidworks
- Can successfully parse .SNM videos from the following LucasArts titles:
  - X-Wing Alliance
  - Indiana Jones and the Infernal Machine
  - Grim Fandango
  - Force Commander
    - you need to unzip the .znm files first to play them.
- Video decoding
  - Handles all 8-bit codecs found in LucasArts DOS/Windows titles
  - BL16 video in .snm/.znm files.
  - frame interpolation for codec47/48 videos
    - default off as it generates a lot of ghosting, but some scenes, esp. zoom-ins and pans, look noticeably smoother.
- Audio decoding
  - audio ouput for all by the decoder is 22kHz/16bit/2ch.
    - all sources with lower quality are very crudely upsampled.
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
- Star Wars: Making Magic
  - lots of artifacts in the codec48 videos.
- Audio
  - RA1/RA2 background music does not loop.
  - RA1 player speech is both genders simultaneously: the game engine filtered them using the SKIP chunks, which are not implemented (yet).

# Build:
- Have SDL3
- run "make"

# Use:
- sanplay [-f] [-v] [-s] [-[0..3]] <file1.san/anm> [file2.san] [file3.san]...
  - -f: start fullscreen
  - -v: be verbose
  - -s: no audio (silent)
  - -0..3: speedmode  0: normal  1: ignore frametimes (display as fast as possible)  2: just decode as fast as possible  3: pause after every frame

20250814