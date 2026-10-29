#---------------------------------------------------------------------------------
# Minecraft3D.3gx - Luma3DS game plugin for Minecraft: New Nintendo 3DS Edition (USA, 00040000001B8700)
#
# Built against CTRPluginFramework (libctrpf) + 3gxtool, both kept OUTSIDE
# C:\devkitPro under  <projects>/toolchain/  (see BUILDING.md).
#
# NOTE: devkitPro's make rules break on paths containing spaces. Do not run
# `make` directly from a folder whose path contains spaces; use build.ps1 or
# build.sh, which go through a space-free junction in %TEMP%.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

export TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

# Toolchain locations (override on the command line if you move them)
TOOLCHAIN	?=	$(abspath $(TOPDIR)/../../toolchain)
CTRPFLIB	?=	$(TOOLCHAIN)/libctrpf
GXTOOL		?=	$(TOOLCHAIN)/3gxtool-1.3/3gxtool.exe

TARGET		:=	Minecraft3D
SOURCES		:=	source
INCLUDES	:=	include
PSF			:=	$(TARGET).plgInfo

#---------------------------------------------------------------------------------
# code generation
#---------------------------------------------------------------------------------
ARCH		:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS		:=	-Wall -Wextra -Wno-unused-parameter -mword-relocations \
				-ffunction-sections -fdata-sections -fno-strict-aliasing \
				$(ARCH) $(BUILD_CFLAGS) $(G)
CFLAGS		+=	$(INCLUDE) -D__3DS__
CXXFLAGS	:=	$(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++20
ASFLAGS		:=	$(ARCH) $(G)
LDFLAGS		:=	-T $(TOPDIR)/3gx.ld $(ARCH) -Os \
				-Wl,$(WL)--gc-sections,--section-start,.text=0x07000100

LIBS		:=	$(BUILD_LIBS) -lm
LIBDIRS		:=	$(CTRPFLIB) $(CTRULIB) $(PORTLIBS)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD		:=	$(CXX)
export OFILES	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all release debug clean

all: release

release: | build/release
	@$(MAKE) BUILD=release OUTPUT=$(CURDIR)/$(TARGET).3gx \
		BUILD_LIBS="-lctrpf -lctru" WL=--strip-discarded,--strip-debug, \
		BUILD_CFLAGS="-DNDEBUG=1 -O2 -fomit-frame-pointer" \
		DEPSDIR=$(CURDIR)/build/release --no-print-directory \
		-C build/release -f $(CURDIR)/Makefile

debug: | build/debug
	@$(MAKE) BUILD=debug OUTPUT=$(CURDIR)/$(TARGET)-debug.3gx \
		BUILD_LIBS="-lctrpfd -lctrud" BUILD_CFLAGS="-DDEBUG=1 -Og" G=-g \
		DEPSDIR=$(CURDIR)/build/debug --no-print-directory \
		-C build/debug -f $(CURDIR)/Makefile

build/release build/debug:
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -rf build *.elf *.3gx

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT) : $(basename $(OUTPUT)).elf
$(basename $(OUTPUT)).elf : $(OFILES)

%.3gx: %.elf
	@echo creating $(notdir $@)
	@"$(GXTOOL)" -s $< $(TOPDIR)/$(PSF) $@

-include $(DEPENDS)
#---------------------------------------------------------------------------------
endif
