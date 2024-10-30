Simple SDL2-based decoder/player for LucasArts Outlaws "SAN" movies.

What works:
- Linux/Unix.
  - Windows untested: decoder builds, player does not due to unistd.h

- Can successfully parse all .SAN and .NUT files found on Outlaws CDs/Game dir
- Video/Audio decoding works without any known artifacts.
- perfect A/V sync in player
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

20241030
