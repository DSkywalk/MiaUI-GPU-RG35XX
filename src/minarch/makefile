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
# debug -O0 -g
###########################################################

TARGET = minarch
INCDIR = -I. -I./libretro-common/include/ -I../common/ -I../platform/ -I/opt/rg35xx/arm-buildroot-linux-gnueabihf/sysroot/usr/include/SDL2/ -I/opt/rg35xx/include
LIBDIR = -L/opt/rg35xx/arm-buildroot-linux-gnueabihf/sysroot/usr/lib -L/opt/rg35xx/lib

SOURCE = $(TARGET).c ../common/utils.c ../common/api.c ../platform/platform.c

CC = $(CROSS_COMPILE)gcc
CFLAGS   = $(ARCH) -fomit-frame-pointer -DGL_VERTEX_SHADER -DSTBI_FAILURE_USERMSG
CFLAGS  += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -DUSE_$(SDL) -Ofast -std=gnu99
minCFLAGS	+= -flto=4
LDFLAGS	 = $(LIBDIR) -lmsettings -l$(SDL) -l$(SDL)_image -l$(SDL)_ttf -lEGL -lGLESv2 -lGLESv1_CM -lGLES_CM -lPVROGL_MESA -lpthread -lm -lz
# CFLAGS  += -Wall -Wno-unused-variable -Wno-unused-function -Wno-format-overflow
# CFLAGS  += -fsanitize=address -fno-common
# LDFLAGS += -lasan

BUILD_DATE!=date +%Y.%m.%d
BUILD_HASH!=cat ../../hash.txt
CFLAGS += -DBUILD_DATE=\"${BUILD_DATE}\" -DBUILD_HASH=\"${BUILD_HASH}\"

# CFLAGS += -DBUILD_DATE=\"20230326\" -DBUILD_HASH=\"d34db33f\"

PRODUCT= build/$(PLATFORM)/$(TARGET).elf

all: libretro-common $(PREFIX)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
clean:
	rm -f $(PRODUCT)

libretro-common:
	git clone https://github.com/libretro/libretro-common
	
$(PREFIX)/include/msettings.h:
	cd ../libmsettings && make

send: all
	/root/workspace/tools/adb push $(PRODUCT) /opt/minui/bin/minarch.elf
	/root/workspace/tools/adb shell chmod +x /opt/minui/bin/minarch.elf
	/root/workspace/tools/adb kill-server
