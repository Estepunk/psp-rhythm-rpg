TARGET = seqtest
OBJS = main.o audio.o

CFLAGS = -O2 -G0 -Wall
ASFLAGS = $(CFLAGS)

LIBS = -lpspaudio -lpspctrl -lm

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Sequencer Test

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
