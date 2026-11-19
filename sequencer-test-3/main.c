// Sequencer test 3 (free jam).
//
// Pick a realm (its ambiance plays underneath), then walk between four
// instrument zones. Each zone loops on the next bar line; inside each zone are
// pads: loop banks for the melodic instruments, style pads for the drums, and
// power pads for the bass. The center pad opens a settings menu.
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <psppower.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "audio.h"
#include "generate.h"

PSP_MODULE_INFO("Sequencer Test 3", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);   // give the program all memory but 1 MB

#define AUTO_KEY_LAUNCHES 30   // launches per key change, when that's turned on

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
static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

#define WHITE  0xFFFFFFFF
#define GREY   0xFFA0A0A0
#define YELLOW 0xFF40E0FF
#define BLACK  0xFF000000

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

// ---- Realms ----------------------------------------------------------------

static const char *realm_names[NUM_REALMS] = { "PULSE", "DRONE", "TEXTURE" };
static const char *realm_info[NUM_REALMS][2] = {
    { "A steady heartbeat, high and bright,", "ticking on every eighth note." },
    { "A low drone of root, fifth and octave,", "slowly breathing." },
    { "Wind, vinyl crackle, and high chord", "tones drifting in and out." },
};
static const int realm_bg[NUM_REALMS][3] = { { 20, 40, 44 }, { 32, 28, 42 }, { 44, 34, 24 } };

// ---- Zones and pads ----------------------------------------------------------

typedef struct { int x, y, w, h, r, g, b; const char *name; } Zone;

static const Zone zones[NUM_TRACKS] = {
    {   8,  24, 220, 108,  40,  80, 170, "BASS"  },
    { 256,  24, 216, 108, 210, 120,  40, "LEAD"  },
    {   8, 152, 220, 108,  40, 150,  90, "BELLS" },
    { 256, 152, 216, 108, 140,  60, 170, "DRUMS" },
};

// Pads: row 0 = banks (melodic) or styles (drums); row 1 = bass powers.
#define PAD_W 32
#define PAD_H 20
static void pad_rect(int t, int row, int i, int *x, int *y) {
    *x = zones[t].x + 96 + i * 40;
    *y = zones[t].y + (row == 0 ? 28 : 52);
}
static int pad_rows(int t) { return t == TRK_BASS ? 2 : 1; }

static const char *style_symbols[NUM_STYLES] = { "*", "+", "#" };
static const char *fx_labels[3] = { "SLD", "SUB", "PMP" };
static const char *fx_names[3] = { "Slide", "Sub", "Pump" };

// The menu pad sits in the middle of the crossroads.
#define MENU_X 230
#define MENU_Y 134
#define MENU_W 24
#define MENU_H 16

static const char *state_names[] = { "off", "starting", "playing", "stopping" };

// ---- Game state ---------------------------------------------------------------

enum { SCR_REALM, SCR_MAP, SCR_EDIT, SCR_MENU };
static int screen = SCR_REALM;
static int realm_choice = REALM_DRONE;

static float player_x = 238, player_y = 138;
static float flash[NUM_TRACKS];
static int frame, launches, auto_key, key_msg_timer;
static int drum_style = STYLE_FOUR;
static int memory[NUM_TRACKS][MAX_STEPS];   // last note of each step, for X on/off
static unsigned int held;                   // buttons held this frame

typedef struct { int filled, wave, steps[MAX_STEPS]; } Bank;
static Bank banks[3][3];                    // [melodic track][bank]

static int inside(float px, float py, int x, int y, int w, int h) {
    int cx = (int)px + 4, cy = (int)py + 4;
    return cx >= x && cx < x + w && cy >= y && cy < y + h;
}

static int zone_at_player(void) {
    for (int t = 0; t < NUM_TRACKS; t++)
        if (inside(player_x, player_y, zones[t].x, zones[t].y, zones[t].w, zones[t].h)) return t;
    return -1;
}

