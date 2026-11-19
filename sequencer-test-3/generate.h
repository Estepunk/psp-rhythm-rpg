// Random loop generator. Notes are weighted toward where they'd musically
// be expected (downbeats for bass, a steady groove for drums, and so on).
#ifndef GENERATE_H
#define GENERATE_H

#include "audio.h"

// Drum styles, shown in the game as symbols for now.
enum { STYLE_FOUR, STYLE_HIPHOP, STYLE_DEMBOW, NUM_STYLES };   // *  +  #

void gen_seed(unsigned int seed);
void gen_loop(int track, int drum_style, int out[MAX_STEPS]);

#endif
