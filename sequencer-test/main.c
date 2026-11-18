// Sequencer test: walk between colored zones, launch each zone's loop on the
// next bar, edit the loops, and sweep a filter with the analog nub.
// Every 30 launches the key moves up a fifth (A minor -> E minor -> ...).
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <stdio.h>
#include <stdarg.h>
#include "audio.h"

PSP_MODULE_INFO("Sequencer Test", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);   // give the program all memory but 1 MB

#define LAUNCHES_PER_KEY_CHANGE 30

// ---- Screen and drawing ----------------------------------------------------

#define SCR_W 480
#define SCR_H 272
#define BUF_W 512                                 // VRAM row width in pixels
#define FRAME_BYTES (BUF_W * SCR_H * 4)

static int draw_buf = 1;                          // the buffer not on screen

static u32 *back_buffer(void) {                   // uncached VRAM address
    return (u32 *)(0x44000000 + draw_buf * FRAME_BYTES);
}

static void flip(void) {
    sceDisplaySetFrameBuf((void *)(0x04000000 + draw_buf * FRAME_BYTES), BUF_W,
                          PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();
    draw_buf ^= 1;
}

// PSP pixels are 0xAABBGGRR. 'bright' scales the color (0..1), 'white' mixes
// toward white (0..1) for flashes.
static u32 color(int r, int g, int b, float bright, float white) {
    float rr = r * bright, gg = g * bright, bb = b * bright;
    rr += (255 - rr) * white; gg += (255 - gg) * white; bb += (255 - bb) * white;
    return 0xFF000000 | ((u32)bb << 16) | ((u32)gg << 8) | (u32)rr;
}
#define WHITE 0xFFFFFFFF
#define GREY  0xFFA0A0A0

static void fill_rect(int x, int y, int w, int h, u32 c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCR_W) w = SCR_W - x;
    if (y + h > SCR_H) h = SCR_H - y;
    u32 *buf = back_buffer();
    for (int j = 0; j < h; j++) {
        u32 *row = buf + (y + j) * BUF_W + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

static void outline_rect(int x, int y, int w, int h, u32 c) {
    fill_rect(x, y, w, 1, c);
    fill_rect(x, y + h - 1, w, 1, c);
    fill_rect(x, y, 1, h, c);
    fill_rect(x + w - 1, y, 1, h, c);
}

// Text on an 8x8 grid: 60 columns x 34 rows.
static void text(int col, int row, u32 c, const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pspDebugScreenSetBase(back_buffer());
    pspDebugScreenSetXY(col, row);
    pspDebugScreenSetTextColor(c);
    pspDebugScreenPrintf("%s", buf);
}

// ---- Zones -----------------------------------------------------------------

typedef struct { int x, y, w, h, r, g, b; const char *name; } Zone;

static const Zone zones[NUM_TRACKS] = {
    {   8,  24, 220, 108,  40,  80, 170, "BASS"  },
    { 252,  24, 220, 108, 210, 120,  40, "LEAD"  },
    {   8, 152, 220, 108,  40, 150,  90, "BELLS" },
    { 252, 152, 220, 108, 140,  60, 170, "DRUMS" },
};

static const char *state_names[] = { "off", "starts next bar", "playing", "stops next bar" };

static int player_x = 236, player_y = 138;        // starts in the crossroads
static float flash[NUM_TRACKS];
static int frame;
static int launches;
static int key_msg_timer;                         // frames to show "key change"

static int zone_at_player(void) {
    int cx = player_x + 4, cy = player_y + 4;
    for (int t = 0; t < NUM_TRACKS; t++) {
        const Zone *z = &zones[t];
        if (cx >= z->x && cx < z->x + z->w && cy >= z->y && cy < z->y + z->h) return t;
    }
    return -1;
}

// Start or stop a track. Both happen on the next bar line.
static void toggle_track(int t) {
    Track *tr = &music.tracks[t];
    switch (tr->state) {
    case ST_OFF:
        tr->state = ST_WAITING;
        launches++;
        if (launches % LAUNCHES_PER_KEY_CHANGE == 0)
            music.pending_root = (music.root + 7) % 12;   // up a fifth
        break;
    case ST_WAITING:  tr->state = ST_OFF; break;       // cancel before it starts
    case ST_PLAYING:  tr->state = ST_STOPPING; break;
    case ST_STOPPING: tr->state = ST_PLAYING; break;   // changed your mind
    }
}

static void stop_all(void) {
    for (int t = 0; t < NUM_TRACKS; t++) {
        Track *tr = &music.tracks[t];
        if (tr->state == ST_WAITING) tr->state = ST_OFF;
        if (tr->state == ST_PLAYING) tr->state = ST_STOPPING;
    }
}

static float zone_brightness(int t) {
    int blink = (frame / 10) & 1;
    switch (music.tracks[t].state) {
    case ST_WAITING:  return blink ? 0.80f : 0.40f;
    case ST_PLAYING:  return 0.85f;
    case ST_STOPPING: return blink ? 0.85f : 0.50f;
    default:          return 0.35f;
    }
}

// The 16 steps as a strip of little cells.
static void draw_step_strip(int t, int x, int y, int cell, int gap, int h) {
    const Zone *z = &zones[t];
    int active = music.tracks[t].state == ST_PLAYING || music.tracks[t].state == ST_STOPPING;
    for (int s = 0; s < NUM_STEPS; s++) {
        int on = music.tracks[t].steps[s] != REST;
        u32 c = on ? color(z->r, z->g, z->b, 1.0f, 0.45f) : color(z->r, z->g, z->b, 0.18f, 0.0f);
        if (active && s == music.step) c = WHITE;
        fill_rect(x + s * (cell + gap), y, cell, h, c);
    }
}

// ---- HUD shared by both screens ---------------------------------------------

static void bar_graph(char *out, float v) {
    int n = (int)(v * 10.0f + 0.5f);
    for (int i = 0; i < 10; i++) out[i] = i < n ? '#' : '-';
    out[10] = 0;
}

static void draw_hud(void) {
    fill_rect(0, 0, SCR_W, 24, 0xFF101010);
    text(1, 0, WHITE, "Key: %s minor  %d BPM  Bar %d  Beat %d",
         key_name(music.root), BPM, music.bar, music.step / 4 + 1);
    int left = LAUNCHES_PER_KEY_CHANGE - launches % LAUNCHES_PER_KEY_CHANGE;
    if (music.pending_root >= 0)
        text(40, 0, 0xFF40E0FF, "-> %s minor next bar", key_name(music.pending_root));
    else
        text(40, 0, GREY, "Launches %d (+%d)", launches, left);

    char cut[11], res[11];
    bar_graph(cut, music.cutoff);
    bar_graph(res, music.resonance);
    text(1, 1, GREY, "Filter (hold R + nub)  Cutoff [%s]  Res [%s]", cut, res);

    if (key_msg_timer > 0)
        text(1, 2, 0xFF40E0FF, "KEY CHANGE: now in %s minor", key_name(music.root));
}

// ---- Map screen --------------------------------------------------------------

static void draw_map(void) {
    fill_rect(0, 24, SCR_W, SCR_H - 24, color(30, 30, 38, 1.0f, 0.0f));
    int here = zone_at_player();

    for (int t = 0; t < NUM_TRACKS; t++) {
        const Zone *z = &zones[t];
        fill_rect(z->x, z->y, z->w, z->h, color(z->r, z->g, z->b, zone_brightness(t), flash[t] * 0.35f));
        if (t == here) outline_rect(z->x, z->y, z->w, z->h, WHITE);
        int col = z->x / 8 + 1, row = z->y / 8 + 1;
        text(col, row, WHITE, "%s", z->name);
        text(col, row + 1, WHITE, "%s", state_names[music.tracks[t].state]);
        if (t == here) {
            text(col, row + 3, WHITE, "X: start/stop");
            text(col, row + 4, WHITE, "Triangle: edit loop");
        }
        draw_step_strip(t, z->x + 14, z->y + z->h - 16, 10, 2, 8);
    }

    fill_rect(player_x - 1, player_y - 1, 10, 10, 0xFF000000);
    fill_rect(player_x, player_y, 8, 8, WHITE);

    fill_rect(0, 262, SCR_W, 10, 0xFF101010);
    text(1, 33, GREY, "D-pad move  X start/stop  Tri edit  START stop all");
}

// ---- Loop editor screen ------------------------------------------------------

static int edit_track = 0, cursor = 0;
static int memory[NUM_TRACKS][NUM_STEPS];   // last note of each step, for X on/off

#define CELL_X0 48
#define CELL_W  24
#define CELL_Y0 64
#define CELL_H  112

static void draw_editor(void) {
    const Zone *z = &zones[edit_track];
    Track *tr = &music.tracks[edit_track];
    fill_rect(0, 24, SCR_W, SCR_H - 24, color(z->r, z->g, z->b, 0.20f, 0.0f));

    text(6, 4, WHITE, "EDITING %s  -  %s", z->name, state_names[tr->state]);
    int playing = tr->state == ST_PLAYING || tr->state == ST_STOPPING;
    int top = edit_track == TRK_DRUMS ? DRUM_HAT : MAX_DEGREE;

    for (int s = 0; s < NUM_STEPS; s++) {
        int x = CELL_X0 + s * CELL_W;
        u32 bg = s == cursor ? color(z->r, z->g, z->b, 0.55f, 0.0f)
                             : color(z->r, z->g, z->b, 0.32f, 0.0f);
        fill_rect(x, CELL_Y0, CELL_W - 2, CELL_H, bg);
        int v = tr->steps[s];
        if (v != REST) {                             // taller bar = higher note
            int h = (v + 1) * CELL_H / (top + 1);
            float w = (playing && s == music.step) ? 0.7f : 0.25f;
            fill_rect(x + 3, CELL_Y0 + CELL_H - h, CELL_W - 8, h, color(z->r, z->g, z->b, 1.0f, w));
        }
        if (s == cursor) outline_rect(x - 1, CELL_Y0 - 1, CELL_W, CELL_H + 2, WHITE);
        if (playing && s == music.step) fill_rect(x, CELL_Y0 + CELL_H + 4, CELL_W - 2, 4, WHITE);
        if (s % 4 == 0) fill_rect(x, CELL_Y0 - 6, 2, 4, GREY);   // beat marks

        char label[4];
        note_label(edit_track, v, label);
        text(x / 8, 23, WHITE, "%s", label);
    }

    text(6, 27, WHITE, "Left/Right: pick step   Up/Down: change note");
    text(6, 28, WHITE, "X: step on/off   Square: clear   SELECT: start/stop");
    text(6, 29, WHITE, "Circle: back to the map");
    text(6, 31, GREY,  "Notes are in the current key, so they move with it.");
}

// ---- Exit callback (HOME button) ---------------------------------------------

static int exit_callback(int arg1, int arg2, void *common) {
    sceKernelExitGame();
    return 0;
}
static int callback_thread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}
static void setup_callbacks(void) {
    int thid = sceKernelCreateThread("callback_thread", callback_thread, 0x11, 0xFA0, 0, NULL);
    if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
}

// ---- Main loop ---------------------------------------------------------------

int main(void) {
    setup_callbacks();
    pspDebugScreenInit();
    pspDebugScreenEnableBackColor(0);         // text draws over our graphics
    pspDebugScreenClearLineDisable();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    audio_init();
    for (int t = 0; t < NUM_TRACKS; t++)
        for (int s = 0; s < NUM_STEPS; s++)
            memory[t][s] = music.tracks[t].steps[s] != REST ? music.tracks[t].steps[s] : 0;
    audio_start();

    int editing = 0, last_root = music.root;
    SceCtrlData pad;
    unsigned int last = 0;

    while (1) {
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~last;

        // Filter: hold R and push the nub. Left/right = cutoff, up/down = resonance.
        // Values stay where you leave them when you let go.
        if (pad.Buttons & PSP_CTRL_RTRIGGER) {
            int dx = pad.Lx - 128, dy = pad.Ly - 128;
            if (dx > 30 || dx < -30) music.cutoff += dx / 127.0f * 0.012f;
            if (dy > 30 || dy < -30) music.resonance -= dy / 127.0f * 0.012f;
            if (music.cutoff < 0.0f) music.cutoff = 0.0f;
            if (music.cutoff > 1.0f) music.cutoff = 1.0f;
            if (music.resonance < 0.0f) music.resonance = 0.0f;
            if (music.resonance > 1.0f) music.resonance = 1.0f;
        }

        if (!editing) {
            if (pad.Buttons & PSP_CTRL_UP)    player_y -= 2;
            if (pad.Buttons & PSP_CTRL_DOWN)  player_y += 2;
            if (pad.Buttons & PSP_CTRL_LEFT)  player_x -= 2;
            if (pad.Buttons & PSP_CTRL_RIGHT) player_x += 2;
            if (player_x < 0) player_x = 0;
            if (player_x > SCR_W - 8) player_x = SCR_W - 8;
            if (player_y < 22) player_y = 22;
            if (player_y > 252) player_y = 252;

            int here = zone_at_player();
            if (here >= 0 && (pressed & PSP_CTRL_CROSS)) toggle_track(here);
            if (here >= 0 && (pressed & PSP_CTRL_TRIANGLE)) { editing = 1; edit_track = here; }
            if (pressed & PSP_CTRL_START) stop_all();
        } else {
            Track *tr = &music.tracks[edit_track];
            int top = edit_track == TRK_DRUMS ? DRUM_HAT : MAX_DEGREE;
            if (pressed & PSP_CTRL_LEFT)  cursor = (cursor + NUM_STEPS - 1) % NUM_STEPS;
            if (pressed & PSP_CTRL_RIGHT) cursor = (cursor + 1) % NUM_STEPS;
            if (pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
                int v = tr->steps[cursor];
                if (v == REST) v = memory[edit_track][cursor];      // turn the step on
                else if (pressed & PSP_CTRL_UP) v = v < top ? v + 1 : top;
                else v = v > 0 ? v - 1 : 0;
                tr->steps[cursor] = v;
                memory[edit_track][cursor] = v;
            }
            if (pressed & PSP_CTRL_CROSS)
                tr->steps[cursor] = tr->steps[cursor] == REST ? memory[edit_track][cursor] : REST;
            if (pressed & PSP_CTRL_SQUARE)
                for (int s = 0; s < NUM_STEPS; s++) tr->steps[s] = REST;
            if (pressed & PSP_CTRL_SELECT) toggle_track(edit_track);
            if (pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_TRIANGLE)) editing = 0;
        }

        // Flashes from the audio thread, and the key change message.
        for (int t = 0; t < NUM_TRACKS; t++) {
            if (music.tracks[t].flash) { flash[t] = 1.0f; music.tracks[t].flash = 0; }
            flash[t] *= 0.85f;
        }
        if (music.root != last_root) { key_msg_timer = 180; last_root = music.root; }
        if (key_msg_timer > 0) key_msg_timer--;

        if (editing) draw_editor(); else draw_map();
        draw_hud();
        flip();

        last = pad.Buttons;
        frame++;
    }
    return 0;
}
