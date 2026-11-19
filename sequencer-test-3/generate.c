// Random loop generator.
//
// Each step of a bar has a "strength": the downbeat, beat 3, beats 2 and 4,
// the eighth-note "and"s, and the sixteenths in between. Each instrument
// has its own chance (in percent) of playing at each strength, so notes land
// mostly where you'd expect them, with the odd surprise. Those `odds` tables
// are the easiest thing to tweak.
//
// Scale degrees are in the current minor key: 0 = root, 2 = third, 4 = fifth,
// 7 = root an octave up, and so on.
#include "generate.h"

static unsigned int rng = 12345;

void gen_seed(unsigned int seed) { rng = seed ? seed : 12345; }

static unsigned int next(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;   // xorshift
    return rng;
}
static int chance(int percent) { return (int)(next() % 100) < percent; }
static int pick(int n) { return (int)(next() % (unsigned)n); }

static int weighted(const int *values, const int *weights, int n) {
    int total = 0;
    for (int i = 0; i < n; i++) total += weights[i];
    int r = pick(total);
    for (int i = 0; i < n; i++) {
        if (r < weights[i]) return values[i];
        r -= weights[i];
    }
    return values[0];
}

// Strength of a sixteenth-note step within a bar.
enum { DOWNBEAT, BEAT3, BACKBEAT, EIGHTH, SIXTEENTH };
static int strength(int step) {
    step %= 16;
    if (step == 0) return DOWNBEAT;
    if (step == 8) return BEAT3;
    if (step % 4 == 0) return BACKBEAT;
    if (step % 2 == 0) return EIGHTH;
    return SIXTEENTH;
}

static int is_chord_tone(int d) { int m = d % 7; return m == 0 || m == 2 || m == 4; }

// ---- Bass: strong on the downbeat, mostly root and fifth -------------------
// About a third of the notes after the downbeat are marked to slide, so the
// Slide power has something to glide into.

static void gen_bass(int *out) {
    static const int odds[] = { 95, 60, 35, 20, 7 };
    static const int notes[]   = { 0, 4, 2, 7, 5, 6, 3 };
    static const int weights[] = { 35, 25, 15, 10, 6, 5, 4 };
    for (int s = 0; s < 16; s++) {
        out[s] = REST;
        if (!chance(odds[strength(s)])) continue;
        out[s] = s == 0 ? (chance(85) ? 0 : 7) : weighted(notes, weights, 7);
        if (s > 0 && chance(35)) out[s] |= SLIDE_FLAG;
    }
    if (out[0] == REST) out[0] = 0;
}

// ---- Lead: a two-bar phrase. Bar 2 answers bar 1. --------------------------
// The answer starts like the question (sometimes a step higher), then goes
// somewhere new and tends to come home to a chord tone.

static int lead_walk(int note, int st) {
    static const int moves[]   = { -2, -1, 0, 1, 2, 3, -3 };
    static const int weights[] = { 15, 25, 10, 25, 15, 5, 5 };
    note += weighted(moves, weights, 7);
    if (note < 4) note = 4;
    if (note > MAX_DEGREE) note = MAX_DEGREE;
    if (st == DOWNBEAT || st == BEAT3)                    // settle on strong beats
        while (!is_chord_tone(note)) note--;
    return note;
}

static void gen_lead(int *out) {
    static const int odds[] = { 60, 45, 35, 22, 6 };
    int note = 7 + pick(3) * 2;                          // start on A', C' or E'
    int last = note;
    for (int s = 0; s < 16; s++) {                       // bar 1: the question
        out[s] = REST;
        if (!chance(odds[strength(s)])) continue;
        note = lead_walk(note, strength(s));
        out[s] = last = note;
    }
    int lift = chance(30) ? 2 : 0;                       // bar 2: the answer
    for (int s = 0; s < 8; s++) {
        int v = out[s];
        if (v != REST) { v += lift; if (v > MAX_DEGREE) v = MAX_DEGREE; last = v; }
        out[16 + s] = v;
    }
    note = last;
    for (int s = 8; s < 16; s++) {
        out[16 + s] = REST;
        if (!chance(odds[strength(s)])) continue;
        note = lead_walk(note, strength(s));
        out[16 + s] = note;
    }
    if (chance(60)) out[24] = chance(50) ? 7 : 4;        // come home on beat 3
    int count = 0;
    for (int s = 0; s < 32; s++) if (out[s] != REST) count++;
    if (count < 3) { out[0] = 7; out[10] = 9; out[16] = 7; out[24] = 4; }
}

// ---- Bells: double-time arpeggios (32 steps in one bar) --------------------
// Sometimes a beat bursts into a fast run of four notes.

