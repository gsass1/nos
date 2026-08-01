// upper -- copy stdin to stdout, uppercased. The canonical pipeline
// consumer: it reads until EOF, so it only exits once every write end of its
// stdin has closed (the other stage exiting, or being killed).
#include "ulib.h"

void _start(void)
{
    char buf[512];
    int n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] >= 'a' && buf[i] <= 'z') {
                buf[i] -= 'a' - 'A';
            }
        }
        write(1, buf, n);
    }
    exit(0);
}
