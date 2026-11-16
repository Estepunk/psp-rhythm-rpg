// Hello World for the PSP.
// D-pad changes the screen color. X says a random greeting. HOME quits.
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <stdlib.h>

PSP_MODULE_INFO("Hello World", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

// PSP colors are written 0xAABBGGRR (alpha, blue, green, red).
typedef struct {
    const char *name;
    unsigned int back;
    unsigned int text;
} Theme;

static const Theme themes[] = {
    { "Midnight", 0xFF000000, 0xFFFFFFFF },  // starting theme
    { "Ocean",    0xFF8B3A00, 0xFFFFFF80 },  // UP
    { "Forest",   0xFF1E5A1E, 0xFFC0FFC0 },  // DOWN
    { "Sunset",   0xFF2050D0, 0xFF80F0FF },  // LEFT
    { "Grape",    0xFF701E5A, 0xFFFFC0F0 },  // RIGHT
};

static const char *greetings[] = {
    "Hello!",
    "Hola!",
    "Hey there!",
    "What's good?",
    "Bonjour!",
    "Yo!",
    "Ready to rock?",
    "Konnichiwa!",
    "Wassup!",
    "Ciao!",
    "Greetings, traveler!",
    "Let's jam!",
};
#define NUM_GREETINGS (sizeof(greetings) / sizeof(greetings[0]))

// Lets the HOME button quit back to the PSP menu.
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

// Clears the screen in the current theme and redraws everything.
static void draw_screen(int theme, const char *greeting, int count) {
    pspDebugScreenSetBackColor(themes[theme].back);
    pspDebugScreenSetTextColor(themes[theme].text);
    pspDebugScreenClear();

    pspDebugScreenPrintf("Hello, World!\n\n");
    pspDebugScreenPrintf("D-pad: change color   X: say hi   HOME: quit\n\n");
    pspDebugScreenPrintf("Theme: %s\n\n", themes[theme].name);
    if (greeting) {
        pspDebugScreenPrintf("  %s\n\n", greeting);
        pspDebugScreenPrintf("(greetings so far: %d)\n", count);
    }
}

// True only on the frame a button goes from up to down.
#define PRESSED(btn) ((pad.Buttons & (btn)) && !(last & (btn)))

int main(void) {
    setup_callbacks();
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    srand(sceKernelGetSystemTimeLow());  // different greetings each run

    int theme = 0;
    int greeting = -1;  // none yet
    int count = 0;
    draw_screen(theme, NULL, count);

    SceCtrlData pad;
    unsigned int last = 0;
    while (1) {
        sceCtrlReadBufferPositive(&pad, 1);
        int redraw = 0;

        if (PRESSED(PSP_CTRL_UP))    { theme = 1; redraw = 1; }
        if (PRESSED(PSP_CTRL_DOWN))  { theme = 2; redraw = 1; }
        if (PRESSED(PSP_CTRL_LEFT))  { theme = 3; redraw = 1; }
        if (PRESSED(PSP_CTRL_RIGHT)) { theme = 4; redraw = 1; }

        if (PRESSED(PSP_CTRL_CROSS)) {
            int next;
            do {
                next = rand() % NUM_GREETINGS;
            } while (next == greeting);  // never the same one twice in a row
            greeting = next;
            count++;
            redraw = 1;
        }

        if (redraw) {
            draw_screen(theme, greeting >= 0 ? greetings[greeting] : NULL, count);
        }
        last = pad.Buttons;
        sceKernelDelayThread(16 * 1000);  // ~60 checks per second
    }
    return 0;
}
