# Headless build for the CH32X035 USB-PD firmware (CI + command line).
#
# Mirrors the MounRiver Studio 2 (MRS 2.4.0) generated build exactly: same
# WCH riscv-none-embed-gcc (GCC 8 fork with the `xw` extension and the
# WCH-Interrupt-fast attribute), same flags, same linker script. MRS2 stays
# the interactive IDE (its own build lives in obj/, gitignored); this
# Makefile exists so CI and IDE-less clones can build into build/.
#
# Toolchain: MRS_Toolchain_Linux_x64_V1.92 — riscv-none-embed-gcc 8.2.
# GitHub mirror used by CI:
#   https://github.com/ch32-riscv-ug/MounRiver_Studio_Community_miror/releases
#
#   make                                   # build/USB-PD.elf + .hex + size
#   make CROSS=/path/to/bin/riscv-none-embed-
#   make clean

CROSS ?= riscv-none-embed-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

BUILD  := build
TARGET := $(BUILD)/USB-PD

ARCHFLAGS := -march=rv32imacxw -mabi=ilp32 -msmall-data-limit=8 -msave-restore
COMMON := $(ARCHFLAGS) -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char \
          -ffunction-sections -fdata-sections -fno-common \
          -Wunused -Wuninitialized -g
INCLUDES := -IDebug -ICore -IUser -IPeripheral/inc
CFLAGS  := $(COMMON) $(INCLUDES) -std=gnu99
ASFLAGS := $(COMMON) -x assembler-with-cpp -IStartup
LDFLAGS := -T Ld/Link.ld -nostartfiles -Xlinker --gc-sections \
          -Wl,-Map,$(TARGET).map --specs=nano.specs --specs=nosys.specs
LIBS    := -lm

C_SRCS := $(wildcard Core/*.c) $(wildcard Debug/*.c) \
          $(wildcard Peripheral/src/*.c) $(wildcard User/*.c)
S_SRCS := $(wildcard Startup/*.S)

OBJS := $(addprefix $(BUILD)/,$(C_SRCS:.c=.o) $(S_SRCS:.S=.o))

all: $(TARGET).hex
	$(SIZE) $(TARGET).elf

$(TARGET).elf: $(OBJS) Ld/Link.ld
	$(CC) $(COMMON) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJS:.o=.d)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
