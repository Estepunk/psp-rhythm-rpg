// Audio engine: everything is synthesized, so no sample files are needed yet.
#include <math.h>
#include <string.h>
#include "audio.h"

#define RATE   44100
#define BLOCK  512          // samples per audio buffer (~11.6 ms)
#define PI     3.14159265f
#define TABLE  1024         // sine lookup table size

Music music;

// ---- Musical setup -------------------------------------------------------

static const int minor_scale[7] = { 0, 2, 3, 5, 7, 8, 10 };
static const char *note_names[12] = {
    "A", "A#", "B", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#"
};

// Lowest note (scale degree 0) for each melodic track, in A.
static const float base_freq[NUM_TRACKS] = { 55.0f, 220.0f, 440.0f, 0.0f };
static const float track_gain[NUM_TRACKS] = { 0.50f, 0.20f, 0.17f, 0.45f };
static const float track_pan[NUM_TRACKS]  = { 0.50f, 0.35f, 0.65f, 0.50f }; // 0 L .. 1 R

// Starter patterns (original phrases). REST = silent step.
#define R REST
static const int presets[NUM_TRACKS][NUM_STEPS] = {
    // Bass: A ... A . E ... C ...
    { 0, R, R, R,  R, R, 0, R,  4, R, R, R,  2, R, R, R },
    // Lead: A' . . C' . . B' . . . . . E . . .
    { 7, R, R, 9,  R, R, 8, R,  R, R, R, R,  4, R, R, R },
    // Bells: rippling A minor arpeggio
    { 0, R, 2, R,  4, R, 2, R,  0, R, 2, R,  4, R, 6, R },
    // Drums: K . H . S . H . K K H . S . H H
    { DRUM_KICK, R, DRUM_HAT, R, DRUM_SNARE, R, DRUM_HAT, R,
      DRUM_KICK, DRUM_KICK, DRUM_HAT, R, DRUM_SNARE, R, DRUM_HAT, DRUM_HAT },
};
#undef R

const char *key_name(int root) { return note_names[((root % 12) + 12) % 12]; }

void note_label(int track, int value, char *out) {
    if (value == REST) { strcpy(out, "-"); return; }
    if (track == TRK_DRUMS) {
        strcpy(out, value == DRUM_KICK ? "K" : value == DRUM_SNARE ? "S" : "H");
        return;
    }
    int pc = (music.root + minor_scale[value % 7]) % 12;
    strcpy(out, note_names[pc]);
    if (value >= 7) strcat(out, "'");   // ' = upper octave
}

// Keep the key near A so repeated fifths don't climb out of range:
// A->E goes up a fifth, then later keys fold back down an octave.
static int root_offset(void) {
    int r = music.root % 12;
    return r <= 7 ? r : r - 12;
}

// ---- Synthesis helpers -----------------------------------------------------

static float sine_tab[TABLE];
static unsigned int noise_state = 22222;

static inline float tsin(float phase) {          // phase 0..1
    return sine_tab[(int)(phase * TABLE) & (TABLE - 1)];
}
static inline float noise(void) {
    noise_state = noise_state * 1664525u + 1013904223u;
    return (float)(int)noise_state * (1.0f / 2147483648.0f);
}
static inline float wrap(float p) { return p >= 1.0f ? p - 1.0f : p; }
static float decay_for(float seconds) {           // per-sample multiplier to -60 dB
    return expf(logf(0.001f) / (seconds * RATE));
}

typedef struct {
    float phase, phase2, inc, inc2;
    float env, attack, decay;
    float lp, pitch, last_noise;
    int attacking, kind;
} Voice;

static Voice voices[NUM_TRACKS];

static void trigger(int t, int value) {
    Voice *v = &voices[t];
    if (t == TRK_DRUMS) {
        v->kind = value;
        v->phase = 0.0f;
        v->attacking = 0;
        v->env = 1.0f;
        if (value == DRUM_KICK)       { v->pitch = 150.0f; v->decay = decay_for(0.35f); }
        else if (value == DRUM_SNARE) { v->decay = decay_for(0.16f); }
        else                          { v->env = 0.6f; v->decay = decay_for(0.05f); }
        return;
    }
    int semis = 12 * (value / 7) + minor_scale[value % 7] + root_offset();
    float freq = base_freq[t] * powf(2.0f, semis / 12.0f);
    v->inc = freq / RATE;
    v->attacking = 1;
    switch (t) {
    case TRK_BASS:  v->inc2 = v->inc;          v->attack = 1.0f / (0.004f * RATE); v->decay = decay_for(0.50f); break;
    case TRK_LEAD:  v->inc2 = v->inc * 1.005f; v->attack = 1.0f / (0.030f * RATE); v->decay = decay_for(1.00f); break;
    case TRK_BELLS: v->inc2 = v->inc * 2.0f;   v->attack = 1.0f / (0.002f * RATE); v->decay = decay_for(1.60f); break;
    }
}

