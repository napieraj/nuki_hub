#pragma once

// PCF85063 register <-> UTC epoch conversion, pure (no Arduino, no TZ, no
// mktime/gmtime) so it runs in the host tests: test/test_waveshare_rtc.
// The RTC holds UTC in 24 h mode, years 2000-2099.

#include <stdint.h>

namespace WaveshareRtcLogic
{
    constexpr int64_t TIME_FLOOR = 1767225600;  // 2026-01-01T00:00:00Z
    constexpr int64_t TIME_CEIL = 4102444800;   // 2100-01-01T00:00:00Z (RTC range ends 2099)

    constexpr uint8_t CTRL1_STOP = 0x20;        // Control_1: clock stopped
    constexpr uint8_t CTRL1_12_24 = 0x02;       // Control_1: 12 h mode
    constexpr uint8_t SECONDS_OS = 0x80;        // Seconds: oscillator stopped

    // Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant).
    inline int64_t daysFromCivil(int y, unsigned m, unsigned d)
    {
        y -= m <= 2;
        const int era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return (int64_t)era * 146097 + (int64_t)doe - 719468;
    }

    // Inverse of daysFromCivil.
    inline void civilFromDays(int64_t z, int& y, unsigned& m, unsigned& d)
    {
        z += 719468;
        const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const unsigned doe = (unsigned)(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        d = doy - (153 * mp + 2) / 5 + 1;
        m = mp < 10 ? mp + 3 : mp - 9;
        y = (int)(yoe + era * 400) + (m <= 2);
    }

    inline unsigned daysInMonth(int y, unsigned m)
    {
        static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        return m == 2 && leap ? 29 : days[m - 1];
    }

    inline bool isBcd(uint8_t v)
    {
        return (v & 0x0f) <= 9 && (v >> 4) <= 9;
    }

    inline int fromBcd(uint8_t v)
    {
        return (v >> 4) * 10 + (v & 0x0f);
    }

    inline uint8_t toBcd(int v)
    {
        return (uint8_t)(((v / 10) << 4) | (v % 10));
    }

    enum class RtcStatus
    {
        Ok,
        OscillatorStopped, // OS flag: power was lost without a (charged) backup cell
        ClockStopped,      // Control_1 STOP bit set
        TwelveHourMode,    // Control_1 12_24 set: we only read 24 h mode
        Invalid,           // not BCD, impossible date, or outside 2026..2099
    };

    // ctrl1: register 0x00; t: registers 0x04..0x0A (Seconds..Years).
    inline RtcStatus decode(uint8_t ctrl1, const uint8_t t[7], int64_t& epoch)
    {
        if(t[0] & SECONDS_OS) return RtcStatus::OscillatorStopped;
        if(ctrl1 & CTRL1_STOP) return RtcStatus::ClockStopped;
        if(ctrl1 & CTRL1_12_24) return RtcStatus::TwelveHourMode;

        const uint8_t sec = t[0] & 0x7f, min = t[1] & 0x7f, hour = t[2] & 0x3f;
        const uint8_t day = t[3] & 0x3f, month = t[5] & 0x1f, year = t[6];
        if(!isBcd(sec) || !isBcd(min) || !isBcd(hour) || !isBcd(day) || !isBcd(month) || !isBcd(year))
        {
            return RtcStatus::Invalid;
        }
        const int s = fromBcd(sec), mi = fromBcd(min), h = fromBcd(hour);
        const int d = fromBcd(day), mo = fromBcd(month), y = 2000 + fromBcd(year);
        if(s > 59 || mi > 59 || h > 23 || mo < 1 || mo > 12 || d < 1 || d > (int)daysInMonth(y, (unsigned)mo))
        {
            return RtcStatus::Invalid;
        }
        const int64_t e = daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + s;
        if(e < TIME_FLOOR || e >= TIME_CEIL)
        {
            return RtcStatus::Invalid;
        }
        epoch = e;
        return RtcStatus::Ok;
    }

    // Fills registers 0x04..0x0A for a UTC epoch; false if the RTC can't hold it
    // (or it's before TIME_FLOOR, i.e. the clock isn't set).
    inline bool encode(int64_t epoch, uint8_t t[7])
    {
        if(epoch < TIME_FLOOR || epoch >= TIME_CEIL)
        {
            return false;
        }
        const int64_t days = epoch / 86400;
        const int64_t secs = epoch % 86400;
        int y;
        unsigned m, d;
        civilFromDays(days, y, m, d);
        t[0] = toBcd((int)(secs % 60));      // OS bit 0: writing Seconds clears the flag
        t[1] = toBcd((int)(secs / 60 % 60));
        t[2] = toBcd((int)(secs / 3600));
        t[3] = toBcd((int)d);
        t[4] = (uint8_t)((days + 4) % 7);    // 1970-01-01 was a Thursday (4)
        t[5] = toBcd((int)m);
        t[6] = toBcd(y - 2000);
        return true;
    }
}
