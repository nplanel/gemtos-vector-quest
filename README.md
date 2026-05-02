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

Vector Quest is a real-time 3D vector-graphics spaceship game written in C for
the Atari ST (GemTOS). Pilot your shuttle through space, dodge alien fighters,
and land safely on the strip before fuel runs out.

The game ships as a bootable floppy image with a custom loader, LZ4-compressed
game data, and YM2149 PSG music — fitting entirely within the constraints of a
stock 512 KB Atari ST.

A Linux/SDL2 port and an ASCII-terminal renderer are also included for
development and testing.

---

## Gameplay

The goal is to complete as many rounds as possible. Each successful landing advances to the next round at higher speed; crashing resets to round 1.

Each round is three phases:

**Takeoff** — you start on the ground. Hold Up to climb against gravity. You have roughly 2.4 seconds to reach cruise altitude. Fail to climb in time, or drift too far sideways off the strip, and you crash. Down does nothing here.

**Cruise** — once at altitude the vertical axis locks and you fly forward at a fixed height with faster lateral steering. A landing strip appears somewhere ahead, randomly offset to the left or right. Aliens spawn along the approach corridor (4 on round 1, +1 per round, up to 8 max) and scroll toward you with the world. Shoot them with Fire; any alien that reaches your position will kill you. Cruise ends automatically once the strip is close enough.

**Landing** — vertical control returns. Gravity pulls you down continuously; Up brakes the descent (slows but does not stop the fall), Down accelerates it. You must touch down meeting two conditions at once: descent speed below the crash threshold, and lateral position within one grid unit of the strip centre (the strip is offset, not centred — watch the arrows). Aliens are still active and still lethal during approach.

A successful landing shows the spinning 3D logo and credits — press any key to launch the next round. Speed increases ~12 % per round, capped at 3× the starting speed.

---

## Controls

### Joystick (primary)

| Input | Action         |
|-------|----------------|
| Left  | Bank left      |
| Right | Bank right     |
| Up    | Thrust up      |
| Down  | Descend        |
| Fire  | Launch missile |

### Keyboard

| Key         | Action                  |
|-------------|-------------------------|
| Left arrow  | Bank left               |
| Right arrow | Bank right              |
| Up arrow    | Thrust up               |
| Down arrow  | Descend                 |
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
