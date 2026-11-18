// Audio engine for the sequencer test: synth voices, step sequencer, drone,
// delay and a master filter. Runs on its own thread.
#ifndef AUDIO_H
#define AUDIO_H

#define NUM_TRACKS 4
#define NUM_STEPS  16
#define BPM        95

enum { TRK_BASS, TRK_LEAD, TRK_BELLS, TRK_DRUMS };

// Track states. Starting and stopping wait for the next bar line.
enum { ST_OFF, ST_WAITING, ST_PLAYING, ST_STOPPING };

// Drum step values
enum { DRUM_KICK, DRUM_SNARE, DRUM_HAT };

#define REST (-1)
#define MAX_DEGREE 13   // two octaves of the minor scale (0..13)

typedef struct {
    volatile int steps[NUM_STEPS];  // REST, a scale degree, or a drum type
    volatile int state;
    volatile int flash;             // audio thread sets 1 when a note plays
} Track;

typedef struct {
    Track tracks[NUM_TRACKS];
    volatile int step;          // 0..15, current sixteenth note
    volatile int bar;           // counts up from 1
    volatile int root;          // key root in semitones above A (0 = A minor)
    volatile int pending_root;  // -1, or a root to switch to at the next bar
    volatile float cutoff;      // filter cutoff, 0..1 (dark..bright)
    volatile float resonance;   // filter resonance, 0..1
} Music;

extern Music music;

void audio_init(void);                 // set up patterns and tables
int  audio_start(void);                // start the audio thread (PSP only)
void audio_render(short *out, int frames);  // fill interleaved stereo samples

const char *key_name(int root);        // "A", "E", "F#" ...
void note_label(int track, int value, char *out);  // label for a step value

#endif
