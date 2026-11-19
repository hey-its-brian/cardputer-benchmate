#include "config.h"
#include "radio.h"
#include "tools.h"
#include "ui.h"

using namespace ui;

struct Finding {
    String text;
    uint16_t color;
};

static void add(std::vector<Finding>& out, uint16_t color, const String& s)
{
    out.push_back({s, color});
}

static void probeRadios(std::vector<Finding>& out)
{
    const PinConfig& p = g.pins;
    bool any = false;

    busy("Auto detect", "SPI: probing SX126x...");
    {
        Module* m = radioModule(p.irq, p.rst, p.aux);
        SX1262 r(m);
        int16_t st = r.begin(915.0, 125.0, 9, 7, 0x12, 10, 8, 1.8f);
        if (st != RADIOLIB_ERR_NONE && st != RADIOLIB_ERR_CHIP_NOT_FOUND) st = r.begin(915.0, 125.0, 9, 7, 0x12, 10, 8, 0.0f);
        if (st == RADIOLIB_ERR_NONE) { add(out, GOOD, "SPI  SX126x LoRa found"); any = true; }
        else if (st != RADIOLIB_ERR_CHIP_NOT_FOUND) { add(out, WARN, String("SPI  SX126x answers, begin ") + st); any = true; }
        r.sleep();
        delete m;
        radioPinsRelease();
    }
    if (!any) {
        busy("Auto detect", "SPI: probing SX127x...");
        Module* m = radioModule(p.irq, p.rst, p.aux);
        SX1276 r(m);
        int16_t st = r.begin(915.0);
        if (st == RADIOLIB_ERR_NONE || st == RADIOLIB_ERR_INVALID_FREQUENCY) {
            add(out, GOOD, String("SPI  SX127x LoRa found (v0x") + String(r.getChipVersion(), HEX) + ")");
            any = true;
        }
        r.sleep();
        delete m;
        radioPinsRelease();
    }
    if (!any) {
        busy("Auto detect", "SPI: probing CC1101...");
        Module* m = radioModule(p.irq, RADIOLIB_NC, p.aux);
        CC1101 r(m);
        if (r.begin(433.92) == RADIOLIB_ERR_NONE) {
            add(out, GOOD, String("SPI  CC1101 found (v0x") + String(r.getChipVersion(), HEX) + ")");
            any = true;
            r.sleep();
        }
        delete m;
        radioPinsRelease();
    }
    if (!any) {
        busy("Auto detect", "SPI: probing nRF24...");
        Module* m = radioModule(p.irq, p.aux, RADIOLIB_NC);
        nRF24 r(m);
        if (r.begin() == RADIOLIB_ERR_NONE) {
            add(out, GOOD, "SPI  nRF24L01 found");
            any = true;
            r.sleep();
        }
        delete m;
        radioPinsRelease();
    }
    if (!any) add(out, DIM, String("SPI  no radio on CS G") + p.cs);
}

static void probeI2c(std::vector<Finding>& out)
{
    busy("Auto detect", "I2C: scanning buses...");
    bool found[128];
    memset(found, 0, sizeof(found));
    M5.In_I2C.scanID(found);
    String ext;
    for (int a = 8; a < 0x78; a++) {
        if (!found[a] || a == 0x18 || a == 0x34 || a == 0x68 || a == 0x69) continue;
        ext += String(ext.length() ? " " : "") + "0x" + String(a, HEX);
    }
    add(out, ext.length() ? GOOD : DIM, "I2C  EXT: " + (ext.length() ? ext : String("nothing external")));

    memset(found, 0, sizeof(found));
    // Port 0 only: port 1 is the internal bus (keyboard, IMU). Taking it over
    // and releasing it kills the keyboard.
    M5.Ex_I2C.begin(I2C_NUM_0, g.pins.groveA, g.pins.groveB);
    M5.Ex_I2C.scanID(found);
    M5.Ex_I2C.release();
    String gr;
    for (int a = 8; a < 0x78; a++) if (found[a]) gr += String(gr.length() ? " " : "") + "0x" + String(a, HEX);
    add(out, gr.length() ? GOOD : DIM, "I2C  Grove: " + (gr.length() ? gr : String("nothing")));
}

static void probeGps(std::vector<Finding>& out)
{
    struct Try { UartPort port; bool swap; const char* name; } tries[] = {
        {PORT_EXT, false, "EXT"}, {PORT_EXT, true, "EXT swapped"}, {PORT_GROVE, false, "Grove"}, {PORT_GROVE, true, "Grove swapped"},
    };
    for (auto& t : tries) {
        int rx, tx;
        uartPins(t.port, t.swap, rx, tx);
        uint32_t b = gpsDetectBaud(rx, tx, true);
        if (b) {
            add(out, GOOD, String("UART NMEA/GPS on ") + t.name + " @" + b);
            return;
        }
    }
    add(out, DIM, "UART no NMEA on EXT or Grove");
}

void toolDetect()
{
    std::vector<Finding> out;
    probeRadios(out);
    probeI2c(out);
    probeGps(out);
    M5.Ex_I2C.release();

    for (;;) {
        frame("Auto detect", "r rerun  ` back");
        int y = TOP + 2;
        for (auto& f : out) {
            String s = f.text;
            while (s.length() && y < BOTTOM) {
                text(4, y, s.substring(0, 38).c_str(), f.color);
                s = s.length() > 38 ? "     " + s.substring(38) : "";
                y += LINE + 2;
            }
        }
        push();
        Key k = poll();
        if (k.type == K_BACK) return;
        if (k.is('r')) {
            out.clear();
            probeRadios(out);
            probeI2c(out);
            probeGps(out);
        }
        delay(20);
    }
}
