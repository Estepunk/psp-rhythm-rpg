// Audio engine: everything is synthesized, so no sample files are needed.
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

// Lowest note (scale degree 0) of each melodic instrument, in A.
static const float base_freq[3] = { 55.0f, 220.0f, 440.0f };
static const float track_gain[3] = { 0.55f, 0.28f, 0.22f };
static const float track_pan[3]  = { 0.50f, 0.35f, 0.65f };   // 0 left .. 1 right
static const int default_wave[3] = { WAVE_SAW, WAVE_SOFT, WAVE_BELL };

static const float lane_gain[NUM_LANES] = { 0.55f, 0.30f, 0.42f };
static const float lane_pan[NUM_LANES]  = { 0.50f, 0.58f, 0.45f };

static const char *wave_names[NUM_WAVES] = { "Soft", "Saw", "Square", "Pulse", "Bell" };
static const float wave_gain[NUM_WAVES] = { 1.0f, 0.90f, 0.45f, 0.45f, 0.62f };

const char *wave_name(int wave) { return wave_names[wave % NUM_WAVES]; }
const char *key_name(int root) { return note_names[((root % 12) + 12) % 12]; }

void note_label(int value, char *out) {
    if (value == REST) { strcpy(out, "-"); return; }
    int d = NOTE(value);
    strcpy(out, note_names[(music.root + minor_scale[d % 7]) % 12]);
    if (d >= 7) strcat(out, "'");   // ' = upper octave
}

int track_length(int t) {
    return t == TRK_DRUMS ? 48 : (t == TRK_LEAD || t == TRK_BELLS) ? 32 : 16;
}

int track_position(int t) {
    switch (t) {
    case TRK_LEAD:  return ((music.bar - 1) & 1) * 16 + music.tick / 2;   // two-bar phrase
    case TRK_BELLS: return music.tick;                                     // double time
    default:        return music.tick / 2;
    }
}

// Keep the key near A so repeated fifths don't climb out of range.
static int root_offset(void) {
    int r = music.root % 12;
    return r <= 7 ? r : r - 12;
}
static float root_freq(void) { return 55.0f * powf(2.0f, root_offset() / 12.0f); }

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
static inline float rand01(void) { return 0.5f + 0.5f * noise(); }
static inline float wrap(float p) { return p >= 1.0f ? p - 1.0f : p; }
static float decay_for(float seconds) {           // per-sample multiplier to -60 dB
    return expf(logf(0.001f) / (seconds * RATE));
}