// Which pad the player stands on in zone t: returns 1 and fills row/index.
static int pad_at_player(int t, int *row, int *idx) {
    for (int r = 0; r < pad_rows(t); r++)
        for (int i = 0; i < 3; i++) {
            int x, y;
            pad_rect(t, r, i, &x, &y);
            if (inside(player_x, player_y, x, y, PAD_W, PAD_H)) { *row = r; *idx = i; return 1; }
        }
    return 0;
}

static int on_menu_pad(void) { return inside(player_x, player_y, MENU_X, MENU_Y, MENU_W, MENU_H); }

// ---- Loops --------------------------------------------------------------------

static int playing(int t) {
    int s = music.tracks[t].state;
    return s == ST_PLAYING || s == ST_STOPPING;
}

// The loop to show and edit: the queued one if a change is on its way.
static int shown_step(int t, int s) {
    Track *tr = &music.tracks[t];
    return tr->swap ? tr->next_steps[s] : tr->steps[s];
}

static void set_step(int t, int s, int v) {
    Track *tr = &music.tracks[t];
    if (tr->swap) tr->next_steps[s] = v;
    if (!tr->swap) tr->steps[s] = v;        // also covers a swap that just happened
}

// Put a whole loop in place. A playing loop switches at its next bar line.
static void queue_loop(int t, const int *loop) {
    Track *tr = &music.tracks[t];
    int p = playing(t);
    for (int s = 0; s < MAX_STEPS; s++) {
        if (p) tr->next_steps[s] = loop[s];
        else tr->steps[s] = loop[s];
        if (loop[s] != REST) memory[t][s] = loop[s];
    }
    if (p) tr->swap = 1;
}

static void new_loop(int t) {
    int loop[MAX_STEPS];
    gen_loop(t, drum_style, loop);
    queue_loop(t, loop);
}

static void save_bank(int t, int i) {
    Bank *b = &banks[t][i];
    for (int s = 0; s < MAX_STEPS; s++) b->steps[s] = shown_step(t, s);
    b->wave = music.tracks[t].wave;
    b->filled = 1;
}

static void load_bank(int t, int i) {
    queue_loop(t, banks[t][i].steps);
    music.tracks[t].wave = banks[t][i].wave;
}

// Start or stop a track. Both happen on the next bar line.
static void toggle_track(int t) {
    Track *tr = &music.tracks[t];
    switch (tr->state) {
    case ST_OFF:
        tr->state = ST_WAITING;
        launches++;
        if (auto_key && launches % AUTO_KEY_LAUNCHES == 0)
            music.pending_root = (music.root + 7) % 12;   // up a fifth
        break;
    case ST_WAITING:  tr->state = ST_OFF; break;
    case ST_PLAYING:  tr->state = ST_STOPPING; break;
    case ST_STOPPING: tr->state = ST_PLAYING; break;
    }
}

static void stop_all(void) {
    for (int t = 0; t < NUM_TRACKS; t++) {
        Track *tr = &music.tracks[t];
        if (tr->state == ST_WAITING) tr->state = ST_OFF;
        if (tr->state == ST_PLAYING) tr->state = ST_STOPPING;
    }
}

static void enter_realm(void) {
    for (int t = 0; t < NUM_TRACKS; t++) {
        music.tracks[t].state = ST_OFF;
        music.tracks[t].swap = 0;
        new_loop(t);
    }
    player_x = 238; player_y = 138;
    screen = SCR_MAP;
}

static void leave_realm(void) {
    for (int t = 0; t < NUM_TRACKS; t++) { music.tracks[t].state = ST_OFF; music.tracks[t].swap = 0; }
    realm_choice = music.realm;
    screen = SCR_REALM;
}

// ---- HUD ------------------------------------------------------------------------

static void bar_graph(char *out, float v) {  // 5-character meter
    int n = (int)(v * 5.0f + 0.5f);
    for (int i = 0; i < 5; i++) out[i] = i < n ? '#' : '-';
    out[5] = 0;
}

