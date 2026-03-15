###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
	$(error please specify PLATFORM, eg. PLATFORM=trimui make)
endif

ifeq (,$(CROSS_COMPILE))
	$(error missing CROSS_COMPILE for this toolchain)
endif

###########################################################

include ../platform/makefile.env
SDL?=SDL2
DEBUG=0

###########################################################

TARGET = minui
INCDIR = -I. -I../common/ -I../platform/ -I/opt/rg35xx/arm-buildroot-linux-gnueabihf/sysroot/usr/include/SDL2/ -I/opt/rg35xx/include 
LIBDIR = -L/opt/rg35xx/arm-buildroot-linux-gnueabihf/sysroot/usr/lib -L/opt/rg35xx/lib
SOURCE = $(TARGET).c ../common/utils.c ../common/api.c ../platform/platform.c

CC = $(CROSS_COMPILE)gcc
CFLAGS   = $(ARCH) -fomit-frame-pointer
CFLAGS  += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -DUSE_$(SDL) -std=gnu99
ifeq ($(DEBUG), 1)
	CFLAGS += -O0 -g
	CXXFLAGS += -O0 -g
else 
	CFLAGS += -Os
	CXXFLAGS += -Os
endif

LDFLAGS	 = $(LIBDIR) -ldl -lmsettings $(LIBS) -l$(SDL) -l$(SDL)_image -l$(SDL)_ttf -lEGL -lGLESv2 -lGLESv1_CM -lGLES_CM -lPVROGL_MESA -lpthread -lm -lz
# CFLAGS  += -Wall -Wno-unused-variable -Wno-unused-function -Wno-format-overflow
# CFLAGS  += -fsanitize=address -fno-common
# LDFLAGS += -lasan

PRODUCT= build/$(PLATFORM)/$(TARGET).elf

all: $(PREFIX)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
clean:
	rm -f $(PRODUCT)

$(PREFIX)/include/msettings.h:
	cd /root/workspace/libmsettings && make

send: all
	/root/workspace/tools/adb push $(PRODUCT) /opt/minui/bin/$(TARGET).elf
	/root/workspace/tools/adb shell chmod +x /opt/minui/bin/$(TARGET).elf
	/root/workspace/tools/adb kill-server