static float render_voice(int t) {
    Voice *v = &voices[t];
    if (v->attacking) {
        v->env += v->attack;
        if (v->env >= 1.0f) { v->env = 1.0f; v->attacking = 0; }
    } else {
        if (v->env < 0.0001f) return 0.0f;
        v->env *= v->decay;
    }
    float out = 0.0f;
    switch (t) {
    case TRK_BASS: {                        // saw through a soft lowpass that closes as it fades
        float saw = 2.0f * v->phase - 1.0f;
        v->lp += (0.04f + 0.25f * v->env) * (saw - v->lp);
        out = v->lp;
        v->phase = wrap(v->phase + v->inc);
        break;
    }
    case TRK_LEAD: {                        // two slightly detuned triangles
        float a = 4.0f * fabsf(v->phase - 0.5f) - 1.0f;
        float b = 4.0f * fabsf(v->phase2 - 0.5f) - 1.0f;
        out = 0.5f * (a + b);
        v->phase = wrap(v->phase + v->inc);
        v->phase2 = wrap(v->phase2 + v->inc2);
        break;
    }
    case TRK_BELLS:                         // sine plus an octave that fades faster
        out = tsin(v->phase) + 0.35f * v->env * tsin(v->phase2);
        v->phase = wrap(v->phase + v->inc);
        v->phase2 = wrap(v->phase2 + v->inc2);
        break;
    case TRK_DRUMS:
        if (v->kind == DRUM_KICK) {         // sine with a falling pitch
            out = tsin(v->phase);
            v->phase = wrap(v->phase + v->pitch / RATE);
            v->pitch = 45.0f + (v->pitch - 45.0f) * 0.9992f;
        } else if (v->kind == DRUM_SNARE) { // noise plus a short tone
            out = 0.7f * noise() + 0.4f * tsin(v->phase);
            v->phase = wrap(v->phase + 190.0f / RATE);
        } else {                            // hat: high-passed noise
            float n = noise();
            out = 0.6f * (n - v->last_noise);
            v->last_noise = n;
        }
        break;
    }
    return out * v->env;
}

// ---- Drone (the ambient bed): A, E and A an octave up, slowly breathing ----

static float drone_phase[3], drone_inc[3], lfo_phase[3];
static const float drone_ratio[3] = { 1.0f, 1.4983f, 2.0f };   // root, fifth, octave
static const float lfo_rate[3] = { 0.07f, 0.11f, 0.05f };      // Hz

static void update_drone(void) {
    float root = 55.0f * powf(2.0f, root_offset() / 12.0f);
    for (int i = 0; i < 3; i++) drone_inc[i] = root * drone_ratio[i] / RATE;
}

// ---- Sequencer clock -------------------------------------------------------

static const double samples_per_step = RATE * 60.0 / BPM / 4.0;   // sixteenth notes
static double step_clock;

static void advance_step(void) {
    music.step = (music.step + 1) % NUM_STEPS;
    if (music.step == 0) {                         // bar line: apply queued changes
        music.bar++;
        for (int t = 0; t < NUM_TRACKS; t++) {
            Track *tr = &music.tracks[t];
            if (tr->state == ST_WAITING)  tr->state = ST_PLAYING;
            if (tr->state == ST_STOPPING) tr->state = ST_OFF;
        }
        if (music.pending_root >= 0) {
            music.root = music.pending_root;
            music.pending_root = -1;
            update_drone();
        }
    }
    for (int t = 0; t < NUM_TRACKS; t++) {
        Track *tr = &music.tracks[t];
        int value = tr->steps[music.step];
        if ((tr->state == ST_PLAYING || tr->state == ST_STOPPING) && value != REST) {
            trigger(t, value);
            tr->flash = 1;
        }
    }
}

// ---- Effects: ping-pong delay on lead and bells, master filter ---------------

