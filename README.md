# Super Mario Kart - recompilation

A from-scratch C++ reimplementation of Super Mario Kart's driving code and
mode-7 rendering, written against the original 65816 routines.

**WIP.** Tracks load and render, the kart drives, but there are no karts on
screen, no HUD beyond a debug overlay, and no AI yet.

## You must supply your own ROM

**No game assets are stored in this repository or in the executables.**
Nintendo owns the graphics, music and data, so the game reads everything -
tiles, palettes, tilemaps, character stats - from your own copy of *Super Mario
Kart (USA)* at runtime. There is no bundled or fallback copy: run it somewhere
without a ROM and it stops and asks you for one.

`smk-game.exe` looks for the image in this order:

1. the path given on the command line
2. `Super Mario Kart (USA).sfc` next to the executable
3. any `*.sfc` in the current directory
4. a file-open dialog

If none is found it says so and exits. Please dump your own cartridge; the
assets stay on your machine and nothing is copied out of it.

### The reference PNGs

`SNES - Super Mario Kart - *.png` in the project root are the artwork the game
now draws: the track renders sample them as a top-down ground texture, and the
kart is cropped from the racer sheet. `src/png_read.h` decodes them (with its
own DEFLATE, since zlib is not linkable in this toolchain).

The ROM is still required. The PNGs carry no tile indices, no palettes, no
course list and no physics, so all of that still comes from your ROM and the
game refuses to start without it. Two race courses have no track PNG and fall
back to the ROM's own tiles; the same happens for the kart if a racer sheet is
missing. Both fallbacks are reported at startup.

Files are looked for next to the executable (and one or two directories up),
by name: `SNES - Super Mario Kart - Tracks - <course>.png` and
`SNES - Super Mario Kart - Racers - <driver>.png`.

Note the racer set names Daisy's slot "Donkey Kong Jr." -- there is no Daisy
PNG, so driver 3 uses that one.

## Build

```
make tools     # smk-disasm, smk-extract, smk-game
make game      # just the game
make shot      # render one headless frame to build/shot.png
make assets    # re-extract the asset viewer into assets/
```

Requires a C++17 `g++` (clang on Windows works) and nothing else.

## Run

```
bin\smk-game.exe "Super Mario Kart (USA).sfc"            # windowed
bin\smk-game.exe --course 7 --frames 240 --shot s.png   # headless frame
bin\smk-game.exe --course 3 --mapshot bc.png            # whole course, top down
bin\smk-game.exe --stats                                 # audit every course
bin\smk-game.exe --selftest                              # physics telemetry
```

| flag | |
|---|---|
| `--course N` | course id 0..23 in the ROM's level-data order |
| `--char N` | driver 0..7 |
| `--cc N` | engine class 50/100/150/200 |
| `--start X,Y` | starting position on the 1024 x 1024 world |
| `--frames N` | simulate N frames instead of opening a window |
| `--shot FILE` | write a 256x224 PNG of the mode-7 view |
| `--mapshot FILE` | write a 256x256 top-down PNG of the whole track |
| `--stats` | print tile/palette/tilemap audit for all 24 courses |
| `--selftest` | print speed, heading and position while accelerating |

Arrow keys or WASD drive. Escape quits.

## What is implemented

* `src/smk/lz.h` - port of the game's decompressor `CODE_84E09E`
* `src/smk/course.h` - `CODE_81E6D4` / `CODE_81E745` / `CODE_81E6B9` /
  `CODE_81E72E` / `CODE_84E378`: tilemap, track tiles, 64 shared tiles and
  palette for any of the 8 graphics families, with the course-to-family table
  read from the ROM (`DATA_81EC2F`)
* `src/smk/kart.h` - `CODE_80A3B7` and friends: speed, cc-limited top speed,
  acceleration curve, turning, coast drag, off-world wrap at 1024 px, and the
  per-driver stat table at `0x18000`
* `tools/game.cpp` - software mode-7 renderer and Win32 shell
* `src/smk/sprite.h` - driver sprite sheets (`<bank>:2000`, 768 4bpp tiles),
  decoded and drawn as a 4x2 frame with the colours the game takes from the
  course palette stream

## What is not

* kart animation frames, per-driver colours and distance scaling (the kart is
  one static frame at a fixed size)
* other racers, AI, item logic, sound
* battle courses (ids 20..23) - the ROM's track-type table only lists the 20
  race courses
* the spinning/jumping state machine (`CODE_80B7EB`, `CODE_80B1BE`)
* road-edge collision and off-road slowdown from the level data

## Documentation

* `docs/mode7.md` - track graphics pipeline, tables and the decompressor
* `docs/sprites.md` - driver sprite sheets, the palette discovery, debug flags
* `docs/assets.md` - the extracted-asset catalogue and viewer
* `assets/index.html` - browse the extracted assets
