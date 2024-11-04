Simple SDL2-based decoder/player for LucasArts Outlaws "SAN" movies.
(and Curse of Monkey Island now too)

What works:
- Linux/Unix.
  - Windows untested: decoder builds, player does not due to unistd.h

- Can successfully parse all .SAN and .NUT files found on Outlaws CDs/Game dir
  - can also handle all SAN files from "Curse of Monkey Island", although
    some frames are missing text, which is most noticeable in the intro file.
    Also, COMI lies about audio samplerate: all SAN files advertise
    11.025kHz, but it is actually 22.05kHz, just like all of Outlaws' files.
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
- call "sanplay /path/to/Outlaws/OP_CR.SAN"

20241104
