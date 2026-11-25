ARCH    = x86_64
CC      = gcc
LD      = ld
OBJCOPY = objcopy

BUILD   = build
FORMAT  = pei-x86-64
OBJS    = $(BUILD)/mbp133_m1b_igpu.o
TARGET  = $(BUILD)/mbp133_m1b_igpu.efi
SO      = $(BUILD)/mbp133_m1b_igpu.so
OBJ     = $(BUILD)/mbp133_m1b_igpu.o

INC     = /usr/include/efi
LIBDIR  = /usr/lib

CFLAGS  = -I$(INC) -I$(INC)/$(ARCH) \
          -DGNU_EFI_USE_MS_ABI -fPIC -fshort-wchar -ffreestanding \
          -fno-stack-protector -fno-strict-aliasing -maccumulate-outgoing-args \
          -Wall -Wextra -Werror -D$(ARCH) -m64 -mno-red-zone -O2

LDFLAGS = -T $(LIBDIR)/elf_$(ARCH)_efi.lds -Bsymbolic -shared -nostdlib -znocombreloc -z defs \
          $(LIBDIR)/crt0-efi-$(ARCH).o

all: $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

$(OBJ): mbp133_m1b_igpu.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(SO): $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^ $(shell $(CC) $(CFLAGS) -print-libgcc-file-name) $(LIBDIR)/libgnuefi.a

$(TARGET): $(SO)
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym -j .rel -j .rela -j .reloc -S --output-target=$(FORMAT) --subsystem=10 $^ $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
