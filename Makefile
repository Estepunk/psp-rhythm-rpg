TARGET = rhythm_rpg
OBJS = main.o

CFLAGS = -O2 -Wall
ASFLAGS = $(CFLAGS)

LIBS = -lpspctrl

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSP Rhythm RPG

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
