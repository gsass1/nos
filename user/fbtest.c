// fbtest -- framebuffer smoke test. Maps the linear framebuffer, draws a
// deterministic four-quadrant pattern (red/green/blue/white), holds it long
// enough for the test harness to screendump and check pixel colors, then
// returns the display to text mode. "fbtest crash" instead faults while
// holding the display, to exercise the kernel's restore-on-death path.
#include "ulib.h"

int main(int argc, char **argv)
{
    struct fb_info info;
    if (fbinfo(&info) != 0) {
        put("fbtest: no framebuffer\n");
        exit(1);
    }

    unsigned int *fb = fbmap();
    if (fb == (unsigned int *)-1) {
        put("fbtest: map failed\n");
        exit(1);
    }

    unsigned w = info.width, h = info.height, stride = info.pitch / 4;
    for (unsigned y = 0; y < h; y++) {
        unsigned int top = y < h / 2;
        for (unsigned x = 0; x < w; x++) {
            unsigned int left = x < w / 2;
            unsigned int color = top ? (left ? 0x00FF0000 : 0x0000FF00)
                                     : (left ? 0x000000FF : 0x00FFFFFF);
            fb[y * stride + x] = color;
        }
    }

    put("fbtest: pattern drawn\n");
    if (argc > 1 && streq(argv[1], "crash")) {
        *(volatile int *)0 = 1; // die owning the display; kernel must restore
    }
    sleep(3000); // window for the harness (or a human) to look at the screen
    fboff();
    put("fbtest: done\n");
    exit(0);
}
