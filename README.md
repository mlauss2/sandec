Simple SDL2-based decoder/player for LucasArts Outlaws "SAN" movies.

What works:
- Can successfully parse all .SAN files found on Outlaws CDs/Game dir
- Audio works
- Video display works
- reasonable A/V sync

What does not yet work:
- pause / fullscreen

Build:
- Have SDL2
- run "make"
- call "sanplay /path/to/Outlaws/OP_CR.SAN"
