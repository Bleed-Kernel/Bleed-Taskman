ELF_TARGET_64 = taskman

CC 	?= x86_64-elf-gcc

ELF_BIN_DIR_64 = bin
ELF_OBJ_DIR_64 = $(ELF_BIN_DIR_64)/obj

LIB_DIR = sysroot/lib
LIBC = $(LIB_DIR)/blibc.a
CRT0 = $(LIB_DIR)/start.o

BLIBC_REPO = https://github.com/Bleed-Kernel/blibc.git
BLIBC_DIR  = external/blibc

COMMON_CFLAGS = \
	-ffreestanding \
	-fno-stack-protector \
	-fno-stack-check \
	-Wall -Wextra -Werror \
	-Wunused-command-line-argument \
	-nostdlib \
	-no-pie \
	-m64 \
	-Iinclude \
	-Isysroot/include \
	-Isysroot/libc \
	-nostdinc

LDFLAGS = \
	-static \
	-nostdlib \
	-no-pie \
	-m64 \
	-Iinclude \
	-L$(LIB_DIR) \
	-Isysroot/include \
	-Isysroot/lib

SRCS = $(shell find src -name '*.c')
ELF_OBJS_64 = $(patsubst src/%.c,$(ELF_OBJ_DIR_64)/%.o,$(SRCS))

all: $(ELF_TARGET_64)

$(ELF_TARGET_64): blibc $(ELF_OBJS_64)
	@mkdir -p $(ELF_BIN_DIR_64)
	$(CC) $(LDFLAGS) \
		-o $(ELF_BIN_DIR_64)/$(ELF_TARGET_64) \
		$(CRT0) \
		$(ELF_OBJS_64) \
		-l:blibc.a

$(ELF_OBJ_DIR_64)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) -c $< -o $@

blibc: $(LIBC)

$(LIBC):
	@echo "[BLIBC] Preparing blibc"
	@if [ ! -d "$(BLIBC_DIR)" ]; then \
		git clone $(BLIBC_REPO) $(BLIBC_DIR); \
	fi
	$(MAKE) -C $(BLIBC_DIR)

	@echo "[BLIBC] Syncing sysroot"
	@mkdir -p sysroot
	@cp -r $(BLIBC_DIR)/sysroot/* sysroot/

build: $(ELF_TARGET_64)

clean:
	rm -rf bin

distclean:
	rm -rf bin sysroot external

.PHONY: all clean distclean blibc