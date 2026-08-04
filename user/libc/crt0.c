// crt0: the process entry point, linked into every user program. elf_exec()
// lays out the initial user stack as [fake return][argc][argv], so _start
// receives them as plain cdecl arguments and hands them to main(); returning
// from main() exits with its value.
#include "../ulib.h"

int main(int argc, char **argv);

void _start(int argc, char **argv)
{
    exit(main(argc, argv));
}