#define DELAY_LEN 20889          // 3 sixteenths at 95 BPM (a dotted eighth)
static float delay_l[DELAY_LEN], delay_r[DELAY_LEN];
static int delay_pos;

static float cut_smooth = 0.75f, res_smooth = 0.2f;
static float low_l, band_l, low_r, band_r;

static inline float svf(float in, float f, float q, float *low, float *band) {
    for (int k = 0; k < 2; k++) {            // run twice per sample for stability
        *low += f * *band;
        float high = in - *low - q * *band;
        *band += f * high;
    }
    return *low;
}

static inline float soft_clip(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

void audio_render(short *out, int frames) {
    // Glide filter settings toward the targets once per block.
    cut_smooth += (music.cutoff - cut_smooth) * 0.15f;
    res_smooth += (music.resonance - res_smooth) * 0.15f;
    float fc = 120.0f * powf(8000.0f / 120.0f, cut_smooth);       // 120 Hz .. 8 kHz
    float f = 2.0f * sinf(PI * fc / (2.0f * RATE));
    float q = 1.4f - 1.25f * res_smooth;

    for (int i = 0; i < frames; i++) {
        step_clock += 1.0;
        if (step_clock >= samples_per_step) {
            step_clock -= samples_per_step;
            advance_step();
        }

        float l = 0.0f, r = 0.0f, send = 0.0f;
        for (int t = 0; t < NUM_TRACKS; t++) {
            float s = render_voice(t) * track_gain[t];
            l += s * (1.0f - track_pan[t]) * 1.4f;
            r += s * track_pan[t] * 1.4f;
            if (t == TRK_LEAD || t == TRK_BELLS) send += s;
        }

        float d = 0.0f;
        for (int k = 0; k < 3; k++) {
            d += tsin(drone_phase[k]) * (0.55f + 0.45f * tsin(lfo_phase[k]));
            drone_phase[k] = wrap(drone_phase[k] + drone_inc[k]);
            lfo_phase[k] = wrap(lfo_phase[k] + lfo_rate[k] / RATE);
        }
        l += d * 0.045f;
        r += d * 0.045f;

        float old_l = delay_l[delay_pos], old_r = delay_r[delay_pos];
        delay_l[delay_pos] = send + old_r * 0.45f;
        delay_r[delay_pos] = old_l * 0.45f;
        if (++delay_pos >= DELAY_LEN) delay_pos = 0;
        l += old_l * 0.5f;
        r += old_r * 0.5f;

        l = soft_clip(svf(l, f, q, &low_l, &band_l));
        r = soft_clip(svf(r, f, q, &low_r, &band_r));
        out[2 * i]     = (short)(l * 30000.0f);
        out[2 * i + 1] = (short)(r * 30000.0f);
    }
}

void audio_init(void) {
    memset(&music, 0, sizeof(music));
    for (int i = 0; i < TABLE; i++) sine_tab[i] = sinf(2.0f * PI * i / TABLE);
    for (int t = 0; t < NUM_TRACKS; t++)
        for (int s = 0; s < NUM_STEPS; s++)
            music.tracks[t].steps[s] = presets[t][s];
    music.step = NUM_STEPS - 1;          // the first sample starts bar 1
    music.pending_root = -1;
    music.cutoff = 0.75f;
    music.resonance = 0.2f;
    step_clock = samples_per_step;
    update_drone();
}

// ---- PSP audio thread ------------------------------------------------------

#ifndef AUDIO_HOST_TEST
#include <pspkernel.h>
#include <pspaudio.h>

static short buffers[2][BLOCK * 2] __attribute__((aligned(64)));

static int audio_thread(SceSize args, void *argp) {
    int ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, BLOCK, PSP_AUDIO_FORMAT_STEREO);
    if (ch < 0) return ch;
    int cur = 0;
    while (1) {
        audio_render(buffers[cur], BLOCK);
        sceAudioOutputBlocking(ch, PSP_AUDIO_VOLUME_MAX, buffers[cur]);
        cur ^= 1;
    }
    return 0;
}

int audio_start(void) {
    int thid = sceKernelCreateThread("audio_thread", audio_thread, 0x12, 0x10000,
                                     THREAD_ATTR_USER | THREAD_ATTR_VFPU, NULL);
    if (thid < 0) return thid;
    return sceKernelStartThread(thid, 0, NULL);
}
#endif
