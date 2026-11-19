#include <TinyGPSPlus.h>
#include "config.h"
#include "tools.h"
#include "ui.h"

using namespace ui;

static const char* const PORTS[]  = {"EXT G13/G15", "Grove G1/G2"};
static const char* const BAUDS[]  = {"auto", "4800", "9600", "19200", "38400", "57600", "115200"};
static const uint32_t BAUD_VAL[]  = {0, 4800, 9600, 19200, 38400, 57600, 115200};
static const char* const YESNO[]  = {"no", "yes"};

static HardwareSerial& gpsSerial = Serial1;

struct Sat {
    char sys;  // P=GPS L=GLONASS A=Galileo B=BeiDou Q=QZSS
    uint8_t prn;
    int8_t elev;
    int8_t snr;
    uint32_t seen;
};

static bool nmeaChecksumOk(const char* s)
{
    if (*s != '$') return false;
    uint8_t cs = 0;
    const char* p = s + 1;
    while (*p && *p != '*') cs ^= (uint8_t)*p++;
    if (*p != '*' || !isxdigit(p[1]) || !isxdigit(p[2])) return false;
    return cs == (uint8_t)strtoul(p + 1, nullptr, 16);
}

static int splitFields(char* s, char** out, int max)
{
    int n = 0;
    char* star = strchr(s, '*');
    if (star) *star = 0;
    out[n++] = s;
    for (char* p = s; *p && n < max; p++) {
        if (*p == ',') {
            *p       = 0;
            out[n++] = p + 1;
        }
    }
    return n;
}

static char talkerSys(const char* t)
{
    if (!strncmp(t, "GP", 2)) return 'P';
    if (!strncmp(t, "GL", 2)) return 'L';
    if (!strncmp(t, "GA", 2)) return 'A';
    if (!strncmp(t, "GB", 2) || !strncmp(t, "BD", 2)) return 'B';
    if (!strncmp(t, "GQ", 2)) return 'Q';
    return '?';
}

uint32_t gpsDetectBaud(int rx, int tx, bool quick)
{
    static const uint32_t order[] = {9600, 115200, 38400, 57600, 4800, 19200};
    for (int i = 0; i < (quick ? 3 : 6); i++) {
        uint32_t b = order[i];
        busy("GPS", (String("RX G") + rx + ": trying " + b + " baud...").c_str());
        gpsSerial.end();
        gpsSerial.begin(b, SERIAL_8N1, rx, tx);
        uint32_t t0 = millis();
        char line[100];
        int len = 0, good = 0;
        while (millis() - t0 < (quick ? 1200u : 1500u) && good < 2) {
            while (gpsSerial.available()) {
                char c = gpsSerial.read();
                if (c == '$') len = 0;
                if (len < (int)sizeof(line) - 1) line[len++] = c;
                if (c == '\n') {
                    line[len] = 0;
                    while (len && (line[len - 1] == '\r' || line[len - 1] == '\n')) line[--len] = 0;
                    if (nmeaChecksumOk(line)) good++;
                    len = 0;
                }
            }
            delay(2);
        }
        if (good >= 2) return b;
    }
    gpsSerial.end();
    return 0;
}

