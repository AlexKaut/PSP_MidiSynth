TARGET = User
OBJS = main.o kernel.o

CFLAGS = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = -lpspaudio -lpspkubridge -lpsppower

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSP Synth
PSP_EBOOT_ICON = ICON.png

PSP_EBOOT_COMMENT = PSP midi synth, a special adapter is needed for connection. Made by Alexkaut.
PSP_EBOOT_VERSION = 1.00

PSPSDK := $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

# ===== targets =====

all: EBOOT.PBP install

install:
	mkdir -p ../PSP_MidiSynth
	cp EBOOT.PBP ../PSP_MidiSynth/

clean:
	rm -f *.o *.elf EBOOT.PBP
