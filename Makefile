# Cross toolchain. TOOLPREFIX?= so the environment/CI can point at a different
# prefix; `make CC=... AS=...` on the command line also overrides (and `?=`
# would NOT work for CC/AS/LD -- make predefines those to host tools).
TOOLPREFIX?=i686-elf-
AS=$(TOOLPREFIX)as
AFLAGS=-g
CC=$(TOOLPREFIX)gcc
LD=$(TOOLPREFIX)ld
CFLAGS=-I./include/ -std=gnu99 -ffreestanding -nostdlib -g -Wall -Wextra
LFLAGS=-lgcc
NM=$(TOOLPREFIX)nm

# CI builds with `make WERROR=1`: the tree is warning-clean and must stay so.
ifeq ($(WERROR),1)
CFLAGS+=-Werror
endif
BIN=kernel.elf
ISO=gianos.iso
INITRD=initrd/initrd.tar

# Freestanding userspace programs bundled into the initrd. They talk to the
# kernel only through the int 0x80 syscall ABI (include/syscall.h).
USERPROGS=initrd/hello initrd/sh initrd/crash initrd/cat initrd/echo initrd/grep initrd/wc initrd/fbtest initrd/mtest initrd/wm initrd/spin initrd/upper initrd/badptr initrd/disktest initrd/mkdir initrd/libctest initrd/wget initrd/browser
OBJ=boot/boot.o \
	drivers/keyboard.o \
	drivers/mouse.o \
	drivers/serial.o \
	drivers/ata.o \
	drivers/rtc.o \
	drivers/rtl8139.o \
	kernel/block.o \
	kernel/console.o \
kernel/copy_page_physical.o \
kernel/gdt.o \
kernel/elf.o \
kernel/ext2.o \
kernel/fb.o \
kernel/idt.o \
kernel/initrd.o \
kernel/interrupt.o \
kernel/isr.o \
kernel/kernel.o \
kernel/kmalloc.o \
kernel/main.o \
kernel/mutex.o \
kernel/net.o \
kernel/paging.o \
kernel/pci.o \
kernel/pic.o \
kernel/pipe.o \
kernel/pit.o \
kernel/string.o \
kernel/syscall.o \
kernel/task.o \
kernel/tcp.o \
kernel/vfs.o \
kernel/vga.o \
kernel/vsprintf.o \
kernel/wsurf.o

# BearSSL (third_party/bearssl, git submodule), built for userland against the
# freestanding shim libc in user/libc. The x86 SIMD implementations are
# excluded -- their intrinsics headers require a hosted libc -- and disabled
# through the BR_* macros so BearSSL's portable constant-time code is used.
# -O2 matters: TLS handshakes do real bignum math on an emulated i386.
# Objects go to build/bearssl so the submodule checkout stays clean.
BEARSSL_DIR=third_party/bearssl
ifeq ($(wildcard $(BEARSSL_DIR)/src),)
$(error $(BEARSSL_DIR) is empty; run: git submodule update --init)
endif
BEARSSL_BUILD=build/bearssl
BEARSSL_SRC=$(filter-out %/ghash_pclmul.c %/chacha20_sse2.c %/sysrng.c \
    $(wildcard $(BEARSSL_DIR)/src/symcipher/aes_x86ni*.c), \
    $(wildcard $(BEARSSL_DIR)/src/*.c $(BEARSSL_DIR)/src/*/*.c))
BEARSSL_OBJ=$(patsubst $(BEARSSL_DIR)/src/%.c,$(BEARSSL_BUILD)/%.o,$(BEARSSL_SRC))
# The BR_USE_* pins disable BearSSL's host-OS auto-detection: a Linux-target
# cross compiler (CI's i686-linux-gnu-gcc) defines __unix__/__linux__, which
# would pull in <time.h>/urandom paths that don't exist on NOS. wget provides
# the validation time (SYS_TIME) and entropy explicitly.
BEARSSL_CFLAGS=-I$(BEARSSL_DIR)/inc -I$(BEARSSL_DIR)/src -Iuser/libc \
    -std=gnu99 -ffreestanding -O2 -g \
    -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_RDRAND=0 \
    -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0 \
    -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 -DBR_USE_WIN32_RAND=0
BEARSSL_LIB=$(BEARSSL_BUILD)/libbearssl.a
AR=$(TOOLPREFIX)ar

all: $(BIN)

