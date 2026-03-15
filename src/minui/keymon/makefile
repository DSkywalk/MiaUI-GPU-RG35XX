#keymon

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

include ../../platform/makefile.env
SDL?=SDL2

TARGET = keymon
LIBDIR = -L/opt/rg35xx/arm-buildroot-linux-gnueabihf/sysroot/usr/lib -L/opt/rg35xx/lib


CC = $(CROSS_COMPILE)gcc
CFLAGS	= -Os -lmsettings -lpthread -lrt -ldl -Wl,--gc-sections -s -DUSE_$(SDL)
CFLAGS  += -I. -I../../common/ -I../../platform/ -I/opt/rg35xx/include -I/opt/rg35xx/arm-buildroot-linux-gnueabihf/sysroot/usr/include/SDL2 -DPLATFORM=\"$(UNION_PLATFORM)\"
LDFLAGS	 = $(LIBDIR) -ldl -lmsettings

all:
	$(CC) $(TARGET).c -o $(TARGET).elf $(CFLAGS) $(LDFLAGS)
clean:
	rm -rf $(TARGET).elf

send: all
	/root/workspace/tools/adb push $(TARGET).elf /opt/minui/bin/$(TARGET).elf
	/root/workspace/tools/adb shell chmod +x /opt/minui/bin/$(TARGET).elf
	/root/workspace/tools/adb kill-server