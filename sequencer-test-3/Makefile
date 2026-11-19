TARGET = seqtest3
OBJS = main.o audio.o generate.o

CFLAGS = -O2 -G0 -Wall
ASFLAGS = $(CFLAGS)

LIBS = -lpspaudio -lpspctrl -lpsppower -lm

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Sequencer Test 3

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
