###########################################################

PLATFORM=rg35xx

ifeq (,$(PLATFORM))
	$(error please specify PLATFORM, eg. PLATFORM=trimui make)
endif

ifeq (,$(CROSS_COMPILE))
	$(error missing CROSS_COMPILE for this toolchain)
endif

###########################################################

include ../../platform/makefile.env
SDL?=SDL2

###########################################################

TARGET = clock
INCDIR = -I. -I../../common/ -I../../platform/ -I/opt/rg35xx/include
LIBDIR = -L/opt/rg35xx/arm-buildroot-linux-gnueabihf/sysroot/usr/lib -L/opt/rg35xx/lib
SOURCE = $(TARGET).c ../../common/utils.c ../../common/api.c ../../platform/platform.c

CC = $(CROSS_COMPILE)gcc
CFLAGS   = $(ARCH) -fomit-frame-pointer
CFLAGS  += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -DUSE_$(SDL)  -Os 
LDFLAGS	 = -ldl $(LIBS) -l$(SDL) -l$(SDL)_image -l$(SDL)_ttf -lpthread -lm -lz
LDFLAGS	 = $(LIBDIR) -ldl -lmsettings $(LIBS) -l$(SDL) -l$(SDL)_image -l$(SDL)_ttf -lEGL -lGLESv2 -lGLESv1_CM -lGLES_CM -lPVROGL_MESA -lpthread -lm -lz

PRODUCT= build/$(PLATFORM)/$(TARGET).elf

all: $(PREFIX)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
clean:
	rm -f $(PRODUCT)

$(PREFIX)/include/msettings.h:
	cd /root/workspace/$(PLATFORM)/libmsettings && make