// Smooths the jump in saw/square/pulse waves so high notes don't buzz.
static inline float blep(float t, float dt) {
    if (t < dt)        { t /= dt;             return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

static inline float osc(int wave, float p, float p2, float dt, float dt2, float env) {
    switch (wave) {
    case WAVE_SAW: {
        float a = 2.0f * p - 1.0f - blep(p, dt);
        float b = 2.0f * p2 - 1.0f - blep(p2, dt2);
        return 0.5f * (a + b);
    }
    case WAVE_SQUARE: {
        float q = p < 0.5f ? 1.0f : -1.0f;
        return q + blep(p, dt) - blep(wrap(p + 0.5f), dt);
    }
    case WAVE_PULSE: {
        float q = p < 0.25f ? 1.0f : -1.0f;
        return q + blep(p, dt) - blep(wrap(p + 0.75f), dt);
    }
    case WAVE_BELL:
        return tsin(p) + 0.35f * env * tsin(p2);
    default: {
        float a = 4.0f * fabsf(p - 0.5f) - 1.0f;
        float b = 4.0f * fabsf(p2 - 0.5f) - 1.0f;
        return 0.5f * (a + b);
    }
    }
}

typedef struct {
    float phase, phase2, inc, target_inc, ratio2;
    float env, attack, decay, gain;
    float lp, pitch, last_noise;
    float sub_phase, sub_env, sub_decay;
    int attacking, kind, age;
} Voice;

// Voices 0..2 are the melodic instruments; drums get one voice per lane.
enum { V_KICK = 3, V_HAT, V_PERC, NUM_VOICES };
static Voice voices[NUM_VOICES];

static float duck_target, duck_level;           // bass "Pump" effect

static void trigger_melodic(int t, int value, int step) {
    Voice *v = &voices[t];
    int d = NOTE(value);
    int semis = 12 * (d / 7) + minor_scale[d % 7] + root_offset();
    float inc = base_freq[t] * powf(2.0f, semis / 12.0f) / RATE;
    v->kind = music.tracks[t].wave % NUM_WAVES;
    v->ratio2 = v->kind == WAVE_BELL ? 2.0f : 1.005f;
    int slide = t == TRK_BASS && music.bass_slide && (value & SLIDE_FLAG) && v->env > 0.05f;
    v->target_inc = inc;
    if (!slide) v->inc = inc;                    // slide: glide from the old pitch
    v->attacking = 1;
    switch (t) {
    case TRK_BASS:  v->attack = 1.0f / (0.004f * RATE); v->decay = decay_for(slide ? 0.8f : 0.5f); break;
    case TRK_LEAD:  v->attack = 1.0f / (0.030f * RATE); v->decay = decay_for(1.00f); break;
    case TRK_BELLS: v->attack = 1.0f / (0.002f * RATE); v->decay = decay_for(1.20f); break;
    }
    if (t == TRK_BASS) {
        if (music.bass_sub && (step == 0 || step == 8)) {   // Sub: low octave on beats 1 and 3
            v->sub_env = 1.0f;
            v->sub_decay = decay_for(0.9f);
        }
        if (music.bass_pump) duck_target = 1.0f;            // Pump: everything else ducks
    }
}

static float render_melodic(int t) {
    Voice *v = &voices[t];
    if (v->attacking) {
        v->env += v->attack;
        if (v->env >= 1.0f) { v->env = 1.0f; v->attacking = 0; }
    } else {
        if (v->env < 0.0001f && v->sub_env < 0.0001f) return 0.0f;
        v->env = v->env < 0.0001f ? 0.0f : v->env * v->decay;
    }
    v->inc += (v->target_inc - v->inc) * 0.0008f;           // ~30 ms glide
    float inc2 = v->inc * v->ratio2;
    float out = osc(v->kind, v->phase, v->phase2, v->inc, inc2, v->env) * wave_gain[v->kind];
    v->phase = wrap(v->phase + v->inc);
    v->phase2 = wrap(v->phase2 + inc2);
    out *= v->env;
    if (t == TRK_BASS) {
        v->lp += (0.04f + 0.25f * v->env) * (out - v->lp);  // closes as the note fades
        out = v->lp * 1.3f;
        if (v->sub_env > 0.0001f) {
            out += 0.9f * v->sub_env * tsin(v->sub_phase);
            v->sub_phase = wrap(v->sub_phase + v->inc * 0.5f);
            v->sub_env *= v->sub_decay;
        }
    }
    return out;
}

static void trigger_drum(int lane, int value) {
    Voice *v = &voices[V_KICK + lane];
    v->kind = value;
    v->phase = v->phase2 = 0.0f;
    v->attacking = 0;
    v->age = 0;
    v->env = 1.0f;
    v->gain = 1.0f;
    if (lane == LANE_KICK) { v->pitch = 150.0f; v->decay = decay_for(0.35f); return; }
    if (lane == LANE_HATS) {
        v->gain = 0.6f;
        v->decay = decay_for(value == HAT_OPEN ? 0.40f : 0.05f);
        return;
    }
    switch (value) {
    case PERC_SNARE:  v->decay = decay_for(0.16f); break;
    case PERC_CLAP:   v->decay = decay_for(0.22f); break;
    case PERC_RIM:    v->decay = decay_for(0.05f); v->gain = 0.8f; break;
    case PERC_SHAKER: v->decay = decay_for(0.09f); v->gain = 0.45f;
                      v->env = 0.0f; v->attacking = 1; v->attack = 1.0f / (0.008f * RATE); break;
    }
}

static float render_drum(int lane) {
    Voice *v = &voices[V_KICK + lane];
    if (v->attacking) {
        v->env += v->attack;
        if (v->env >= 1.0f) { v->env = 1.0f; v->attacking = 0; }
    } else {
        if (v->env < 0.0001f) return 0.0f;
        v->env *= v->decay;
    }
    float out = 0.0f;
    if (lane == LANE_KICK) {                          // sine with a falling pitch
        out = tsin(v->phase);
        v->phase = wrap(v->phase + v->pitch / RATE);
        v->pitch = 45.0f + (v->pitch - 45.0f) * 0.9992f;
    } else if (lane == LANE_HATS) {                   // high-passed noise
        float n = noise();
        out = n - v->last_noise;
        v->last_noise = n;
    } else {
        switch (v->kind) {
        case PERC_SNARE:                              // noise plus a short tone
            out = 0.7f * noise() + 0.4f * tsin(v->phase);
            v->phase = wrap(v->phase + 190.0f / RATE);
            break;
        case PERC_CLAP: {                             // three quick bursts, then a tail
            float n = noise();
            out = n - 0.5f * v->last_noise;
            v->last_noise = n;
            if (v->age < 1320 && (v->age % 440) >= 180) out *= 0.25f;
            break;
        }
        case PERC_RIM:                                // short, knocky tones
            out = 0.6f * tsin(v->phase) + 0.4f * tsin(v->phase2);
            v->phase = wrap(v->phase + 820.0f / RATE);
            v->phase2 = wrap(v->phase2 + 1650.0f / RATE);
            break;
        default: {                                    // shaker: very bright noise
            float n = noise();
            out = n - v->last_noise;
            v->last_noise = n;
            break;
        }
        }
        v->age++;
    }
    return out * v->env * v->gain;
}

// ---- Realm ambiances ---------------------------------------------------------

static float amb_phase[6], amb_amp[6], amb_target[6], lfo_phase[4];
static int amb_timer[6];
static float pulse_env, pulse_phase, pulse_phase2;
static float wind_low, wind_band, wind_f, crackle_env, crackle_pan;

static void trigger_pulse(void) { pulse_env = 1.0f; pulse_phase = pulse_phase2 = 0.0f; }

// Pulse:   a steady eighth-note heartbeat on the root, high up (like In C)
// Drone:   root, fifth and octave, slowly breathing
// Texture: wind, vinyl crackle and shimmering high chord tones
static float amb_root = 55.0f;             // updated once per buffer

static void render_ambience(float *l, float *r) {
    float root = amb_root;
    float a = 0.0f, b = 0.0f;
    switch (music.realm) {
    case REALM_PULSE: {
        if (pulse_env > 0.0001f) {
            float s = tsin(pulse_phase) + 0.25f * tsin(pulse_phase2) * pulse_env;
            pulse_phase = wrap(pulse_phase + root * 8.0f / RATE);
            pulse_phase2 = wrap(pulse_phase2 + root * 8.0f * 3.93f / RATE);
            pulse_env *= 0.99965f;                  // ~0.45 s ring
            a += s * pulse_env * 0.10f;
        }
        float body = tsin(amb_phase[0]) * 0.035f;   // quiet low root underneath
        amb_phase[0] = wrap(amb_phase[0] + root / RATE);
        a += body;
        b = a;
        break;
    }
    case REALM_DRONE: {
        static const float ratio[3] = { 1.0f, 1.4983f, 2.0f };
        static const float rate[3] = { 0.07f, 0.11f, 0.05f };
        for (int k = 0; k < 3; k++) {
            a += tsin(amb_phase[k]) * (0.55f + 0.45f * tsin(lfo_phase[k])) * 0.045f;
            amb_phase[k] = wrap(amb_phase[k] + root * ratio[k] / RATE);
            lfo_phase[k] = wrap(lfo_phase[k] + rate[k] / RATE);
        }
        b = a;
        break;
    }
    default: {
        // Wind: noise through a slowly moving band-pass filter
        // (wind_f is updated once per buffer in audio_render).
        float n = noise();
        wind_low += wind_f * wind_band;
        wind_band += wind_f * (n - wind_low - 0.5f * wind_band);
        float gust = 0.55f + 0.45f * tsin(lfo_phase[1]);
        lfo_phase[0] = wrap(lfo_phase[0] + 0.05f / RATE);
        lfo_phase[1] = wrap(lfo_phase[1] + 0.13f / RATE);
        float wind = wind_band * gust * 0.10f;
        // Crackle: random little clicks.
        if (rand01() < 6.0f / RATE) { crackle_env = 0.15f + 0.35f * rand01(); crackle_pan = rand01(); }
        float c = 0.0f;
        if (crackle_env > 0.00001f) { c = noise() * crackle_env * 0.25f; crackle_env *= 0.993f; }
        // Shimmer: high chord tones that drift in and out.
        static const float ratio[4] = { 8.0f, 8.0f * 1.4983f, 16.0f * 1.1892f, 16.0f };
        float sl = 0.0f, sr = 0.0f;
        for (int k = 0; k < 4; k++) {
            if (--amb_timer[k] <= 0) { amb_target[k] = rand01() * rand01(); amb_timer[k] = RATE * 2 + (int)(rand01() * RATE * 4); }
            amb_amp[k] += (amb_target[k] - amb_amp[k]) * 0.00003f;
            float s = tsin(amb_phase[k]) * amb_amp[k] * 0.025f;
            amb_phase[k] = wrap(amb_phase[k] + root * ratio[k] / RATE);
            if (k & 1) sr += s; else sl += s;
        }
        a = wind + c * (1.0f - crackle_pan) + sl;
        b = wind + c * crackle_pan + sr;
        break;
    }
    }
    *l += a;
    *r += b;
}

// ---- Sequencer clock (32nd notes) --------------------------------------------

static const double samples_per_tick = RATE * 60.0 / BPM / 8.0;
static double tick_clock;

static void bar_line(void) {
    music.bar++;
    int phrase_start = ((music.bar - 1) % 2) == 0;     // the lead's two-bar phrase
    for (int t = 0; t < NUM_TRACKS; t++) {
        if (t == TRK_LEAD && !phrase_start) continue;
        Track *tr = &music.tracks[t];
        if (tr->state == ST_WAITING)  tr->state = ST_PLAYING;
        if (tr->state == ST_STOPPING) tr->state = ST_OFF;
        if (tr->swap) {
            for (int s = 0; s < MAX_STEPS; s++) tr->steps[s] = tr->next_steps[s];
            tr->swap = 0;
        }
    }
    if (music.pending_root >= 0) {
        music.root = music.pending_root;
        music.pending_root = -1;
    }
}

static void advance_tick(void) {
    music.tick = (music.tick + 1) % 32;
    if (music.tick == 0) bar_line();
    int on16 = (music.tick % 2) == 0;
    if (music.realm == REALM_PULSE && music.tick % 4 == 0) trigger_pulse();

    for (int t = 0; t < NUM_TRACKS; t++) {
        Track *tr = &music.tracks[t];
        if (tr->state != ST_PLAYING && tr->state != ST_STOPPING) continue;
        if (t == TRK_DRUMS) {
            if (!on16) continue;
            for (int lane = 0; lane < NUM_LANES; lane++) {
                int v = tr->steps[lane * 16 + music.tick / 2];
                if (v != REST) { trigger_drum(lane, v); tr->flash = 1; }
            }
            continue;
        }
        if (t != TRK_BELLS && !on16) continue;
        int v = tr->steps[track_position(t)];
        if (v != REST) { trigger_melodic(t, v, music.tick / 2); tr->flash = 1; }
    }
}

// ---- Space effects: echo or reverb -------------------------------------------

#define DELAY_LEN 20889          // 3 sixteenths at 95 BPM (a dotted eighth)
static float delay_l[DELAY_LEN], delay_r[DELAY_LEN];
static int delay_pos;

// A small "Freeverb"-style reverb: 4 comb filters and 2 all-pass filters per side.
static const int comb_len[4] = { 1116, 1277, 1422, 1557 };
static const int ap_len[2] = { 556, 441 };
#define STEREO_SPREAD 23
static float comb_buf[2][4][1557 + STEREO_SPREAD], comb_store[2][4];
static int comb_pos[2][4];
static float ap_buf[2][2][556 + STEREO_SPREAD];
static int ap_pos[2][2];

static float reverb_side(int side, float in, float feedback) {
    float out = 0.0f;
    for (int k = 0; k < 4; k++) {
        int len = comb_len[k] + side * STEREO_SPREAD;
        float *buf = comb_buf[side][k];
        float y = buf[comb_pos[side][k]];
        comb_store[side][k] = y * 0.7f + comb_store[side][k] * 0.3f;   // darken the tail
        buf[comb_pos[side][k]] = in + comb_store[side][k] * feedback;
        if (++comb_pos[side][k] >= len) comb_pos[side][k] = 0;
        out += y;
    }
    for (int k = 0; k < 2; k++) {
        int len = ap_len[k] + side * STEREO_SPREAD;
        float *buf = ap_buf[side][k];
        float y = buf[ap_pos[side][k]];
        buf[ap_pos[side][k]] = out + y * 0.5f;
        out = y - out;
        if (++ap_pos[side][k] >= len) ap_pos[side][k] = 0;
    }
    return out;
}

// ---- Master filter: low-pass, high-pass, band-pass or notch --------------------

static float cut_smooth = 0.75f, res_smooth = 0.2f;
static float flow[2], fband[2];

static inline float svf(int side, float in, float f, float q, int type) {
    float high = 0.0f;
    for (int k = 0; k < 2; k++) {               // run twice per sample for stability
        flow[side] += f * fband[side];
        high = in - flow[side] - q * fband[side];
        fband[side] += f * high;
    }
    switch (type) {
    case FILT_HIGH:  return high;
    case FILT_BAND:  return fband[side];
    case FILT_NOTCH: return high + flow[side];
    default:         return flow[side];
    }
}

static inline float soft_clip(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

void audio_render(short *out, int frames) {
    cut_smooth += (music.cutoff - cut_smooth) * 0.15f;
    res_smooth += (music.resonance - res_smooth) * 0.15f;
    float fc = 120.0f * powf(8000.0f / 120.0f, cut_smooth);       // 120 Hz .. 8 kHz
    float f = 2.0f * sinf(PI * fc / (2.0f * RATE));
    float q = 1.4f - 1.25f * res_smooth;
    int ftype = music.filter_type;
    float wet = music.space_mix;
    int reverb = music.space == SPACE_REVERB;
    float echo_fb = music.space_amt * 0.8f;
    float room = 0.70f + 0.27f * music.space_amt;
    float duck_decay = decay_for(0.30f);
    float wind_center = 350.0f + 900.0f * (0.5f + 0.5f * tsin(lfo_phase[0]));
    wind_f = 2.0f * sinf(PI * wind_center / RATE);
    amb_root = root_freq();

    for (int i = 0; i < frames; i++) {
        tick_clock += 1.0;
        if (tick_clock >= samples_per_tick) {
            tick_clock -= samples_per_tick;
            advance_tick();
        }

        // Pump: duck quickly when the bass hits, then swell back.
        duck_target = duck_target > 0.00001f ? duck_target * duck_decay : 0.0f;
        duck_level += (duck_target - duck_level) * 0.01f;
        if (duck_level < 0.00001f) duck_level = 0.0f;
        float duck = 1.0f - 0.65f * duck_level;

        float l = 0.0f, r = 0.0f, send = 0.0f;
        for (int t = 0; t < 3; t++) {
            float s = render_melodic(t) * track_gain[t];
            if (t != TRK_BASS) { s *= duck; send += s; }
            l += s * (1.0f - track_pan[t]) * 1.4f;
            r += s * track_pan[t] * 1.4f;
        }
        for (int lane = 0; lane < NUM_LANES; lane++) {
            float s = render_drum(lane) * lane_gain[lane];
            if (lane != LANE_KICK) s *= duck;
            if (lane == LANE_PERC) send += s * 0.3f;
            l += s * (1.0f - lane_pan[lane]) * 1.4f;
            r += s * lane_pan[lane] * 1.4f;
        }

        float al = 0.0f, ar = 0.0f;
        render_ambience(&al, &ar);
        l += al * duck;
        r += ar * duck;

        // A tiny offset, far too quiet to hear, keeps the echo/reverb tails
        // from decaying into numbers the processor handles slowly.
        send += 1e-12f;
        float rl, rr;
        if (reverb) {
            rl = reverb_side(0, send * 0.05f, room);
            rr = reverb_side(1, send * 0.05f, room);
        } else {
            rl = delay_l[delay_pos];
            rr = delay_r[delay_pos];
            delay_l[delay_pos] = send + rr * echo_fb;
            delay_r[delay_pos] = rl * echo_fb;
            if (++delay_pos >= DELAY_LEN) delay_pos = 0;
        }
        l += rl * wet * duck;
        r += rr * wet * duck;

        l = soft_clip(svf(0, l, f, q, ftype));
        r = soft_clip(svf(1, r, f, q, ftype));
        out[2 * i]     = (short)(l * 30000.0f);
        out[2 * i + 1] = (short)(r * 30000.0f);
    }
}

void audio_init(void) {
    memset(&music, 0, sizeof(music));
    for (int i = 0; i < TABLE; i++) sine_tab[i] = sinf(2.0f * PI * i / TABLE);
    for (int t = 0; t < NUM_TRACKS; t++) {
        for (int s = 0; s < MAX_STEPS; s++) music.tracks[t].steps[s] = REST;
        if (t < 3) music.tracks[t].wave = default_wave[t];
    }
    music.tick = 31;                     // the first sample starts bar 1
    music.pending_root = -1;
    music.realm = REALM_DRONE;
    music.cutoff = 0.75f;
    music.resonance = 0.2f;
    music.space_mix = 0.5f;
    music.space_amt = 0.56f;
    tick_clock = samples_per_tick;
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
