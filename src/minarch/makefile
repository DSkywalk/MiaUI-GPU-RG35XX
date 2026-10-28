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

###########################################################
# rcheevos - offline RetroAchievements
#
# Only the runtime (trigger evaluation), the patch-response parser, the
# libretro memory mapper and the rom hasher are built. rc_client and
# everything it drags in (networking, badge downloads) is left out, which is
# the whole point: the device has no connection.
###########################################################

RC_DIR     = rcheevos
RC_SRC     = $(RC_DIR)/src/src
RC_INC     = $(RC_DIR)/src/include
RC_VERSION = 683197d8dc92555a51e8280a4123d071ed66bff3

RC_SOURCE = \
	$(RC_SRC)/runtime/rc_alloc.c \
	$(RC_SRC)/runtime/rc_condition.c \
	$(RC_SRC)/runtime/rc_condset.c \
	$(RC_SRC)/runtime/rc_consoleinfo.c \
	$(RC_SRC)/runtime/rc_format.c \
	$(RC_SRC)/runtime/rc_lboard.c \
	$(RC_SRC)/runtime/rc_memref.c \
	$(RC_SRC)/runtime/rc_operand.c \
	$(RC_SRC)/runtime/rc_richpresence.c \
	$(RC_SRC)/runtime/rc_runtime.c \
	$(RC_SRC)/runtime/rc_trigger.c \
	$(RC_SRC)/runtime/rc_value.c \
	$(RC_SRC)/util/rc_compat.c \
	$(RC_SRC)/util/rc_util.c \
	$(RC_SRC)/util/rc_version.c \
	$(RC_SRC)/util/md5.c \
	$(RC_SRC)/api/rc_api_common.c \
	$(RC_SRC)/api/rc_api_runtime.c \
	$(RC_SRC)/libretro/rc_libretro.c \
	$(RC_SRC)/hash/rc_hash.c \
	$(RC_SRC)/hash/rc_hash_rom.c \
	$(RC_SRC)/hash/rc_hash_zip.c \
	$(RC_SRC)/hash/rc_hash_disc.c \
	$(RC_SRC)/hash/rc_cdreader.c

RC_INCDIR = -I$(RC_INC) -I$(RC_SRC) -I$(RC_SRC)/libretro
# no lua, no encrypted (3ds/psp) images: dead weight here.
# disc support IS built, PS (bin/cue, iso, chd) and MDCD need it to hash at all.
RC_CFLAGS = -DRC_DISABLE_LUA -DRC_HASH_NO_ENCRYPTED

###########################################################
# libchdr - so PS games stored as .chd can be hashed too
#
# unity.c is a single translation unit pulling in libchdr plus its bundled
# lzma/miniz/zstd; avhuff is referenced by chd.c's codec table but unity.c
# leaves it out, so it's listed separately.
#
# LOWRAM_TARGET trades CPU for RAM in libchdr. Hashing happens once at load
# and a full CD's hunk map would otherwise cost megabytes, so the trade is
# entirely in our favour here.
#
# Vendored in-tree rather than referenced from ../../../nextui: only workspace/
# is mounted into the build container, so anything outside it doesn't exist at
# build time.
###########################################################

CHD_DIR = libchdr

CHD_SOURCE = \
	$(CHD_DIR)/unity.c \
	$(CHD_DIR)/src/libchdr_codec_avhuff.c

CHD_INCDIR = -I$(CHD_DIR)/include -I$(CHD_DIR) \
	-I$(CHD_DIR)/deps/lzma-26.02/include \
	-I$(CHD_DIR)/deps/miniz-3.1.2 \
	-I$(CHD_DIR)/deps/zstd-1.5.7
CHD_CFLAGS = -DRA_HAS_CHD -DLOWRAM_TARGET=1

###########################################################

INCDIR = -I. -I./libretro-common/include/ -I../common/ -I../platform/ -I/opt/rg35xx/arm-buildroot-linux-gnueabihf/sysroot/usr/include/SDL2/ -I/opt/rg35xx/include
INCDIR += $(RC_INCDIR) $(CHD_INCDIR)
LIBDIR = -L/opt/rg35xx/arm-buildroot-linux-gnueabihf/sysroot/usr/lib -L/opt/rg35xx/lib

SOURCE = $(TARGET).c ra_offline.c chd_reader.c ../common/utils.c ../common/api.c ../common/i18n.c ../common/profile.c ../platform/platform.c
SOURCE += $(RC_SOURCE) $(CHD_SOURCE)

CC = $(CROSS_COMPILE)gcc
CFLAGS   = $(ARCH) -fomit-frame-pointer -DGL_VERTEX_SHADER -DSTBI_FAILURE_USERMSG
CFLAGS  += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -DUSE_$(SDL) -Ofast -std=gnu99
CFLAGS  += $(RC_CFLAGS) $(CHD_CFLAGS)

