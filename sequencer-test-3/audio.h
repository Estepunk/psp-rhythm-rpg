// Audio engine for the sequencer test: synth voices, step sequencer, realm
// ambiances, echo/reverb and a master filter. Runs on its own thread.
#ifndef AUDIO_H
#define AUDIO_H

#define NUM_TRACKS 4
#define MAX_STEPS  48       // the longest loop (drums: 3 lanes x 16)
#define BPM        95

enum { TRK_BASS, TRK_LEAD, TRK_BELLS, TRK_DRUMS };

// Track states. Starting and stopping wait for the next bar line
// (the lead waits for the start of its two-bar phrase).
enum { ST_OFF, ST_WAITING, ST_PLAYING, ST_STOPPING };

// Drums have three lanes of 16 steps, stored one after another.
enum { LANE_KICK, LANE_HATS, LANE_PERC, NUM_LANES };
enum { HAT_CLOSED, HAT_OPEN };
enum { PERC_SNARE, PERC_CLAP, PERC_RIM, PERC_SHAKER, NUM_PERC };

#define REST       (-1)
#define MAX_DEGREE 13        // two octaves of the minor scale (0..13)
#define SLIDE_FLAG 64        // bass: glide into this note (when Slide is on)
#define NOTE(v)    ((v) & 63)

enum { WAVE_SOFT, WAVE_SAW, WAVE_SQUARE, WAVE_PULSE, WAVE_BELL, NUM_WAVES };
enum { REALM_PULSE, REALM_DRONE, REALM_TEXTURE, NUM_REALMS };
enum { SPACE_ECHO, SPACE_REVERB };
enum { FILT_LOW, FILT_HIGH, FILT_BAND, FILT_NOTCH, NUM_FILTERS };

typedef struct {
    volatile int steps[MAX_STEPS];   // REST, a scale degree, or a drum sound
    volatile int next_steps[MAX_STEPS];
    volatile int swap;               // 1 = use next_steps from the next bar line
    volatile int state;
    volatile int wave;               // WAVE_* (melodic tracks)
    volatile int flash;              // audio thread sets 1 when a note plays
} Track;

typedef struct {
    Track tracks[NUM_TRACKS];
    volatile int tick;               // 0..31, thirty-second notes in the bar
    volatile int bar;                // counts up from 1
    volatile int root;               // key root in semitones above A (0 = A minor)
    volatile int pending_root;       // -1, or a root to switch to at the next bar
    volatile int realm;              // which ambiance plays
    volatile int filter_type;        // FILT_*
    volatile int space;              // SPACE_ECHO or SPACE_REVERB
    volatile float cutoff;           // filter cutoff, 0..1
    volatile float resonance;        // filter resonance, 0..1
    volatile float space_mix;        // echo/reverb level, 0..1
    volatile float space_amt;        // echo feedback / reverb size, 0..1
    volatile int bass_slide, bass_sub, bass_pump;   // bass powers, on/off
} Music;

extern Music music;

void audio_init(void);
int  audio_start(void);                        // start the audio thread (PSP only)
void audio_render(short *out, int frames);     // fill interleaved stereo samples

int  track_length(int t);                      // 16, 32 or 48 steps
int  track_position(int t);                    // step under the playhead

const char *key_name(int root);                // "A", "E", "F#" ...
const char *wave_name(int wave);               // "Soft", "Saw" ...
void note_label(int value, char *out);         // "C", "A'" ... for melodic steps

#endif
