Simple SDL2-based decoder/player for LucasArts Outlaws "SAN" movies.
(and Curse of Monkey Island, Shadows of the Empire, Mysteries of the Sith,
 and some of "Full Throttle" and most of "The Dig")

What works:
- Linux/Unix.
  - Windows untested: decoder builds, player does not due to unistd.h

- Can successfully parse all .SAN and .NUT files found on Outlaws CDs/Game dir
  - can also handle all SAN files from "Curse of Monkey Island", although some frames are missing text, which is most noticeable in the intro file.
  - "Shadows of the Empire" videos are now supported as well
  - "Mysteries of the Sith" videos are also supported (codec48).
  - Some/Most videos of "Full Throttle"/"The Dig" are also playable (codec37), but without sound (different audio than the above 3)
- Video/Audio decoding works for all codec47/codec48 videos
  - The Dig sq1.san crashes, sq14sc14.san has artifacts.
  - no audio for codec37 videos yet (Full Throttle/The Dig).
- good enough A/V sync in player
- tested on AMD64, ARM64, MIPS32el.
  - BE targets are untested, there are probably issues with the audio format and palette.

What does not yet work:
- pause / fullscreen

Build:
- Have SDL2
- run "make"

Use:
- call
 - sanplay /path/to/Outlaws/OP_CR.SAN
 - sanplay /path/to/SOTE/L00INTRO.SAN
 - sanplay /path/to/COMI/OPENING.SAN
 - sanplay /path/to/JKM/Resource/VIDEO/FINALE.SAN
 - sanplay /path/to/throttle/resource/video/introd_8.san
 - sanplay /path/to/dig/dig/video/pigout.san

20241210
