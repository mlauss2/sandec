Simple SDL2-based decoder/player for LucasArts Outlaws "SAN" movies.

As of 20241007, the following works
- Can successfully parse all .SAN files found on Outlaws CDs/Game dir
- Audio works
- Video display works, still issues with Codec1 and delta palettes
  (esp. visible in the sunset->night transition in the Outro RAE.SAN)
- A/V sync is crude at best.