static void draw_hud(void) {
    fill_rect(0, 0, SCR_W, 24, 0xFF101010);
    text(0, 0, WHITE, "%s  %s minor  %d BPM  Bar %d.%d", realm_names[music.realm],
         key_name(music.root), BPM, music.bar, music.tick / 8 + 1);
    if (music.pending_root >= 0)
        text(40, 0, YELLOW, "-> %s minor next bar", key_name(music.pending_root));
    else if (auto_key)
        text(40, 0, GREY, "Launches %d (+%d)", launches,
             AUTO_KEY_LAUNCHES - launches % AUTO_KEY_LAUNCHES);

    static const char *ftype[NUM_FILTERS] = { "LP", "HP", "BP", "NT" };
    char cut[6], res[6], mix[6], amt[6];
    bar_graph(cut, music.cutoff);
    bar_graph(res, music.resonance);
    bar_graph(mix, music.space_mix);
    bar_graph(amt, music.space_amt);
    u32 rc = (held & PSP_CTRL_RTRIGGER) ? WHITE : GREY;
    u32 lc = (held & PSP_CTRL_LTRIGGER) ? WHITE : GREY;
    text(0, 1, rc, "R %s cut[%s] res[%s]", ftype[music.filter_type], cut, res);
    if (music.space == SPACE_REVERB) text(30, 1, lc, "L verb mix[%s] size[%s]", mix, amt);
    else                             text(30, 1, lc, "L echo mix[%s] fb[%s]", mix, amt);

    if (key_msg_timer > 0) text(0, 2, YELLOW, "KEY CHANGE: now in %s minor", key_name(music.root));
}

// ---- Realm select screen -------------------------------------------------------

static void draw_realm_select(void) {
    const int *bg = realm_bg[realm_choice];
    fill_rect(0, 0, SCR_W, SCR_H, color(bg[0], bg[1], bg[2], 1.0f, 0.0f));
    text(16, 4, WHITE, "WHERE DO YOU WANT TO JAM?");
    for (int i = 0; i < NUM_REALMS; i++) {
        int row = 9 + i * 6;
        int sel = i == realm_choice;
        if (sel) fill_rect(56, row * 8 - 6, 368, 36, color(bg[0], bg[1], bg[2], 1.0f, 0.15f));
        text(9, row, sel ? WHITE : GREY, "%s %s", sel ? ">" : " ", realm_names[i]);
        text(11, row + 1, sel ? WHITE : GREY, "%s", realm_info[i][0]);
        text(11, row + 2, sel ? WHITE : GREY, "%s", realm_info[i][1]);
    }
    text(12, 30, GREY, "Up/Down: choose (listen)   X: enter");
}

// ---- Map screen ---------------------------------------------------------------

static float zone_brightness(int t) {
    int blink = (frame / 10) & 1;
    switch (music.tracks[t].state) {
    case ST_WAITING:  return blink ? 0.80f : 0.40f;
    case ST_PLAYING:  return 0.85f;
    case ST_STOPPING: return blink ? 0.85f : 0.50f;
    default:          return 0.35f;
    }
}

// The loop as little cells: one row per 16 steps (drums: one row per lane).
static void draw_step_strip(int t) {
    const Zone *z = &zones[t];
    int len = track_length(t), rows = len / 16;
    int x0 = z->x + 14, y0 = z->y + z->h - 20;
    int pos = playing(t) ? track_position(t) : -1;
    for (int s = 0; s < len; s++) {
        int row = s / 16, col = s % 16;
        int on = shown_step(t, s) != REST;
        u32 c = on ? color(z->r, z->g, z->b, 1.0f, 0.45f) : color(z->r, z->g, z->b, 0.18f, 0.0f);
        int head = t == TRK_DRUMS ? (col == pos) : (s == pos);
        if (head) c = WHITE;
        fill_rect(x0 + col * 12, y0 + row * (16 / rows), 10, 16 / rows - 1, c);
    }
}