static void gpsRun(UartPort port, bool swap, uint32_t baud)
{
    int rx, tx;
    uartPins(port, swap, rx, tx);
    if (port == PORT_GROVE) M5.Ex_I2C.release();

    if (!baud) {
        baud = gpsDetectBaud(rx, tx, false);
        if (!baud) {
            String m = String("No valid NMEA on RX=G") + rx + " TX=G" + tx +
                       "\nat any common baud rate.\n\n- check GND/VCC\n- try Swap RX/TX\n- module may output UBX only";
            message("GPS: no data", m.c_str(), BAD);
            gpsSerial.end();
            return;
        }
    } else {
        gpsSerial.end();
        gpsSerial.begin(baud, SERIAL_8N1, rx, tx);
    }

    TinyGPSPlus gps;
    constexpr int MAX_SATS = 48, RAW = 11;
    Sat sats[MAX_SATS];
    int nsats = 0;
    int inView[5] = {0};  // P L A B Q
    int fixType = 0;
    String raw[RAW];
    int rawHead = 0;
    char line[120];
    int len = 0;
    uint32_t start = millis(), ttff = 0, lastByte = 0, bytes = 0;
    int page = 0;
    bool pauseRaw = false;
    const char* sysChars = "PLABQ";

    for (;;) {
        while (gpsSerial.available()) {
            char c = gpsSerial.read();
            gps.encode(c);
            bytes++;
            lastByte = millis();
            if (c == '$') len = 0;
            if (c == '\r') continue;
            if (c != '\n') {
                if (len < (int)sizeof(line) - 1) line[len++] = c;
                continue;
            }
            line[len] = 0;
            len = 0;
            if (line[0] != '$') continue;
            if (!pauseRaw) {
                raw[rawHead] = line;
                rawHead = (rawHead + 1) % RAW;
            }
            if (!nmeaChecksumOk(line) || strlen(line) < 7) continue;
            char work[120];
            strcpy(work, line);
            char* fl[24];
            int nf = splitFields(work, fl, 24);
            const char* type = fl[0] + 3;
            char sys = talkerSys(fl[0] + 1);
            if (!strcmp(type, "GSV") && nf >= 4) {
                const char* p = strchr(sysChars, sys);
                if (p) inView[p - sysChars] = atoi(fl[3]);
                for (int i = 4; i + 3 < nf; i += 4) {
                    if (!*fl[i]) continue;
                    uint8_t prn = atoi(fl[i]);
                    int slot = -1;
                    for (int s = 0; s < nsats; s++)
                        if (sats[s].sys == sys && sats[s].prn == prn) slot = s;
                    if (slot < 0 && nsats < MAX_SATS) slot = nsats++;
                    if (slot < 0) continue;
                    sats[slot] = {sys, prn, (int8_t)atoi(fl[i + 1]), (int8_t)(*fl[i + 3] ? atoi(fl[i + 3]) : -1), millis()};
                }
            } else if (!strcmp(type, "GSA") && nf >= 3) {
                fixType = atoi(fl[2]);
            }
        }
        // expire sats not reported for 6 s
        for (int s = 0; s < nsats;) {
            if (millis() - sats[s].seen > 6000) sats[s] = sats[--nsats];
            else s++;
        }
        if (!ttff && gps.location.isValid() && gps.location.age() < 2000) ttff = millis() - start;

        const char* titles[] = {"GPS fix", "GPS satellites", "GPS raw NMEA"};
        frame(titles[page], page == 2 ? "TAB page  p pause  ` back" : "TAB page  r reset TTFF  ` back");
        bool alive = millis() - lastByte < 2000;
        textRight(W - 40, 3, (String(baud) + (alive ? "" : " NO DATA")).c_str(), alive ? DIM : BAD);

        if (page == 0) {
            int y = TOP;
            const char* ft = !gps.location.isValid() ? "NO FIX" : fixType == 3 ? "3D FIX" : fixType == 2 ? "2D FIX" : "FIX";
            canvas.setFont(&fonts::Font2);
            canvas.setTextColor(gps.location.isValid() ? GOOD : WARN);
            canvas.setCursor(4, y);
            canvas.print(ft);
            canvas.setTextColor(FG);
            canvas.setCursor(84, y);
            canvas.printf("sats %lu/%d", gps.satellites.isValid() ? (unsigned long)gps.satellites.value() : 0UL,
                          inView[0] + inView[1] + inView[2] + inView[3] + inView[4]);
            canvas.setFont(&fonts::Font0);
            y += 18;
            if (gps.location.isValid()) {
                textf(4, y, FG, "lat %11.6f  lon %11.6f", gps.location.lat(), gps.location.lng());
            } else {
                text(4, y, "lat ---          lon ---", DIM);
            }
            y += LINE;
            textf(4, y, FG, "alt %s m   hdop %s",
                  gps.altitude.isValid() ? String(gps.altitude.meters(), 1).c_str() : "--",
                  gps.hdop.isValid() ? String(gps.hdop.hdop(), 2).c_str() : "--");
            y += LINE;
            textf(4, y, FG, "spd %s km/h  crs %s",
                  gps.speed.isValid() ? String(gps.speed.kmph(), 1).c_str() : "--",
                  gps.course.isValid() ? String(gps.course.deg(), 0).c_str() : "--");
            y += LINE;
            if (gps.date.isValid() && gps.time.isValid())
                textf(4, y, FG, "UTC %04d-%02d-%02d %02d:%02d:%02d", gps.date.year(), gps.date.month(),
                      gps.date.day(), gps.time.hour(), gps.time.minute(), gps.time.second());
            else
                text(4, y, "UTC --", DIM);
            y += LINE + 3;
            if (ttff) textf(4, y, GOOD, "TTFF %.1f s", ttff / 1000.0f);
            else textf(4, y, WARN, "searching %lu s", (unsigned long)((millis() - start) / 1000));
            textf(110, y, DIM, "fix age %s", gps.location.isValid() ? (String(gps.location.age()) + " ms").c_str() : "--");
            y += LINE;
            textf(4, y, DIM, "bytes %lu  ok %lu  bad %lu", (unsigned long)bytes,
                  (unsigned long)gps.passedChecksum(), (unsigned long)gps.failedChecksum());
            y += LINE;
            textf(4, y, DIM, "RX G%d  TX G%d  %s", rx, tx, PORTS[port]);
        } else if (page == 1) {
            textf(4, TOP, DIM, "GPS %d  GLO %d  GAL %d  BDS %d  QZS %d", inView[0], inView[1], inView[2], inView[3], inView[4]);
            // sort by SNR desc for display
            int order[MAX_SATS];
            for (int i = 0; i < nsats; i++) order[i] = i;
            std::sort(order, order + nsats, [&](int a, int b) { return sats[a].snr > sats[b].snr; });
            int shown = min(nsats, 24);
            int colW  = shown ? min(19, (W - 8) / max(shown, 1)) : 19;
            int base  = BOTTOM - 12, maxH = 72;
            for (int i = 0; i < shown; i++) {
                const Sat& s = sats[order[i]];
                int x = 4 + i * colW;
                int h = s.snr > 0 ? constrain(s.snr * maxH / 50, 1, maxH) : 1;
                uint16_t c = s.snr >= 35 ? GOOD : s.snr >= 25 ? WARN : s.snr > 0 ? BAD : PANEL;
                canvas.fillRect(x, base - h, colW - 3, h, c);
                if (s.snr > 0 && colW >= 14) textf(x, base - h - 9, FG, "%d", s.snr);
                char lbl[4];
                snprintf(lbl, sizeof(lbl), "%c%d", s.sys, s.prn % 100);
                if (colW >= 14) text(x, base + 2, lbl, DIM);
            }
            if (!nsats) text(4, TOP + 40, "no GSV data yet", DIM);
        } else {
            int y = TOP;
            for (int i = 0; i < RAW; i++) {
                const String& s = raw[(rawHead + i) % RAW];
                if (!s.length()) continue;
                String shown = s.substring(0, 39);
                text(1, y, shown.c_str(), nmeaChecksumOk(s.c_str()) ? FG : BAD);
                y += LINE;
            }
            if (pauseRaw) textRight(W - 4, BOTTOM - 8, "PAUSED", ACCENT);
        }
        push();

        Key k = poll();
        if (k.type == K_BACK) break;
        if (k.type == K_TAB || k.type == K_RIGHT) page = (page + 1) % 3;
        if (k.type == K_LEFT) page = (page + 2) % 3;
        if (k.is('r')) { start = millis(); ttff = 0; }
        if (k.is('p')) pauseRaw = !pauseRaw;
        delay(15);
    }
    gpsSerial.end();
}

void toolGps()
{
    static int port = PORT_EXT, baud = 0, swap = 0, sel = 3;
    Field f[] = {
        Field::choice("Port", &port, PORTS, 2),
        Field::choice("Baud", &baud, BAUDS, 7),
        Field::choice("Swap RX/TX", &swap, YESNO, 2),
        Field::action("Start"),
    };
    for (;;) {
        if (form("GPS / GNSS", f, 4, sel) < 0) return;
        gpsRun((UartPort)port, swap, BAUD_VAL[baud]);
    }
}
