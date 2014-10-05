AS=i686-elf-as
AFLAGS=-g
CC=i686-elf-gcc
CFLAGS=-I./include/ -std=gnu99 -ffreestanding -nostdlib -g -Wall -Wextra
LFLAGS=-lgcc
BIN=kernel.elf
ISO=gianos.iso
OBJ=boot/boot.o \
drivers/keyboard.o \
drivers/serial.o \
kernel/copy_page_physical.o \
kernel/gdt.o \
kernel/idt.o \
kernel/initrd.o \
kernel/interrupt.o \
kernel/isr.o \
kernel/kernel.o \
kernel/kmalloc.o \
kernel/main.o \
kernel/mutex.o \
kernel/paging.o \
kernel/pic.o \
kernel/pit.o \
kernel/string.o \
kernel/task.o \
kernel/vfs.o \
kernel/vga.o \
kernel/vsprintf.o

all: $(BIN)

boot/boot.o: boot/boot.S
	$(AS) boot/boot.S -o boot/boot.o $(AFLAGS)

drivers/keyboard.o: drivers/keyboard.c
	$(CC) -c drivers/keyboard.c -o drivers/keyboard.o $(CFLAGS)

drivers/serial.o: drivers/serial.c
	$(CC) -c drivers/serial.c -o drivers/serial.o $(CFLAGS)

kernel/copy_page_physical.o: kernel/copy_page_physical.S
	$(AS) kernel/copy_page_physical.S -o kernel/copy_page_physical.o $(AFLAGS)

kernel/gdt.o: kernel/gdt.c
	$(CC) -c kernel/gdt.c -o kernel/gdt.o $(CFLAGS)

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

kernel/pic.o: kernel/pic.c
	$(CC) -c kernel/pic.c -o kernel/pic.o $(CFLAGS)

kernel/pit.o: kernel/pit.c
	$(CC) -c kernel/pit.c -o kernel/pit.o $(CFLAGS)

kernel/string.o: kernel/string.c
	$(CC) -c kernel/string.c -o kernel/string.o $(CFLAGS)

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

iso: $(BIN)
	cp $(BIN) iso
	nm $(BIN) > symtable
	cp symtable iso
	mkisofs -R -b boot/grub/stage2_eltorito -no-emul-boot -boot-load-size 4 -boot-info-table -o $(ISO) iso

clean:
	rm -f $(OBJ)
	rm -f $(BIN)
	rm -f $(ISO)
