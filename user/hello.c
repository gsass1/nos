// A freestanding user program. It knows nothing about the kernel except the
// int 0x80 syscall ABI, so it is loaded and run entirely through elf_exec().
#include "ulib.h"

int main(void)
{
    put("Hello from a loaded ELF program (talking only via syscalls)!\n");
    exit(0);
}
