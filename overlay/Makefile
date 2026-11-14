#---------------------------------------------------------------------------------
# SwitchSplash overlay (libultrahand). Based on libultrahand's example Makefile.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

APP_TITLE	:= SwitchSplash
APP_AUTHOR	:= SwitchSplash
APP_VERSION	:= 1.0.0
TARGET		:= switchsplash
BUILD		:= build
SOURCES		:= source
INCLUDES	:= source
NO_ICON		:= 1

include $(TOPDIR)/lib/libultrahand/ultrahand.mk

#---------------------------------------------------------------------------------
# code generation
#---------------------------------------------------------------------------------
ARCH := -march=armv8-a+simd+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS := -g -Wall -O2 -ffunction-sections -fdata-sections -flto \
          -fuse-linker-plugin -fomit-frame-pointer -finline-small-functions \
          -fno-strict-aliasing -frename-registers -falign-functions=16 \
          $(ARCH) $(DEFINES)

CFLAGS += $(INCLUDE) -D__SWITCH__ -DAPP_VERSION="\"$(APP_VERSION)\"" -D_FORTIFY_SOURCE=2

# libultrahand options
UI_OVERRIDE_PATH := /config/bootsplash/overlay/
CFLAGS += -DUI_OVERRIDE_PATH="\"$(UI_OVERRIDE_PATH)\""
CFLAGS += -DUSE_EXCEPTION_WRAP=1
LDFLAGS += -Wl,-wrap,__cxa_throw -Wl,-wrap,_Unwind_Resume -Wl,-wrap,__gxx_personality_v0

CXXFLAGS := $(CFLAGS) -std=c++26 -Wno-dangling-else -ffast-math -fno-unwind-tables -fno-asynchronous-unwind-tables
CXXFLAGS += -fno-exceptions -ffunction-sections -fdata-sections -fno-rtti

ASFLAGS := $(ARCH)
LDFLAGS += -specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) -Wl,-Map,$(notdir $*.map)
LDFLAGS += -Wl,--as-needed -Wl,--gc-sections

LIBS := -lcurl -lz -lminizip -lmbedtls -lmbedx509 -lmbedcrypto -lnx

CXXFLAGS += -flto=4
LDFLAGS  += -flto=4

LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(TARGET).ovl $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all : $(OUTPUT).ovl

$(OUTPUT).ovl: $(OUTPUT).elf $(OUTPUT).nacp
	@elf2nro $< $@ $(NROFLAGS)
	@echo "built ... $(notdir $(OUTPUT).ovl)"
	@printf 'ULTR' >> $@
	@printf "Ultrahand signature has been added.\n"

$(OUTPUT).elf: $(OFILES)

$(OFILES_SRC): $(HFILES_BIN)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
