Simple SDL2-based decoder/player for LucasArts Outlaws "SAN" movies.

What works:
- Can successfully parse all .SAN files found on Outlaws CDs/Game dir
- Video/Audio decoding works without any known artifacts.
- reasonable A/V sync
- tested on AMD64, ARM64, MIPS32el.

What does not yet work:
- pause / fullscreen

Build:
- Have SDL2
- run "make"
- call "sanplay /path/to/Outlaws/OP_CR.SAN"

20241027
