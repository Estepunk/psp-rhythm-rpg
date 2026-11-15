// Hello World for the PSP.
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>

PSP_MODULE_INFO("Hello World", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

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

int main(void) {
    setup_callbacks();
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    pspDebugScreenPrintf("Hello, World!\n\n");
    pspDebugScreenPrintf("Press X to say hi. Press HOME to quit.\n\n");

    SceCtrlData pad;
    unsigned int last = 0;
    while (1) {
        sceCtrlReadBufferPositive(&pad, 1);
        if ((pad.Buttons & PSP_CTRL_CROSS) && !(last & PSP_CTRL_CROSS)) {
            pspDebugScreenPrintf("hi! ");
        }
        last = pad.Buttons;
        sceKernelDelayThread(16 * 1000);  // ~60 checks per second
    }
    return 0;
}