static void draw_pads(int t, int here_row, int here_idx, int here) {
    const Zone *z = &zones[t];
    for (int r = 0; r < pad_rows(t); r++)
        for (int i = 0; i < 3; i++) {
            int x, y, lit;
            pad_rect(t, r, i, &x, &y);
            const char *label;
            char num[2] = { (char)('1' + i), 0 };
            if (t == TRK_DRUMS) { lit = drum_style == i; label = style_symbols[i]; }
            else if (r == 0)    { lit = banks[t][i].filled; label = num; }
            else {
                lit = i == 0 ? music.bass_slide : i == 1 ? music.bass_sub : music.bass_pump;
                label = fx_labels[i];
            }
            fill_rect(x, y, PAD_W, PAD_H, lit ? color(z->r, z->g, z->b, 1.0f, 0.5f)
                                              : color(z->r, z->g, z->b, 0.15f, 0.0f));
            outline_rect(x, y, PAD_W, PAD_H, (here && r == here_row && i == here_idx) ? WHITE
                                             : color(z->r, z->g, z->b, 1.0f, 0.2f));
            int len = (int)strlen(label);
            text((x + (PAD_W - len * 8) / 2) / 8, (y + 4) / 8, lit ? BLACK : WHITE, "%s", label);
        }
}

static void map_help(char *out) {
    int t = zone_at_player(), row, idx;
    if (on_menu_pad()) { strcpy(out, "X: open the menu"); return; }
    if (t < 0) { strcpy(out, "SELECT: new loops everywhere   START: stop all"); return; }
    if (pad_at_player(t, &row, &idx)) {
        if (t == TRK_DRUMS)
            sprintf(out, "X: new beat in style %s", style_symbols[idx]);
        else if (row == 1)
            sprintf(out, "X: %s %s", fx_names[idx],
                    (idx == 0 ? music.bass_slide : idx == 1 ? music.bass_sub : music.bass_pump) ? "off" : "on");
        else if (banks[t][idx].filled)
            sprintf(out, "X: load bank %d   Square: save over bank %d", idx + 1, idx + 1);
        else
            sprintf(out, "X: save this loop to bank %d", idx + 1);
        return;
    }
    strcpy(out, "X: start/stop   Square: new loop   Triangle: edit");
}

static void draw_map(void) {
    const int *bg = realm_bg[music.realm];
    fill_rect(0, 24, SCR_W, SCR_H - 24, color(bg[0], bg[1], bg[2], 1.0f, 0.0f));
    int here = zone_at_player(), prow = -1, pidx = -1;
    int on_pad = here >= 0 && pad_at_player(here, &prow, &pidx);

    for (int t = 0; t < NUM_TRACKS; t++) {
        const Zone *z = &zones[t];
        fill_rect(z->x, z->y, z->w, z->h, color(z->r, z->g, z->b, zone_brightness(t), flash[t] * 0.35f));
        if (t == here && !on_pad) outline_rect(z->x, z->y, z->w, z->h, WHITE);
        int col = z->x / 8 + 1, row = z->y / 8 + 1;
        text(col, row, WHITE, "%s", z->name);
        if (t == TRK_DRUMS) text(col, row + 1, WHITE, "style %s", style_symbols[drum_style]);
        else                text(col, row + 1, WHITE, "%s", wave_name(music.tracks[t].wave));
        text(col, row + 2, WHITE, "%s", state_names[music.tracks[t].state]);
        if (music.tracks[t].swap) text(col, row + 3, YELLOW, "queued");
        draw_pads(t, prow, pidx, t == here && on_pad);
        draw_step_strip(t);
    }

    int menu_here = on_menu_pad();
    fill_rect(MENU_X, MENU_Y, MENU_W, MENU_H, menu_here ? WHITE : GREY);
    text(MENU_X / 8 + 1, MENU_Y / 8 + 1, BLACK, "M");

    fill_rect((int)player_x - 1, (int)player_y - 1, 10, 10, BLACK);
    fill_rect((int)player_x, (int)player_y, 8, 8, WHITE);

    char help[64];
    map_help(help);
    fill_rect(0, 262, SCR_W, 10, 0xFF101010);
    text(1, 33, GREY, "%s", help);
}

// ---- Settings menu (center pad) -----------------------------------------------

