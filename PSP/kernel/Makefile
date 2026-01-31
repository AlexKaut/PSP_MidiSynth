TARGET = kernel
OBJS = kernel.o exports.o

PRX_EXPORTS = exports.exp

USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1

CFLAGS = -Os -G0 -Wall -fno-builtin-printf
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = -lpsphprm_driver
LDFLAGS = -nostartfiles

PSPSDK := $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak



all: $(TARGET).prx install

exports:
	psp-build-exports -s $(PRX_EXPORTS)

install: exports
	mkdir -p ../PSP_MidiSynth
	cp $(TARGET).prx ../PSP_MidiSynth/
	cp $(TARGET).S ../user/

clean:
	rm -f *.o *.prx *.elf *.S
