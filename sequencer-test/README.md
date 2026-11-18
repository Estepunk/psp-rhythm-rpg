# Sequencer test

Ambient jam prototype in A minor at 95 BPM. All sounds are synthesized.

Walk between four colored zones (Bass, Lead, Bells, Drums). Each zone has a
16-step loop. Starting or stopping a loop waits for the next bar line, so
everything stays in time. A drone plays underneath the whole time.

Every 30 loop launches, the key moves up a fifth (A minor -> E minor -> ...),
at the next bar line.

## Controls

Map
- D-pad: move
- X: start/stop the zone you're standing in
- Triangle: edit that zone's loop
- START: stop everything
- Hold R + analog nub: filter (left/right = cutoff, up/down = resonance)

Loop editor
- Left/Right: pick a step
- Up/Down: change the note (turns an empty step on)
- X: step on/off
- Square: clear the loop
- SELECT: start/stop this loop
- Circle: back to the map

## Things to tweak

- `LAUNCHES_PER_KEY_CHANGE` in `main.c`: launches before each key change
- `BPM` in `audio.h`
- `presets` in `audio.c`: the starting loop for each zone
- `track_gain` in `audio.c`: instrument volumes

## Build

    make
