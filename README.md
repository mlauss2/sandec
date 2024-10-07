Simple SDL2-based decoder/player for LucasArts Outlaws "SAN" movies.

What works:
- Can successfully parse all .SAN files found on Outlaws CDs/Game dir
- Audio works
- Video display works

What does not work:
- A/V sync in player is crude at best.

Build:
- Have SDL2
- run "make"
- call "sanplay /path/to/Outlaws/OP_CR.SAN"