static void gen_bells(int *out) {
    static const int arp[] = { 0, 2, 4, 7, 9, 11 };      // chord tones over two octaves
    int pos = pick(3), dir = chance(50) ? 1 : -1;
    for (int s = 0; s < 32; s++) {
        int odds = s == 0 ? 85 : s % 8 == 0 ? 70 : s % 4 == 0 ? 60 : s % 2 == 0 ? 40 : 12;
        out[s] = REST;
        if (!chance(odds)) continue;
        out[s] = arp[pos];
        if (chance(20)) dir = -dir;
        pos += dir;
        if (pos < 0) { pos = 1; dir = 1; }
        if (pos > 5) { pos = 4; dir = -1; }
    }
    for (int beat = 0; beat < 4; beat++) {               // fast runs
        if (!chance(18)) continue;
        int p = pick(3), d = chance(50) ? 1 : -1;
        for (int k = 0; k < 4; k++) {
            out[beat * 8 + k] = arp[p];
            p += d;
            if (p < 0 || p > 5) { d = -d; p += 2 * d; }
        }
    }
}

// ---- Drums: three lanes, in one of three styles ------------------------------
// Steps 0-15 kick, 16-31 hats, 32-47 percussion (snare, clap, rim, shaker).

static void set(int *out, int lane, int step, int value) { out[lane * 16 + step] = value; }

static void gen_drums(int style, int *out) {
    for (int s = 0; s < MAX_STEPS; s++) out[s] = REST;

    switch (style) {
    case STYLE_FOUR:            // *  four on the floor
        for (int s = 0; s < 16; s++) {
            if (s % 4 == 0 && chance(97)) set(out, LANE_KICK, s, 0);
            if (s % 4 == 2 && chance(85)) set(out, LANE_HATS, s, HAT_OPEN);
            else if (s % 2 == 1 && chance(40)) set(out, LANE_HATS, s, HAT_CLOSED);
            if ((s == 4 || s == 12) && chance(92)) set(out, LANE_PERC, s, PERC_CLAP);
            else if (chance(15)) set(out, LANE_PERC, s, PERC_SHAKER);
        }
        if (chance(10)) set(out, LANE_KICK, 15, 0);
        break;

    case STYLE_HIPHOP:          // +  laid-back kick, snare on 2 and 4
        set(out, LANE_KICK, 0, 0);
        if (chance(65)) set(out, LANE_KICK, 10, 0);
        if (chance(35)) set(out, LANE_KICK, 7, 0);
        if (chance(20)) set(out, LANE_KICK, 3, 0);
        if (chance(15)) set(out, LANE_KICK, 8, 0);
        if (chance(15)) set(out, LANE_KICK, 15, 0);
        for (int s = 0; s < 16; s++) {
            if (s % 2 == 0 && chance(90)) set(out, LANE_HATS, s, HAT_CLOSED);
            else if (s % 2 == 1 && chance(10)) set(out, LANE_HATS, s, HAT_CLOSED);
            if ((s == 4 || s == 12) && chance(95)) set(out, LANE_PERC, s, PERC_SNARE);
            else if ((s == 7 || s == 9 || s == 15) && chance(15)) set(out, LANE_PERC, s, PERC_RIM);
        }
        if (chance(12)) set(out, LANE_HATS, 14, HAT_OPEN);
        break;

    default:                    // #  dembow: kick on every beat, snare on the off-beat pattern
        for (int s = 0; s < 16; s++) {
            if (s % 4 == 0 && chance(95)) set(out, LANE_KICK, s, 0);
            if ((s == 3 || s == 6 || s == 11 || s == 14) && chance(92)) set(out, LANE_PERC, s, PERC_SNARE);
            else if (chance(10)) set(out, LANE_PERC, s, chance(50) ? PERC_RIM : PERC_SHAKER);
            if (s % 2 == 0 && chance(65)) set(out, LANE_HATS, s, HAT_CLOSED);
        }
        break;
    }

    set(out, LANE_KICK, 0, 0);                               // always land on 1
    if (chance(15))                                          // end-of-bar fill
        for (int s = 13; s < 16; s++)
            if (chance(60)) set(out, LANE_PERC, s, PERC_SNARE);
}

void gen_loop(int track, int drum_style, int out[MAX_STEPS]) {
    for (int s = 0; s < MAX_STEPS; s++) out[s] = REST;
    switch (track) {
    case TRK_BASS:  gen_bass(out);  break;
    case TRK_LEAD:  gen_lead(out);  break;
    case TRK_BELLS: gen_bells(out); break;
    default:        gen_drums(drum_style, out); break;
    }
}
