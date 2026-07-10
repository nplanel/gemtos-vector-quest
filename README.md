```
____   ____             __                 ________                          __   
\   \ /   /____   _____/  |_  ___________  \_____  \  __ __   ____   _______/  |_ 
 \   Y   // __ \_/ ___\   __\/  _ \_  __ \  /  / \  \|  |  \_/ __ \ /  ___/\   __\
  \     /\  ___/\  \___|  | (  <_> )  | \/ /   \_/.  \  |  /\  ___/ \___ \  |  |  
   \___/  \___  >\___  >__|  \____/|__|    \_____\ \_/____/  \___  >____  > |__|  
              \/     \/                           \__>           \/     \/        
```

**A 3D vector-graphics space game for the Atari ST**

---

## About

Vector Quest is a real-time 3D vector-graphics spaceship racing game written in
C for the Atari ST (GemTOS). Pilot your shuttle head-to-head against another
player or the computer, dodge alien fighters, and cross the finish line first.

The game ships as a bootable floppy image with a custom loader, LZ4-compressed
game data, and YM2149 PSG music — fitting entirely within the constraints of a
stock 512 KB Atari ST.

A Linux/SDL2 port and an ASCII-terminal renderer are also included for
development and testing.

---

## Gameplay

Each round is one lap of the course between a shared start/finish line. You
race a peer over the serial link, or the computer opponent if none is
connected — both launch together, and whoever crosses the finish line first
wins the lap.

**Cruise** — throttle with Up/Down, steer left/right. Aliens spawn
continuously along the whole lap, get denser and the world gets faster every
round (rounds never reset). Shoot aliens with Fire. Alien contact or an enemy
missile doesn't end the lap: it stuns you for about 1.5 seconds and drops your
speed to the floor — lost time, not a lost game.

**Victory screen** — crossing the line shows the spinning 3D logo and credits
with a VICTORY or DEFEAT verdict. Press Fire to ready up; the next lap
launches once both players (or the bot) are ready, so laps always start
together.

---

## Controls

### Joystick (primary)

| Input | Action         |
|-------|----------------|
| Left  | Steer left     |
| Right | Steer right    |
| Up    | Throttle up    |
| Down  | Throttle down  |
| Fire  | Launch missile |

### Keyboard

| Key         | Action                  |
|-------------|-------------------------|
| Left arrow  | Steer left              |
| Right arrow | Steer right             |
| Up arrow    | Throttle up             |
| Down arrow  | Throttle down           |
| Space       | Launch missile          |
| F1          | Toggle 50 Hz / 60 Hz    |
| Q           | Quit                    |

---

## Building

Requires:
- `m68k-atari-mint-gcc` for Atari ST targets
- `gcc` + `libsdl2-dev` for the Linux/SDL2 port

```sh
make            # vquest.tos  — Atari TOS executable
make vquest.st  # bootable floppy image (with LZ4 loader)
make vq-sdl     # Linux/SDL2 build
make vq-ascii   # ASCII terminal renderer
```

### Race mode (2-player) testing

Run one player-A target and one player-B target in two terminals; the
instances are linked through named pipes in `/tmp`, so any pairing of
hatari (`race-hatari-a`/`-b`), SDL (`race-sdl-a`/`-b`) and the autopiloted
text backend — native (`race-ascii-a`/`-b`) or under hatari
(`race-ascii-tos-a`/`-b`) — works:

```sh
make race-sdl-a      # terminal 1
make race-hatari-b   # terminal 2
```

`make test-race` runs an automated regression of the serial link and
remote-player rendering on the text backend, natively (POSIX serial) and
under hatari (TOS serial).

---

## Credits

| Role   | Credit                                          |
|--------|-------------------------------------------------|
| Code   | Benou × Pump                                    |
| Sound  | Cyberic                                         |
| Thanks | Kalmalyzer, Leonard/Oxygene, Anthropic, FreeMint |

### Third-party components

| Component | Author | License | Purpose |
|-----------|--------|---------|---------|
| [segmented-line](https://github.com/Kalmalyzer/segmented-line) | Kalmalyzer | no license declared | Segmented line draw & clip routines |
| [libcmini](https://github.com/freemint/libcmini) | FreeMint project | LGPL-2.1 | Minimal C runtime for Atari TOS |
| [lz4-68k](https://github.com/arnaud-carre/lz4-68k) | Arnaud Carré | MIT | LZ4 block decompressor |

LZ4 technology by Yann Collet.
