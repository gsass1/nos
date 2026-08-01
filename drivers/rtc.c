// CMOS real-time clock. Read once at boot; afterwards the wall clock is
// boot time plus the PIT tick counter, so SYS_TIME never touches the (slow,
// torn-read-prone) CMOS registers again.
#include <io.h>
#include <kernel.h>
#include <pit.h>
#include <rtc.h>

MODULE("RTC ");

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define REG_SECONDS 0x00
#define REG_MINUTES 0x02
#define REG_HOURS   0x04
#define REG_DAY     0x07
#define REG_MONTH   0x08
#define REG_YEAR    0x09
#define REG_STATUS_A 0x0A
#define REG_STATUS_B 0x0B

static uint32_t boot_unix;

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_INDEX, reg | 0x80); // keep NMI disabled during the access
    return inb(CMOS_DATA);
}

static int update_in_progress(void)
{
    return cmos_read(REG_STATUS_A) & 0x80;
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm,
// unsigned form; valid for the years an RTC will ever report).
static uint32_t days_from_civil(uint32_t y, uint32_t m, uint32_t d)
{
    y -= m <= 2;
    uint32_t era = y / 400;
    uint32_t yoe = y - era * 400;
    uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void rtc_init(void)
{
    uint8_t sec, min, hour, day, month, year;
    uint8_t sec2, min2, hour2, day2, month2, year2;

    // Read twice around the update flag until both reads agree, so a
    // mid-update rollover can't hand us a torn date.
    do {
        while (update_in_progress()) {
        }
        sec = cmos_read(REG_SECONDS);
        min = cmos_read(REG_MINUTES);
        hour = cmos_read(REG_HOURS);
        day = cmos_read(REG_DAY);
        month = cmos_read(REG_MONTH);
        year = cmos_read(REG_YEAR);
        while (update_in_progress()) {
        }
        sec2 = cmos_read(REG_SECONDS);
        min2 = cmos_read(REG_MINUTES);
        hour2 = cmos_read(REG_HOURS);
        day2 = cmos_read(REG_DAY);
        month2 = cmos_read(REG_MONTH);
        year2 = cmos_read(REG_YEAR);
    } while (sec != sec2 || min != min2 || hour != hour2 ||
             day != day2 || month != month2 || year != year2);

    uint8_t status_b = cmos_read(REG_STATUS_B);
    if (!(status_b & 0x04)) { // BCD mode
        sec = (sec & 0x0F) + (sec >> 4) * 10;
        min = (min & 0x0F) + (min >> 4) * 10;
        hour = ((hour & 0x0F) + ((hour & 0x70) >> 4) * 10) | (hour & 0x80);
        day = (day & 0x0F) + (day >> 4) * 10;
        month = (month & 0x0F) + (month >> 4) * 10;
        year = (year & 0x0F) + (year >> 4) * 10;
    }
    if (!(status_b & 0x02)) { // 12-hour mode; bit 7 of the hour is PM
        uint8_t pm = hour & 0x80;
        hour &= 0x7F;
        if (hour == 12) {
            hour = 0;
        }
        if (pm) {
            hour += 12;
        }
    } else {
        hour &= 0x7F;
    }

    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 59) {
        mprintf(LOGLEVEL_DEFAULT, "Implausible CMOS date; wall clock unset\n");
        return;
    }

    // The century register is unreliable across chipsets; a two-digit year
    // on this OS's hardware (QEMU) is always 20xx.
    boot_unix = days_from_civil(2000 + year, month, day) * 86400
                + (uint32_t)hour * 3600 + (uint32_t)min * 60 + sec;

    kprintf("rtc: %d-%02d-%02d %02d:%02d:%02d UTC (unix %u)\n",
            2000 + year, month, day, hour, min, sec, boot_unix);
}

uint32_t rtc_unix_time(void)
{
    if (!boot_unix) {
        return 0;
    }
    return boot_unix + timer_ticks / PIT_HZ;
}
