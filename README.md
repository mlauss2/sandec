Simple SDL2-based decoder/player for LucasArts Outlaws "SAN" movies.
(and Curse of Monkey Island, Shadows of the Empire, and Mysteries of the Sith)

What works:
- Linux/Unix.
  - Windows untested: decoder builds, player does not due to unistd.h

- Can successfully parse all .SAN and .NUT files found on Outlaws CDs/Game dir
  - can also handle all SAN files from "Curse of Monkey Island", although
    some frames are missing text, which is most noticeable in the intro file.
  - "Shadows of the Empire" videos are now supported as well
  - "Mysteries of the Sith" videos are also supported (codec48).
- Video/Audio decoding works without any known artifacts.
- good enough A/V sync in player
- tested on AMD64, ARM64, MIPS32el.
  - BE targets are untested, there are probably issues with the
    audio format and palette.

What does not yet work:
- pause / fullscreen

Build:
- Have SDL2
- run "make"

Use:
- call
 * sanplay /path/to/Outlaws/OP_CR.SAN
 * sanplay /path/to/SOTE/L00INTRO.SAN
 * sanplay /path/to/COMI/OPENING.SAN
 * sanplay /path/to/JKM/Resource/VIDEO/FINALE.SAN

20241127
