#include "ulib.h"

void _start(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            put(" ");
        }
        put(argv[i]);
    }
    put("\n");
    exit(0);
}
