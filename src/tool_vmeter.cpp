// Voltmeter using the M5Stack Unit VMeter (U087): an isolated ADS1115 at
// 0x49 behind a divider, with factory calibration in an EEPROM at 0x53.
#include "config.h"
#include "tools.h"
#include "ui.h"

using namespace ui;

namespace {

constexpr uint8_t ADS_ADDR    = 0x49;
constexpr uint8_t EEPROM_ADDR = 0x53;
constexpr uint32_t HZ         = 100000;
// +/-1.024 V at the ADC is about +/-64 V at the probes, which covers the
// unit's +/-36 V rating. Values below are from M5Stack's own library.
constexpr uint16_t ADS_CONFIG = 0x8783;         // single shot, AIN0-AIN1, PGA 1.024 V, 128 SPS
constexpr float LSB_MV        = 0.03125f;       // ADC step at PGA 1.024 V
constexpr float DIVIDER       = 0.015918958f;   // input divider ratio
constexpr uint8_t CAL_REG     = 0xD0 + 3 * 8;   // calibration record for PGA 1.024 V

m5::I2C_Class* s_bus = nullptr;
float s_scale        = 0;  // volts per ADC count, sign included

bool present(m5::I2C_Class& bus) { return bus.scanID(ADS_ADDR, HZ); }

}  // namespace

bool vmeterBegin(const char** where, bool* calibrated)
{
    s_bus = nullptr;
    // Port 0 only: port 1 is the internal bus (keyboard, IMU).
    M5.Ex_I2C.begin(I2C_NUM_0, g.pins.groveA, g.pins.groveB);
    if (present(M5.Ex_I2C)) {
        s_bus = &M5.Ex_I2C;
        if (where) *where = "Grove";
    } else {
        M5.Ex_I2C.release();
        if (present(M5.In_I2C)) {
            s_bus = &M5.In_I2C;
            if (where) *where = "EXT";
        }
    }
    if (!s_bus) return false;

    // Calibration record: gain, expected (2), measured (2), XOR checksum.
    uint8_t c[8] = {0};
    float cal    = 1.0f;
    bool ok      = s_bus->readRegister(EEPROM_ADDR, CAL_REG, c, 8, HZ);
    if (ok) {
        uint8_t x = 0;
        for (int i = 0; i < 5; i++) x ^= c[i];
        int16_t hope = (c[1] << 8) | c[2], actual = (c[3] << 8) | c[4];
        ok = x == c[5] && hope != 0 && actual != 0;
        if (ok) cal = fabsf((float)hope / actual);
    }
    if (calibrated) *calibrated = ok;
    s_scale = -(LSB_MV / DIVIDER) * cal / 1000.0f;  // the unit's input is inverted
    return true;
}

void vmeterEnd()
{
    if (s_bus == &M5.Ex_I2C) M5.Ex_I2C.release();
    s_bus = nullptr;
}

bool vmeterRead(float& volts)
{
    if (!s_bus) return false;
    uint8_t cfg[2] = {(uint8_t)(ADS_CONFIG >> 8), (uint8_t)ADS_CONFIG}, d[2];
    if (!s_bus->writeRegister(ADS_ADDR, 0x01, cfg, 2, HZ)) return false;
    delay(10);
    if (!s_bus->readRegister(ADS_ADDR, 0x00, d, 2, HZ)) return false;
    volts = (int16_t)((d[0] << 8) | d[1]) * s_scale;
    return true;
}

const char* vmeterRail(float v, bool* ok)
{
    float a = fabsf(v);
    bool good = false;
    const char* s;
    if (a < 0.2f) s = "no voltage";
    else if (a >= 3.135f && a <= 3.465f) { s = "3.3 V rail, within 5%"; good = true; }
    else if (a >= 4.5f && a <= 5.25f) { s = "5 V rail, in USB range"; good = true; }
    else if (a > 3.465f && a < 4.5f) s = "between 3.3 V and 5 V";
    else if (a < 3.135f) s = "below 3.3 V";
    else s = "above 5 V";
    if (ok) *ok = good;
    return s;
}

void toolVmeter()
{
    const char* where = "";
    bool cal          = false;
    busy("Voltmeter", "looking for Unit VMeter...");
    if (!vmeterBegin(&where, &cal)) {
        message("Voltmeter", "No Unit VMeter found.\n\nPlug it into the Grove port.\nIt answers at I2C 0x49.", BAD);
        return;
    }
    float lo = NAN, hi = NAN;
    for (;;) {
        float sum = 0, v = 0;
        int n = 0;
        for (int i = 0; i < 4; i++)
            if (vmeterRead(v)) { sum += v; n++; }
        frame("Voltmeter", "r reset min/max  ` back");
        if (!n) {
            text(4, TOP + 40, "read failed. Unit unplugged?", BAD);
        } else {
            v = sum / n;
            if (isnan(lo) || v < lo) lo = v;
            if (isnan(hi) || v > hi) hi = v;
            bool ok;
            const char* rail = vmeterRail(v, &ok);
            char buf[16];
            snprintf(buf, sizeof(buf), "%.3f", v);
            canvas.setFont(&fonts::Font7);
            canvas.setTextColor(ok ? GOOD : FG);
            canvas.setTextDatum(top_right);
            canvas.drawString(buf, W - 34, TOP + 6);
            canvas.setFont(&fonts::Font4);
            canvas.setTextDatum(top_left);
            canvas.drawString("V", W - 28, TOP + 28);
            canvas.setFont(&fonts::Font0);
            text(4, TOP + 62, rail, ok ? GOOD : WARN);
            if (v < -0.2f) textRight(W - 4, TOP + 62, "probes reversed", WARN);
            textf(4, TOP + 76, DIM, "min %.3f   max %.3f   span %.0f mV", lo, hi, (hi - lo) * 1000);
            textf(4, TOP + 88, cal ? DIM : WARN, "%s port, %s   max +/-36 V", where, cal ? "factory cal" : "NO calibration");
        }
        push();
        Key k = poll();
        if (k.type == K_BACK) break;
        if (k.is('r')) lo = hi = NAN;
    }
    vmeterEnd();
}