enum { MI_SPACE, MI_FILTER, MI_AUTOKEY, MI_KEYUP, MI_KEYRESET, MI_LEAVE, NUM_MENU };
static int menu_sel;

static void draw_menu(void) {
    static const char *space_names[2] = { "Echo", "Reverb" };
    static const char *filter_names[NUM_FILTERS] = { "Low-pass", "High-pass", "Band-pass", "Notch" };
    fill_rect(64, 56, 352, 168, 0xFF181818);
    outline_rect(64, 56, 352, 168, WHITE);
    text(10, 8, WHITE, "SETTINGS");
    char lines[NUM_MENU][48];
    sprintf(lines[MI_SPACE],    "Space effect (L):  < %s >", space_names[music.space]);
    sprintf(lines[MI_FILTER],   "Filter type (R):   < %s >", filter_names[music.filter_type]);
    sprintf(lines[MI_AUTOKEY],  "Auto key change:   < %s >", auto_key ? "every 30" : "off");
    sprintf(lines[MI_KEYUP],    "Move the key up a fifth (now %s minor)", key_name(music.root));
    sprintf(lines[MI_KEYRESET], "Reset the key to A minor");
    sprintf(lines[MI_LEAVE],    "Leave this realm");
    for (int i = 0; i < NUM_MENU; i++)
        text(10, 11 + i * 2, i == menu_sel ? YELLOW : WHITE, "%s %s", i == menu_sel ? ">" : " ", lines[i]);
    text(10, 25, GREY, "Up/Down: choose   Left/Right or X: change");
    text(10, 26, GREY, "Circle: close");
}

static void menu_input(unsigned int pressed) {
    if (pressed & PSP_CTRL_UP)   menu_sel = (menu_sel + NUM_MENU - 1) % NUM_MENU;
    if (pressed & PSP_CTRL_DOWN) menu_sel = (menu_sel + 1) % NUM_MENU;
    int dir = (pressed & PSP_CTRL_LEFT) ? -1 : (pressed & (PSP_CTRL_RIGHT | PSP_CTRL_CROSS)) ? 1 : 0;
    if (pressed & PSP_CTRL_CIRCLE) { screen = SCR_MAP; return; }
    if (!dir) return;
    switch (menu_sel) {
    case MI_SPACE:    music.space ^= 1; break;
    case MI_FILTER:   music.filter_type = (music.filter_type + NUM_FILTERS + dir) % NUM_FILTERS; break;
    case MI_AUTOKEY:  auto_key ^= 1; break;
    case MI_KEYUP:    if (pressed & PSP_CTRL_CROSS) music.pending_root = (music.root + 7) % 12; break;
    case MI_KEYRESET: if (pressed & PSP_CTRL_CROSS) music.pending_root = 0; break;
    case MI_LEAVE:    if (pressed & PSP_CTRL_CROSS) leave_realm(); break;
    }
}

// ---- Loop editor ---------------------------------------------------------------

static int edit_track, cursor, cursor_lane;
static const char *perc_labels[NUM_PERC] = { "Sn", "Cp", "Rm", "Sh" };
static const char *perc_names[NUM_PERC] = { "snare", "clap", "rim", "shaker" };

static void editor_layout(int t, int *rows, int *row_y, int *row_h) {
    int len = track_length(t);
    *rows = len / 16;
    if (*rows == 1) { row_y[0] = 56; *row_h = 112; }
    else if (*rows == 2) { row_y[0] = 56; row_y[1] = 120; *row_h = 52; }
    else { row_y[0] = 56; row_y[1] = 96; row_y[2] = 136; *row_h = 32; }
}