$(BEARSSL_BUILD)/%.o: $(BEARSSL_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(BEARSSL_CFLAGS)

$(BEARSSL_LIB): $(BEARSSL_OBJ)
	$(AR) rcs $@ $(BEARSSL_OBJ)

# The user-side libc, linked into every user program: crt0 (_start -> main),
# string.h in libc.c, malloc/strtol in stdlib.c, printf in stdio.c. Plain
# objects rather than an archive so link order against BearSSL never matters.
ULIBC=user/libc/crt0.o user/libc/libc.o user/libc/stdlib.o user/libc/stdio.o
ULIBC_DEPS=$(ULIBC) user/ulib.h user/user.ld include/syscall.h
# -Iuser/libc comes first so the user string.h/stdio.h shadow the kernel's
# headers in include/ (which stays on the path for syscall.h/stdint.h).
UCFLAGS=-Iuser/libc $(CFLAGS)

user/libc/crt0.o: user/libc/crt0.c user/ulib.h include/syscall.h
	$(CC) -c user/libc/crt0.c -o user/libc/crt0.o $(UCFLAGS)

user/libc/libc.o: user/libc/libc.c user/libc/string.h user/libc/stdlib.h
	$(CC) -c user/libc/libc.c -o user/libc/libc.o $(UCFLAGS)

user/libc/stdlib.o: user/libc/stdlib.c user/libc/stdlib.h user/libc/string.h user/libc/limits.h user/ulib.h include/syscall.h
	$(CC) -c user/libc/stdlib.c -o user/libc/stdlib.o $(UCFLAGS)

user/libc/stdio.o: user/libc/stdio.c user/libc/stdio.h user/libc/string.h user/ulib.h include/syscall.h
	$(CC) -c user/libc/stdio.c -o user/libc/stdio.o $(UCFLAGS)

boot/boot.o: boot/boot.S
	$(AS) boot/boot.S -o boot/boot.o $(AFLAGS)

drivers/keyboard.o: drivers/keyboard.c
	$(CC) -c drivers/keyboard.c -o drivers/keyboard.o $(CFLAGS)

drivers/mouse.o: drivers/mouse.c
	$(CC) -c drivers/mouse.c -o drivers/mouse.o $(CFLAGS)

drivers/serial.o: drivers/serial.c
	$(CC) -c drivers/serial.c -o drivers/serial.o $(CFLAGS)

drivers/ata.o: drivers/ata.c
	$(CC) -c drivers/ata.c -o drivers/ata.o $(CFLAGS)

drivers/rtc.o: drivers/rtc.c
	$(CC) -c drivers/rtc.c -o drivers/rtc.o $(CFLAGS)

drivers/rtl8139.o: drivers/rtl8139.c
	$(CC) -c drivers/rtl8139.c -o drivers/rtl8139.o $(CFLAGS)

kernel/block.o: kernel/block.c
	$(CC) -c kernel/block.c -o kernel/block.o $(CFLAGS)

kernel/console.o: kernel/console.c
	$(CC) -c kernel/console.c -o kernel/console.o $(CFLAGS)

kernel/copy_page_physical.o: kernel/copy_page_physical.S
	$(AS) kernel/copy_page_physical.S -o kernel/copy_page_physical.o $(AFLAGS)

kernel/gdt.o: kernel/gdt.c
	$(CC) -c kernel/gdt.c -o kernel/gdt.o $(CFLAGS)

kernel/elf.o: kernel/elf.c
	$(CC) -c kernel/elf.c -o kernel/elf.o $(CFLAGS)

kernel/ext2.o: kernel/ext2.c
	$(CC) -c kernel/ext2.c -o kernel/ext2.o $(CFLAGS)

kernel/fb.o: kernel/fb.c
	$(CC) -c kernel/fb.c -o kernel/fb.o $(CFLAGS)

kernel/idt.o: kernel/idt.c
	$(CC) -c kernel/idt.c -o kernel/idt.o $(CFLAGS)

kernel/initrd.o: kernel/initrd.c
	$(CC) -c kernel/initrd.c -o kernel/initrd.o $(CFLAGS)

kernel/interrupt.o: kernel/interrupt.S
	$(AS) kernel/interrupt.S -o kernel/interrupt.o

kernel/isr.o: kernel/isr.c
	$(CC) -c kernel/isr.c -o kernel/isr.o $(CFLAGS)

kernel/kernel.o: kernel/kernel.c
	$(CC) -c kernel/kernel.c -o kernel/kernel.o $(CFLAGS)

kernel/kmalloc.o: kernel/kmalloc.c
	$(CC) -c kernel/kmalloc.c -o kernel/kmalloc.o $(CFLAGS)

kernel/main.o: kernel/main.c
	$(CC) -c kernel/main.c -o kernel/main.o $(CFLAGS)

kernel/mutex.o: kernel/mutex.c
	$(CC) -c kernel/mutex.c -o kernel/mutex.o $(CFLAGS)

kernel/net.o: kernel/net.c
	$(CC) -c kernel/net.c -o kernel/net.o $(CFLAGS)

kernel/paging.o: kernel/paging.c
	$(CC) -c kernel/paging.c -o kernel/paging.o $(CFLAGS)

kernel/pci.o: kernel/pci.c
	$(CC) -c kernel/pci.c -o kernel/pci.o $(CFLAGS)

kernel/pic.o: kernel/pic.c
	$(CC) -c kernel/pic.c -o kernel/pic.o $(CFLAGS)

kernel/pipe.o: kernel/pipe.c
	$(CC) -c kernel/pipe.c -o kernel/pipe.o $(CFLAGS)

kernel/pit.o: kernel/pit.c
	$(CC) -c kernel/pit.c -o kernel/pit.o $(CFLAGS)

kernel/string.o: kernel/string.c
	$(CC) -c kernel/string.c -o kernel/string.o $(CFLAGS)

kernel/syscall.o: kernel/syscall.c
	$(CC) -c kernel/syscall.c -o kernel/syscall.o $(CFLAGS)

kernel/task.o: kernel/task.c
	$(CC) -c kernel/task.c -o kernel/task.o $(CFLAGS)

kernel/tcp.o: kernel/tcp.c
	$(CC) -c kernel/tcp.c -o kernel/tcp.o $(CFLAGS)

kernel/vfs.o: kernel/vfs.c
	$(CC) -c kernel/vfs.c -o kernel/vfs.o $(CFLAGS)

kernel/vga.o: kernel/vga.c
	$(CC) -c kernel/vga.c -o kernel/vga.o $(CFLAGS)

kernel/vsprintf.o: kernel/vsprintf.c
	$(CC) -c kernel/vsprintf.c -o kernel/vsprintf.o $(CFLAGS)

kernel/wsurf.o: kernel/wsurf.c
	$(CC) -c kernel/wsurf.c -o kernel/wsurf.o $(CFLAGS)

$(BIN): $(OBJ)
	$(CC) -T linker.ld -o $(BIN) $(CFLAGS) $(OBJ) $(LFLAGS)

iso: $(BIN) $(INITRD)
	cp $(BIN) iso
	cp $(INITRD) iso/initrd.tar
	mkisofs -R -b boot/grub/stage2_eltorito -no-emul-boot -boot-load-size 4 -boot-info-table -o $(ISO) iso

# Build a freestanding user program: compile, then link at the user base
# address (see user/user.ld) into a flat ELF the kernel's elf_exec() loads.
$(USERPROGS): | initrd-dir

.PHONY: initrd-dir
initrd-dir:
	mkdir -p initrd

initrd/hello: user/hello.c $(ULIBC_DEPS)
	$(CC) -c user/hello.c -o user/hello.o $(UCFLAGS)
	$(LD) -T user/user.ld user/hello.o $(ULIBC) -o initrd/hello

initrd/sh: user/sh.c $(ULIBC_DEPS)
	$(CC) -c user/sh.c -o user/sh.o $(UCFLAGS)
	$(LD) -T user/user.ld user/sh.o $(ULIBC) -o initrd/sh

initrd/crash: user/crash.c $(ULIBC_DEPS)
	$(CC) -c user/crash.c -o user/crash.o $(UCFLAGS)
	$(LD) -T user/user.ld user/crash.o $(ULIBC) -o initrd/crash

initrd/cat: user/cat.c $(ULIBC_DEPS)
	$(CC) -c user/cat.c -o user/cat.o $(UCFLAGS)
	$(LD) -T user/user.ld user/cat.o $(ULIBC) -o initrd/cat

initrd/echo: user/echo.c $(ULIBC_DEPS)
	$(CC) -c user/echo.c -o user/echo.o $(UCFLAGS)
	$(LD) -T user/user.ld user/echo.o $(ULIBC) -o initrd/echo

initrd/grep: user/grep.c $(ULIBC_DEPS)
	$(CC) -c user/grep.c -o user/grep.o $(UCFLAGS)
	$(LD) -T user/user.ld user/grep.o $(ULIBC) -o initrd/grep

initrd/wc: user/wc.c $(ULIBC_DEPS)
	$(CC) -c user/wc.c -o user/wc.o $(UCFLAGS)
	$(LD) -T user/user.ld user/wc.o $(ULIBC) -o initrd/wc

initrd/fbtest: user/fbtest.c $(ULIBC_DEPS)
	$(CC) -c user/fbtest.c -o user/fbtest.o $(UCFLAGS)
	$(LD) -T user/user.ld user/fbtest.o $(ULIBC) -o initrd/fbtest

initrd/mtest: user/mtest.c $(ULIBC_DEPS)
	$(CC) -c user/mtest.c -o user/mtest.o $(UCFLAGS)
	$(LD) -T user/user.ld user/mtest.o $(ULIBC) -o initrd/mtest

initrd/wm: user/wm.c user/gfx.h $(ULIBC_DEPS)
	$(CC) -c user/wm.c -o user/wm.o $(UCFLAGS)
	$(LD) -T user/user.ld user/wm.o $(ULIBC) -o initrd/wm

initrd/spin: user/spin.c $(ULIBC_DEPS)
	$(CC) -c user/spin.c -o user/spin.o $(UCFLAGS)
	$(LD) -T user/user.ld user/spin.o $(ULIBC) -o initrd/spin

initrd/upper: user/upper.c $(ULIBC_DEPS)
	$(CC) -c user/upper.c -o user/upper.o $(UCFLAGS)
	$(LD) -T user/user.ld user/upper.o $(ULIBC) -o initrd/upper

initrd/badptr: user/badptr.c $(ULIBC_DEPS)
	$(CC) -c user/badptr.c -o user/badptr.o $(UCFLAGS)
	$(LD) -T user/user.ld user/badptr.o $(ULIBC) -o initrd/badptr

initrd/disktest: user/disktest.c $(ULIBC_DEPS)
	$(CC) -c user/disktest.c -o user/disktest.o $(UCFLAGS)
	$(LD) -T user/user.ld user/disktest.o $(ULIBC) -o initrd/disktest

initrd/mkdir: user/mkdir.c $(ULIBC_DEPS)
	$(CC) -c user/mkdir.c -o user/mkdir.o $(UCFLAGS)
	$(LD) -T user/user.ld user/mkdir.o $(ULIBC) -o initrd/mkdir

initrd/libctest: user/libctest.c user/libc/stdio.h user/libc/stdlib.h user/libc/string.h $(ULIBC_DEPS)
	$(CC) -c user/libctest.c -o user/libctest.o $(UCFLAGS)
	$(LD) -T user/user.ld user/libctest.o $(ULIBC) -o initrd/libctest

# wget links BearSSL for https; the libc satisfies BearSSL's imports
# (and gcc's own memcpy/memset emissions).
initrd/wget: user/wget.c user/trust_anchors.h $(ULIBC_DEPS) $(BEARSSL_LIB)
	$(CC) -c user/wget.c -o user/wget.o $(UCFLAGS) -I$(BEARSSL_DIR)/inc
	$(LD) -T user/user.ld user/wget.o $(ULIBC) -o initrd/wget $(BEARSSL_LIB)

# browser renders HTML on the framebuffer; it fetches like wget, so it links
# BearSSL the same way.
initrd/browser: user/browser.c user/gfx.h user/trust_anchors.h $(ULIBC_DEPS) $(BEARSSL_LIB)
	$(CC) -c user/browser.c -o user/browser.o $(UCFLAGS) -I$(BEARSSL_DIR)/inc
	$(LD) -T user/user.ld user/browser.o $(ULIBC) -o initrd/browser $(BEARSSL_LIB)

# Regenerate the initrd: an address-sorted symbol table matching the current
# kernel build (so backtraces resolve names) plus the bundled user programs.
$(INITRD): $(BIN) $(USERPROGS)
	$(NM) -n $(BIN) > initrd/symtable
	cd initrd && tar --format ustar -cf initrd.tar symtable hello sh crash cat echo grep wc fbtest mtest wm spin upper badptr disktest mkdir libctest wget browser

initrd: $(INITRD)

# Boot the multiboot kernel directly in QEMU with the initrd as a module
# (no GRUB/ISO needed). Serial (com1) is routed to stdio; Ctrl-A X quits.
run: $(BIN) $(INITRD)
	qemu-system-i386 -kernel $(BIN) -initrd $(INITRD) -serial stdio

# Same, plus an RTL8139 on user-mode networking: `wget google.com` works from
# the shell (DNS via slirp at 10.0.2.3, outbound TCP proxied by the host).
run-net: $(BIN) $(INITRD)
	qemu-system-i386 -kernel $(BIN) -initrd $(INITRD) -serial stdio \
		-netdev user,id=n0 -device rtl8139,netdev=n0

# Boot/integration tests: drives the shell in headless QEMU and asserts on
# serial output and the VGA screen. See tests/run_tests.py.
test: $(BIN) $(INITRD)
	python3 tests/run_tests.py
	python3 tests/run_ata_tests.py
	python3 tests/run_ext2_tests.py
	python3 tests/run_net_tests.py

test-ata: $(BIN) $(INITRD)
	python3 tests/run_ata_tests.py

test-ext2: $(BIN) $(INITRD)
	python3 tests/run_ext2_tests.py

test-net: $(BIN) $(INITRD)
	python3 tests/run_net_tests.py

clean:
	rm -f $(OBJ)
	rm -f $(BIN)
	rm -f $(ISO)
	rm -f user/*.o user/libc/*.o
	rm -rf $(BEARSSL_BUILD)
	rm -rf initrd
