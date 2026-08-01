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
USERPROGS=initrd/hello initrd/sh initrd/crash initrd/cat initrd/fbtest initrd/mtest initrd/wm
OBJ=boot/boot.o \
drivers/keyboard.o \
drivers/mouse.o \
drivers/serial.o \
kernel/copy_page_physical.o \
kernel/gdt.o \
kernel/elf.o \
kernel/fb.o \
kernel/idt.o \
kernel/initrd.o \
kernel/interrupt.o \
kernel/isr.o \
kernel/kernel.o \
kernel/kmalloc.o \
kernel/main.o \
kernel/mutex.o \
kernel/paging.o \
kernel/pci.o \
kernel/pic.o \
kernel/pit.o \
kernel/string.o \
kernel/syscall.o \
kernel/task.o \
kernel/vfs.o \
kernel/vga.o \
kernel/vsprintf.o

all: $(BIN)

boot/boot.o: boot/boot.S
	$(AS) boot/boot.S -o boot/boot.o $(AFLAGS)

drivers/keyboard.o: drivers/keyboard.c
	$(CC) -c drivers/keyboard.c -o drivers/keyboard.o $(CFLAGS)

drivers/mouse.o: drivers/mouse.c
	$(CC) -c drivers/mouse.c -o drivers/mouse.o $(CFLAGS)

drivers/serial.o: drivers/serial.c
	$(CC) -c drivers/serial.c -o drivers/serial.o $(CFLAGS)

kernel/copy_page_physical.o: kernel/copy_page_physical.S
	$(AS) kernel/copy_page_physical.S -o kernel/copy_page_physical.o $(AFLAGS)

kernel/gdt.o: kernel/gdt.c
	$(CC) -c kernel/gdt.c -o kernel/gdt.o $(CFLAGS)

kernel/elf.o: kernel/elf.c
	$(CC) -c kernel/elf.c -o kernel/elf.o $(CFLAGS)

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

kernel/paging.o: kernel/paging.c
	$(CC) -c kernel/paging.c -o kernel/paging.o $(CFLAGS)

kernel/pci.o: kernel/pci.c
	$(CC) -c kernel/pci.c -o kernel/pci.o $(CFLAGS)

kernel/pic.o: kernel/pic.c
	$(CC) -c kernel/pic.c -o kernel/pic.o $(CFLAGS)

kernel/pit.o: kernel/pit.c
	$(CC) -c kernel/pit.c -o kernel/pit.o $(CFLAGS)

kernel/string.o: kernel/string.c
	$(CC) -c kernel/string.c -o kernel/string.o $(CFLAGS)

kernel/syscall.o: kernel/syscall.c
	$(CC) -c kernel/syscall.c -o kernel/syscall.o $(CFLAGS)

kernel/task.o: kernel/task.c
	$(CC) -c kernel/task.c -o kernel/task.o $(CFLAGS)

kernel/vfs.o: kernel/vfs.c
	$(CC) -c kernel/vfs.c -o kernel/vfs.o $(CFLAGS)

kernel/vga.o: kernel/vga.c
	$(CC) -c kernel/vga.c -o kernel/vga.o $(CFLAGS)

kernel/vsprintf.o: kernel/vsprintf.c
	$(CC) -c kernel/vsprintf.c -o kernel/vsprintf.o $(CFLAGS)

$(BIN): $(OBJ)
	$(CC) -T linker.ld -o $(BIN) $(CFLAGS) $(OBJ) $(LFLAGS)

iso: $(BIN) $(INITRD)
	cp $(BIN) iso
	cp $(INITRD) iso/initrd.tar
	mkisofs -R -b boot/grub/stage2_eltorito -no-emul-boot -boot-load-size 4 -boot-info-table -o $(ISO) iso

# Build a freestanding user program: compile, then link at the user base
# address (see user/user.ld) into a flat ELF the kernel's elf_exec() loads.
initrd/hello: user/hello.c user/ulib.h user/user.ld include/syscall.h
	$(CC) -c user/hello.c -o user/hello.o $(CFLAGS)
	$(LD) -T user/user.ld user/hello.o -o initrd/hello

initrd/sh: user/sh.c user/ulib.h user/user.ld include/syscall.h
	$(CC) -c user/sh.c -o user/sh.o $(CFLAGS)
	$(LD) -T user/user.ld user/sh.o -o initrd/sh

initrd/crash: user/crash.c user/ulib.h user/user.ld include/syscall.h
	$(CC) -c user/crash.c -o user/crash.o $(CFLAGS)
	$(LD) -T user/user.ld user/crash.o -o initrd/crash

initrd/cat: user/cat.c user/ulib.h user/user.ld include/syscall.h
	$(CC) -c user/cat.c -o user/cat.o $(CFLAGS)
	$(LD) -T user/user.ld user/cat.o -o initrd/cat

initrd/fbtest: user/fbtest.c user/ulib.h user/user.ld include/syscall.h
	$(CC) -c user/fbtest.c -o user/fbtest.o $(CFLAGS)
	$(LD) -T user/user.ld user/fbtest.o -o initrd/fbtest

initrd/mtest: user/mtest.c user/ulib.h user/user.ld include/syscall.h
	$(CC) -c user/mtest.c -o user/mtest.o $(CFLAGS)
	$(LD) -T user/user.ld user/mtest.o -o initrd/mtest

initrd/wm: user/wm.c user/gfx.h user/ulib.h user/user.ld include/syscall.h
	$(CC) -c user/wm.c -o user/wm.o $(CFLAGS)
	$(LD) -T user/user.ld user/wm.o -o initrd/wm

# Regenerate the initrd: an address-sorted symbol table matching the current
# kernel build (so backtraces resolve names) plus the bundled user programs.
$(INITRD): $(BIN) $(USERPROGS)
	$(NM) -n $(BIN) > initrd/symtable
	cd initrd && tar --format ustar -cf initrd.tar symtable hello sh crash cat fbtest mtest wm

initrd: $(INITRD)

# Boot the multiboot kernel directly in QEMU with the initrd as a module
# (no GRUB/ISO needed). Serial (com1) is routed to stdio; Ctrl-A X quits.
run: $(BIN) $(INITRD)
	qemu-system-i386 -kernel $(BIN) -initrd $(INITRD) -serial stdio

# Boot/integration tests: drives the shell in headless QEMU and asserts on
# serial output and the VGA screen. See tests/run_tests.py.
test: $(BIN) $(INITRD)
	python3 tests/run_tests.py

clean:
	rm -f $(OBJ)
	rm -f $(BIN)
	rm -f $(ISO)
	rm -f user/*.o $(USERPROGS)