static void draw_editor(void) {
    const Zone *z = &zones[edit_track];
    Track *tr = &music.tracks[edit_track];
    int drums = edit_track == TRK_DRUMS;
    fill_rect(0, 24, SCR_W, SCR_H - 24, color(z->r, z->g, z->b, 0.20f, 0.0f));

    if (drums) text(1, 4, WHITE, "EDITING DRUMS  -  style %s  -  %s", style_symbols[drum_style], state_names[tr->state]);
    else text(1, 4, WHITE, "EDITING %s  -  sound: %s  -  %s", z->name, wave_name(tr->wave), state_names[tr->state]);
    if (tr->swap) text(1, 5, YELLOW, "A new loop starts at the next bar line");

    int rows, row_y[3], row_h;
    editor_layout(edit_track, &rows, row_y, &row_h);
    static const char *row_names[3][3] = {
        { "", "", "" }, { "bar 1", "bar 2", "" }, { "KICK", "HATS", "PERC" } };
    static const char *bell_rows[2] = { "1st", "2nd" };
    int pos = playing(edit_track) ? track_position(edit_track) : -1;

    for (int r = 0; r < rows; r++) {
        if (rows > 1)
            text(0, row_y[r] / 8, GREY, "%s", edit_track == TRK_BELLS ? bell_rows[r] : row_names[rows - 1][r]);
        for (int c = 0; c < 16; c++) {
            int s = r * 16 + c;
            int x = 48 + c * 24, y = row_y[r];
            int sel = drums ? (r == cursor_lane && c == cursor) : s == cursor;
            fill_rect(x, y, 22, row_h, sel ? color(z->r, z->g, z->b, 0.55f, 0.0f)
                                           : color(z->r, z->g, z->b, 0.32f, 0.0f));
            int head = drums ? c == pos : s == pos;
            int v = shown_step(edit_track, s);
            if (v != REST) {
                int h = row_h - 10;
                if (!drums) h = (NOTE(v) + 1) * (row_h - 10) / (MAX_DEGREE + 1);
                fill_rect(x + 3, y + row_h - h, 16, h, color(z->r, z->g, z->b, 1.0f, head ? 0.7f : 0.25f));
                if (drums) {
                    const char *lab = r == LANE_KICK ? "K" : r == LANE_HATS ? (v == HAT_OPEN ? "o" : "x")
                                                                             : perc_labels[v % NUM_PERC];
                    text(x / 8, (y + 16) / 8, BLACK, "%s", lab);
                } else {
                    char lab[4];
                    note_label(v, lab);
                    text(x / 8, y / 8, WHITE, "%s", lab);
                    if (v & SLIDE_FLAG) fill_rect(x + 3, y + row_h - h - 3, 16, 2, WHITE);
                }
            }
            if (sel) outline_rect(x - 1, y - 1, 24, row_h + 2, WHITE);
            if (head) fill_rect(x, y + row_h + 1, 22, 2, WHITE);
            if (c % 4 == 0 && r == 0) fill_rect(x, y - 5, 2, 3, GREY);   // beat marks
        }
    }

    // What's under the cursor.
    int s = drums ? cursor_lane * 16 + cursor : cursor;
    int v = shown_step(edit_track, s);
    if (drums) {
        const char *what = v == REST ? "off" : cursor_lane == LANE_KICK ? "kick"
                         : cursor_lane == LANE_HATS ? (v == HAT_OPEN ? "open hat" : "closed hat")
                         : perc_names[v % NUM_PERC];
        text(1, 23, WHITE, "%s step %d: %s", row_names[2][cursor_lane], cursor + 1, what);
    } else {
        char lab[4];
        note_label(v, lab);
        text(1, 23, WHITE, "Step %d of %d: %s%s", cursor + 1, track_length(edit_track),
             v == REST ? "off" : lab, (v != REST && (v & SLIDE_FLAG)) ? "  (slides in)" : "");
    }

    if (drums) {
        text(1, 26, WHITE, "Left/Right: step   Up/Down: lane   X: hit on/off");
        text(1, 27, WHITE, "Triangle: change the sound (hats, perc)");
    } else {
        text(1, 26, WHITE, "Left/Right: step   Up/Down: note   X: note on/off");
        text(1, 27, WHITE, "Triangle: change the sound");
    }
    text(1, 28, WHITE, "Square: new random loop   START: clear   SELECT: play/stop");
    text(1, 29, WHITE, "Circle: back to the map");
}

