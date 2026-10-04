TARGET := firmware
CC := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy
BUILD := build

CFLAGS := -mcpu=cortex-m3 -mthumb -Wall -Wextra -Werror -O2 -ffreestanding -fno-common -fno-builtin -nostdlib -nostartfiles -ffunction-sections -fdata-sections
LDFLAGS := -T linker.ld -nostdlib -nostartfiles -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map

OBJECTS := $(BUILD)/main.o $(BUILD)/startup.o

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin

$(BUILD):
	mkdir -p $@

$(BUILD)/main.o: main.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/startup.o: startup.s | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJECTS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJECTS) -o $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

flash: $(BUILD)/$(TARGET).bin
	@command -v st-flash >/dev/null 2>&1 || { echo "st-flash not found. Install STM32CubeProgrammer or stlink tools."; exit 1; }
	st-flash write $< 0x08000000

clean:
	rm -rf $(BUILD)

.PHONY: all flash clean
