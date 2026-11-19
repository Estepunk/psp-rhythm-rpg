# Sequencer test 3 (free jam)

Ambient jam prototype in A minor at 95 BPM. All sounds are synthesized.

First pick a realm. Each realm is its ambiance:
- PULSE: a steady high heartbeat on every eighth note
- DRONE: root, fifth and octave, slowly breathing
- TEXTURE: wind, vinyl crackle and drifting high chord tones

Then walk between four zones. Each zone loops on the next bar line, and its
loop is randomly generated when you enter the realm.

## The instruments

- BASS: one bar. Pads 1-3 are loop banks. Under them are its powers:
  - SLD (Slide): some notes glide into the next (which ones is random)
  - SUB (Sub): a low octave under beats 1 and 3 (headphones; the PSP
    speakers can't play that low)
  - PMP (Pump): everything else ducks each time the bass hits
- LEAD: a two-bar phrase; bar 2 answers bar 1. Starts and stops wait for
  the start of the phrase. Pads 1-3 are loop banks.
- BELLS: double time (32 steps in one bar) for fast arpeggios.
  Pads 1-3 are loop banks.
- DRUMS: three lanes (kick, hats, percussion). Pads pick a style:
  `*` four on the floor, `+` hip hop, `#` dembow.

## Controls

Map
- Analog stick (or d-pad): move
- X: start/stop the zone you're in
- Square: new random loop
- Triangle: edit the loop
- On a bank pad: X loads it (or saves, if it's empty), Square saves over it
- On a drum style pad: X makes a new beat in that style
- On a bass power pad: X turns it on/off
- M pad in the center: X opens the settings menu
- SELECT: new loops everywhere
- START: stop everything
- Hold R + stick: filter (left/right = cutoff, up/down = resonance)
- Hold L + stick: echo (level/feedback) or reverb (level/size)

Settings menu
- Space effect: echo or reverb
- Filter type: low-pass, high-pass, band-pass or notch
- Auto key change: off, or up a fifth every 30 launches
- Move the key up a fifth now, or reset it to A minor
- Leave the realm (back to realm select)

Loop editor
- Left/Right: pick a step
- Up/Down: change the note (drums: pick a lane)
- X: step on/off
- Triangle: change the sound (drums: change the hat or percussion sound)
- Square: new random loop
- START: clear the loop
- SELECT: start/stop this loop
- Circle: back to the map

## Things to tweak

- `odds` tables in `generate.c`: how likely notes are on each beat
- `gen_drums` in `generate.c`: the three drum styles
- `BPM` in `audio.h`
- `AUTO_KEY_LAUNCHES` in `main.c`
- `track_gain` and `lane_gain` in `audio.c`: volumes

## Build

    make