static void editor_input(unsigned int pressed) {
    Track *tr = &music.tracks[edit_track];
    int len = track_length(edit_track);
    if (edit_track == TRK_DRUMS) {
        if (pressed & PSP_CTRL_LEFT)  cursor = (cursor + 15) % 16;
        if (pressed & PSP_CTRL_RIGHT) cursor = (cursor + 1) % 16;
        if (pressed & PSP_CTRL_UP)    cursor_lane = (cursor_lane + NUM_LANES - 1) % NUM_LANES;
        if (pressed & PSP_CTRL_DOWN)  cursor_lane = (cursor_lane + 1) % NUM_LANES;
        int s = cursor_lane * 16 + cursor;
        int v = shown_step(TRK_DRUMS, s);
        if (pressed & PSP_CTRL_CROSS) set_step(TRK_DRUMS, s, v == REST ? memory[TRK_DRUMS][s] : REST);
        if ((pressed & PSP_CTRL_TRIANGLE) && cursor_lane != LANE_KICK) {
            int kinds = cursor_lane == LANE_HATS ? 2 : NUM_PERC;
            v = v == REST ? memory[TRK_DRUMS][s] : (v + 1) % kinds;
            set_step(TRK_DRUMS, s, v);
            memory[TRK_DRUMS][s] = v;
        }
    } else {
        if (pressed & PSP_CTRL_LEFT)  cursor = (cursor + len - 1) % len;
        if (pressed & PSP_CTRL_RIGHT) cursor = (cursor + 1) % len;
        if (pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
            int v = shown_step(edit_track, cursor);
            if (v == REST) v = memory[edit_track][cursor];          // turn the step on
            else {
                int n = NOTE(v), flag = v & SLIDE_FLAG;
                if (pressed & PSP_CTRL_UP) n = n < MAX_DEGREE ? n + 1 : MAX_DEGREE;
                else n = n > 0 ? n - 1 : 0;
                v = n | flag;
            }
            set_step(edit_track, cursor, v);
            memory[edit_track][cursor] = v;
        }
        if (pressed & PSP_CTRL_CROSS)
            set_step(edit_track, cursor, shown_step(edit_track, cursor) == REST
                                         ? memory[edit_track][cursor] : REST);
        if (pressed & PSP_CTRL_TRIANGLE) tr->wave = (tr->wave + 1) % NUM_WAVES;
    }
    if (pressed & PSP_CTRL_SQUARE) new_loop(edit_track);
    if (pressed & PSP_CTRL_START) {
        tr->swap = 0;
        for (int s = 0; s < MAX_STEPS; s++) tr->steps[s] = REST;
    }
    if (pressed & PSP_CTRL_SELECT) toggle_track(edit_track);
    if (pressed & PSP_CTRL_CIRCLE) screen = SCR_MAP;
}

// ---- Map input -------------------------------------------------------------------