###########################################################
# Frame profiler. Off by default and compiled out entirely; only ever enable it
# for this target, since platform.c is shared with minui.
#
#   make PROFILE=1 send              normal pipeline, vsync as configured
#   make PROFILE=1 PROFILE_VSYNC=0   vsync off, so frame time isn't pinned to 16.7ms
#   make PROFILE=1 PROFILE_GLFINISH=1 attribute gpu time (kills pipelining)
###########################################################
PROFILE ?= 0
PROFILE_FRAMES ?= 300
PROFILE_VSYNC ?= 1
PROFILE_GLFINISH ?= 0

ifeq ($(PROFILE),1)
CFLAGS += -DPROFILE=1 -DPROFILE_FRAMES=$(PROFILE_FRAMES)
CFLAGS += -DPROFILE_GLFINISH=$(PROFILE_GLFINISH)
ifeq ($(PROFILE_VSYNC),0)
CFLAGS += -DPROFILE_FORCE_NO_VSYNC=1
endif
endif
# Link-time optimisation across the frontend, rcheevos and libchdr (it was
# `minCFLAGS += -flto=4` before, a typo, so it never applied).
# With it GCC inlines rc_runtime_destroy() into ra_offline.c, where the runtime
# is a static, and flags its `if (self->owns_self) free(self)`: a branch that is
# never taken, rc_runtime_init() having set owns_self to 0. rcheevos is a
# pinned clone rather than ours to patch, so that one warning is silenced here.
CFLAGS	+= -flto=4 -Wno-free-nonheap-object
LDFLAGS	 = $(LIBDIR) -lmsettings -l$(SDL) -l$(SDL)_image -l$(SDL)_ttf -lEGL -lGLESv2 -lGLESv1_CM -lGLES_CM -lPVROGL_MESA -lpthread -lm -lz
# CFLAGS  += -Wall -Wno-unused-variable -Wno-unused-function -Wno-format-overflow
# CFLAGS  += -fsanitize=address -fno-common
# LDFLAGS += -lasan

BUILD_DATE!=date +%Y.%m.%d
BUILD_HASH!=cat ../../hash.txt
CFLAGS += -DBUILD_DATE=\"${BUILD_DATE}\" -DBUILD_HASH=\"${BUILD_HASH}\"

# CFLAGS += -DBUILD_DATE=\"20230326\" -DBUILD_HASH=\"d34db33f\"

PRODUCT= build/$(PLATFORM)/$(TARGET).elf

.PHONY: all clean send bench bench-log bench-save bench-off

all: libretro-common rcheevos $(PREFIX)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
clean:
	rm -f $(PRODUCT)

libretro-common:
	git clone https://github.com/libretro/libretro-common

rcheevos:
	git clone https://github.com/RetroAchievements/rcheevos.git $(RC_DIR)/src
	cd $(RC_DIR)/src && git checkout $(RC_VERSION)
	
$(PREFIX)/include/msettings.h:
	cd ../libmsettings && make

ADB ?= /root/workspace/tools/adb
PROFILE_LOG ?= /userdata/minarch_profile.log

send: all
	$(ADB) push $(PRODUCT) /opt/minui/bin/minarch.elf
	$(ADB) shell chmod +x /opt/minui/bin/minarch.elf
	$(ADB) kill-server

# build an instrumented binary, clear the old log and deploy it
bench:
	rm -f $(PRODUCT)
	$(MAKE) all PROFILE=1 PROFILE_FRAMES=$(PROFILE_FRAMES) \
		PROFILE_VSYNC=$(PROFILE_VSYNC) PROFILE_GLFINISH=$(PROFILE_GLFINISH)
	$(ADB) shell rm -f $(PROFILE_LOG)
	$(ADB) push $(PRODUCT) /opt/minui/bin/minarch.elf
	$(ADB) shell chmod +x /opt/minui/bin/minarch.elf
	$(ADB) kill-server
	@echo ""
	@echo "Instrumented build is on the device. Launch a game, play ~30s, quit,"
	@echo "then run:  make bench-log"

# read the reports back off the device
bench-log:
	@$(ADB) shell cat $(PROFILE_LOG)
	$(ADB) kill-server

bench-save:
	@mkdir -p bench
	@$(ADB) shell cat $(PROFILE_LOG) > bench/$(NAME).log
	@echo "saved bench/$(NAME).log"
	$(ADB) kill-server

# back to a clean, uninstrumented binary
bench-off:
	rm -f $(PRODUCT)
	$(MAKE) all
	$(ADB) push $(PRODUCT) /opt/minui/bin/minarch.elf
	$(ADB) shell chmod +x /opt/minui/bin/minarch.elf
	$(ADB) kill-server

