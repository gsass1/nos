// mtest -- mouse smoke test. Polls SYS_MOUSE for a few seconds and reports
// every state change; the test harness injects movement and clicks through
// the QEMU monitor and asserts on these lines.
#include "ulib.h"

int main(void)
{
    struct mouse_state m, last;
    mouse(&last);
    put("mtest: start\n");

    for (int i = 0; i < 400; i++) {
        mouse(&m);
        if (m.x != last.x || m.y != last.y || m.buttons != last.buttons) {
            put("mtest: x=");
            puti(m.x);
            put(" y=");
            puti(m.y);
            put(" b=");
            puti((int)m.buttons);
            put("\n");
            last = m;
        }
        sleep(10);
    }

    put("mtest: done\n");
    exit(0);
}
