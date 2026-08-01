// mkdir -- create a directory on the ext2 filesystem.
//
// Usage: mkdir disk/path
#include "ulib.h"

void _start(int argc, char **argv)
{
    if (argc < 2) {
        put("usage: mkdir <path>\n");
        exit(1);
    }
    if (mkdir(argv[1]) < 0) {
        put("mkdir: cannot create ");
        put(argv[1]);
        put("\n");
        exit(1);
    }
    exit(0);
}