static void map_input(unsigned int pressed, int dx, int dy) {
    if (!(held & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER))) {
        player_x += dx / 127.0f * 2.5f;
        player_y += dy / 127.0f * 2.5f;
    }
    if (held & PSP_CTRL_UP)    player_y -= 2;     // d-pad works too, for now
    if (held & PSP_CTRL_DOWN)  player_y += 2;
    if (held & PSP_CTRL_LEFT)  player_x -= 2;
    if (held & PSP_CTRL_RIGHT) player_x += 2;
    if (player_x < 0) player_x = 0;
    if (player_x > SCR_W - 8) player_x = SCR_W - 8;
    if (player_y < 26) player_y = 26;
    if (player_y > 252) player_y = 252;

    if (pressed & PSP_CTRL_START) stop_all();
    if (pressed & PSP_CTRL_SELECT) for (int t = 0; t < NUM_TRACKS; t++) new_loop(t);

    if (on_menu_pad()) {
        if (pressed & PSP_CTRL_CROSS) { screen = SCR_MENU; menu_sel = 0; }
        return;
    }
    int t = zone_at_player(), row, idx;
    if (t < 0) return;
    if (pad_at_player(t, &row, &idx)) {
        if (t == TRK_DRUMS) {
            if (pressed & PSP_CTRL_CROSS) { drum_style = idx; new_loop(TRK_DRUMS); }
        } else if (row == 1) {
            if (pressed & PSP_CTRL_CROSS) {
                if (idx == 0) music.bass_slide ^= 1;
                if (idx == 1) music.bass_sub ^= 1;
                if (idx == 2) music.bass_pump ^= 1;
            }
        } else {
            if (pressed & PSP_CTRL_CROSS) {
                if (banks[t][idx].filled) load_bank(t, idx);
                else save_bank(t, idx);
            }
            if (pressed & PSP_CTRL_SQUARE) save_bank(t, idx);
        }
        return;
    }
    if (pressed & PSP_CTRL_CROSS) toggle_track(t);
    if (pressed & PSP_CTRL_SQUARE) new_loop(t);
    if (pressed & PSP_CTRL_TRIANGLE) {
        screen = SCR_EDIT;
        edit_track = t;
        cursor = 0;
        cursor_lane = 0;
    }
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
    scePowerSetClockFrequency(333, 333, 166);   // full speed
    pspDebugScreenInit();
    pspDebugScreenEnableBackColor(0);           // text draws over our graphics
    pspDebugScreenClearLineDisable();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    audio_init();
    gen_seed(sceKernelGetSystemTimeLow());      // different loops every time
    music.realm = realm_choice;
    audio_start();

    int last_root = music.root;
    SceCtrlData pad;
    unsigned int last = 0;

    while (1) {
        sceCtrlPeekBufferPositive(&pad, 1);
        held = pad.Buttons;
        unsigned int pressed = pad.Buttons & ~last;

        // Analog stick: moves you on the map, unless L or R is held.
        //   Hold R: left/right = filter cutoff, up/down = resonance
        //   Hold L: left/right = echo/reverb level, up/down = feedback/size
        int dx = pad.Lx - 128, dy = pad.Ly - 128;
        if (dx > -30 && dx < 30) dx = 0;        // dead zone: ignore small drift
        if (dy > -30 && dy < 30) dy = 0;
        if (screen != SCR_REALM) {
            if (held & PSP_CTRL_RTRIGGER) {
                music.cutoff = clamp01(music.cutoff + dx / 127.0f * 0.012f);
                music.resonance = clamp01(music.resonance - dy / 127.0f * 0.012f);
            }
            if (held & PSP_CTRL_LTRIGGER) {
                music.space_mix = clamp01(music.space_mix + dx / 127.0f * 0.012f);
                music.space_amt = clamp01(music.space_amt - dy / 127.0f * 0.012f);
            }
        }

        switch (screen) {
        case SCR_REALM:
            if (pressed & PSP_CTRL_UP)   realm_choice = (realm_choice + NUM_REALMS - 1) % NUM_REALMS;
            if (pressed & PSP_CTRL_DOWN) realm_choice = (realm_choice + 1) % NUM_REALMS;
            music.realm = realm_choice;          // preview the ambiance
            if (pressed & PSP_CTRL_CROSS) enter_realm();
            break;
        case SCR_MAP:  map_input(pressed, dx, dy); break;
        case SCR_EDIT: editor_input(pressed); break;
        case SCR_MENU: menu_input(pressed); break;
        }

        for (int t = 0; t < NUM_TRACKS; t++) {
            if (music.tracks[t].flash) { flash[t] = 1.0f; music.tracks[t].flash = 0; }
            flash[t] *= 0.85f;
        }
        if (music.root != last_root) { key_msg_timer = 180; last_root = music.root; }
        if (key_msg_timer > 0) key_msg_timer--;

        switch (screen) {
        case SCR_REALM: draw_realm_select(); break;
        case SCR_EDIT:  draw_editor(); draw_hud(); break;
        case SCR_MENU:  draw_map(); draw_hud(); draw_menu(); break;
        default:        draw_map(); draw_hud(); break;
        }
        flip();

        last = pad.Buttons;
        frame++;
    }
    return 0;
}
