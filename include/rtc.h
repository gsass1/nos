#ifndef __RTC_H__
#define __RTC_H__

#include <stdint.h>

// Read the CMOS RTC once and remember the boot wall-clock time; SYS_TIME
// extrapolates from it with the PIT tick counter.
void rtc_init(void);

// Seconds since the Unix epoch (UTC -- QEMU's RTC default), or 0 if the RTC
// read looked invalid at boot.
uint32_t rtc_unix_time(void);

#endif
