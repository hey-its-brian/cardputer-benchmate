// Test rig: pick a module profile, see how to wire it, then run a pass/fail
// sequence that checks every signal wire and the basic function of the part.
#include <SPI.h>
#include <driver/gpio.h>
#include <vector>
#include "config.h"
#include "tools.h"
#include "ui.h"

using namespace ui;

namespace {

enum St : uint8_t { PASS, FAIL, WARNING, INFO };

struct Result {
    St st;
    String name;
    String detail;
};
typedef std::vector<Result> Results;

struct WireRow {
    String mod;   // pin on the module
    String host;  // where it goes on the Cardputer
    uint16_t color;
};
typedef std::vector<WireRow> Wiring;

struct Profile {
    const char* name;
    void (*wiring)(Wiring&);
    void (*run)(Results&);
};

void add(Results& r, St st, const String& name, const String& detail = "")
{
    r.push_back({st, name, detail});
}

// ------------------------------------------------------------ pin labels

// "EXT 13  G5" for a GPIO on the EXT header or Grove port, else just "G5".
String hostPin(int gpio)
{
    static const struct { int8_t gpio; const char* where; } MAP[] = {
        {3, "EXT 1"},  {4, "EXT 3"},   {6, "EXT 5"},   {40, "EXT 7"},  {14, "EXT 9"},
        {39, "EXT 11"}, {5, "EXT 13"}, {8, "EXT 8"},   {9, "EXT 10"},  {13, "EXT 12"},
        {15, "EXT 14"}, {2, "Grove yel"}, {1, "Grove wht"},
    };
    for (auto& m : MAP)
        if (m.gpio == gpio) return String(m.where) + "  G" + gpio;
    return String("G") + gpio;
}

void wire(Wiring& w, const char* mod, const String& host, uint16_t color = FG)
{
    w.push_back({mod, host, color});
}

void wirePower33(Wiring& w, const char* gnd, const char* vcc)
{
    wire(w, gnd, "EXT 4  GND");
    wire(w, vcc, "3.3V reg (NOT 5V!)", WARN);
}

// ------------------------------------------------------------ SPI helpers

const SPISettings SPI_CFG(1000000, MSBFIRST, SPI_MODE0);

// Pull resistors are set through the IDF so the MISO pin stays attached to
// the SPI peripheral (Arduino's pinMode would detach it).
void pull(int pin, gpio_pull_mode_t mode)
{
    gpio_set_pull_mode((gpio_num_t)pin, mode);
    delayMicroseconds(300);
}

bool level(int pin) { return gpio_get_level((gpio_num_t)pin); }

void csLow()
{
    SPI.beginTransaction(SPI_CFG);
    digitalWrite(g.pins.cs, LOW);
}

void csHigh()
{
    digitalWrite(g.pins.cs, HIGH);
    SPI.endTransaction();
}

// Checks that the module releases MISO while CSN is high and pulls it low
// when CSN goes low. CC1101 (chip ready) and nRF24 (status bit 7) both do
// this before any clock, so it proves CSN, MISO and power on their own.
bool checkSelect(Results& r)
{
    int miso = g.pins.miso;
    pull(miso, GPIO_PULLUP_ONLY);
    bool idleHi = level(miso);
    pull(miso, GPIO_PULLDOWN_ONLY);
    bool idleLo = !level(miso);
    pull(miso, GPIO_PULLUP_ONLY);
    csLow();
    delay(3);
    bool selLo = !level(miso);
    csHigh();

    if (!selLo) {
        add(r, FAIL, "CSN + MISO + power", "MISO stays high with CSN low. Check CSN, MISO, VCC, GND");
        return false;
    }
    if (!(idleHi && idleLo)) {
        add(r, FAIL, "CSN + MISO", idleHi ? "MISO driven high with CSN high" : "MISO low with CSN high. CSN wire open, or MISO shorted");
        return true;  // SPI may still work, keep going
    }
    add(r, PASS, "CSN + MISO + power", "module answers chip select");
    return true;
}

// One GDO/IRQ style line, judged from four samples:
//   bothHi/bothLo: both module outputs driven the same way (is it connected?)
//   mine/other:    only this output high, only the other output high (is it the right one?)
void judgeLine(Results& r, const char* name, int gpio, bool bothHi, bool bothLo, bool mine, bool other)
{
    String where = String(name) + " -> G" + gpio;
    if (!(bothHi && !bothLo)) add(r, FAIL, where, "no signal. Wire open?");
    else if (mine && !other) add(r, PASS, where, "toggles");
    else if (!mine && other) add(r, FAIL, where, "swapped with the other GDO line");
    else add(r, FAIL, where, "follows both outputs. Shorted together?");
}

// ------------------------------------------------------------ CC1101

bool ccSelect()
{
    csLow();
    uint32_t t0 = micros();
    while (level(g.pins.miso))
        if (micros() - t0 > 5000) return false;
    return true;
}

uint8_t ccStrobe(uint8_t cmd)
{
    ccSelect();
    uint8_t s = SPI.transfer(cmd);
    csHigh();
    return s;
}

uint8_t ccRead(uint8_t reg)
{
    ccSelect();
    SPI.transfer(reg | 0x80);
    uint8_t v = SPI.transfer(0);
    csHigh();
    return v;
}

uint8_t ccStatus(uint8_t reg) { return ccRead(reg | 0x40); }

void ccWrite(uint8_t reg, uint8_t v)
{
    ccSelect();
    SPI.transfer(reg);
    SPI.transfer(v);
    csHigh();
}

void ccGdo(bool gdo0High, bool gdo2High)
{
    ccWrite(0x02, gdo0High ? 0x6F : 0x2F);  // IOCFG0: constant level
    ccWrite(0x00, gdo2High ? 0x6F : 0x2F);  // IOCFG2
    delayMicroseconds(300);
}

void runCC1101(Results& r)
{
    const PinConfig& p = g.pins;
    radioSpiBegin();
    radioPinsRelease();

    busy("CC1101 test", "checking SPI wires...");
    bool alive = checkSelect(r);
    if (alive) {
        ccStrobe(0x30);  // SRES
        delay(2);
        uint8_t part = ccStatus(0x30), ver = ccStatus(0x31);
        char buf[48];
        if (part != 0x00 || ver == 0x00 || ver == 0xFF) {
            snprintf(buf, sizeof(buf), "part 0x%02X ver 0x%02X. Check SCK, MOSI", part, ver);
            add(r, FAIL, "SCK + MOSI", buf);
            alive = false;
        } else {
            snprintf(buf, sizeof(buf), "CC1101 version 0x%02X", ver);
            add(r, PASS, "SCK + MOSI", buf);
        }
    }
    if (!alive) {
        add(r, INFO, "Remaining checks", "skipped, no SPI link");
        pull(p.miso, GPIO_FLOATING);
        radioPinsRelease();
        return;
    }

    ccWrite(0x09, 0x55);
    uint8_t a = ccRead(0x09);
    ccWrite(0x09, 0xAA);
    uint8_t b = ccRead(0x09);
    if (a == 0x55 && b == 0xAA) add(r, PASS, "Register read-back", "0x55 / 0xAA ok");
    else add(r, FAIL, "Register read-back", String("got 0x") + String(a, HEX) + " / 0x" + String(b, HEX));

    // GDO0 / GDO2. Each sample uses the pull that opposes the expected level,
    // so an open wire reads wrong instead of floating to the right answer.
    busy("CC1101 test", "checking GDO0 / GDO2...");
    ccGdo(true, true);
    pinMode(p.irq, INPUT_PULLDOWN);
    pinMode(p.aux, INPUT_PULLDOWN);
    delay(1);
    bool hi0 = digitalRead(p.irq), hi2 = digitalRead(p.aux);
    ccGdo(false, false);
    pinMode(p.irq, INPUT_PULLUP);
    pinMode(p.aux, INPUT_PULLUP);
    delay(1);
    bool lo0 = digitalRead(p.irq), lo2 = digitalRead(p.aux);
    ccGdo(true, false);
    pinMode(p.irq, INPUT_PULLDOWN);
    delay(1);
    bool a0 = digitalRead(p.irq), a2 = digitalRead(p.aux);
    ccGdo(false, true);
    pinMode(p.irq, INPUT_PULLUP);
    pinMode(p.aux, INPUT_PULLDOWN);
    delay(1);
    bool b0 = digitalRead(p.irq), b2 = digitalRead(p.aux);
    judgeLine(r, "GDO0", p.irq, hi0, lo0, a0, b0);
    judgeLine(r, "GDO2", p.aux, hi2, lo2, b2, a2);
    pinMode(p.irq, INPUT);
    pinMode(p.aux, INPUT);

    // Crystal and synthesizer: tune, enter RX, expect the RX state.
    busy("CC1101 test", "checking radio...");
    float f = g.subGhzFreq;
    bool inBand = (f >= 300 && f <= 348) || (f >= 387 && f <= 464) || (f >= 779 && f <= 928);
    if (!inBand) f = 433.92f;
    uint32_t word = (uint32_t)(f * 65536.0f / 26.0f);
    ccStrobe(0x30);
    delay(2);
    ccWrite(0x0D, word >> 16);
    ccWrite(0x0E, word >> 8);
    ccWrite(0x0F, word);
    ccWrite(0x0B, 0x06);  // FSCTRL1
    ccWrite(0x18, 0x18);  // MCSM0: calibrate when leaving idle
    ccWrite(0x23, 0xE9);  // FSCAL3..0
    ccWrite(0x24, 0x2A);
    ccWrite(0x25, 0x00);
    ccWrite(0x26, 0x1F);
    ccWrite(0x2E, f < 350 ? 0x0B : 0x09);  // TEST0
    ccStrobe(0x34);                        // SRX
    uint8_t state = 0;
    for (int i = 0; i < 20; i++) {
        delay(1);
        state = ccStatus(0x35) & 0x1F;
        if (state == 0x0D) break;
    }
    char buf[48];
    if (state == 0x0D) {
        snprintf(buf, sizeof(buf), "locked at %.2f MHz", f);
        add(r, PASS, "Crystal + synth", buf);
        delay(8);
        uint8_t raw = ccStatus(0x34);
        float dbm   = (raw >= 128 ? (int)raw - 256 : (int)raw) / 2.0f - 74.0f;
        snprintf(buf, sizeof(buf), "noise floor %.0f dBm", dbm);
        add(r, dbm > -120 && dbm < -20 ? PASS : WARNING, "Receiver", buf);
    } else {
        snprintf(buf, sizeof(buf), "no RX lock, state 0x%02X", state);
        add(r, FAIL, "Crystal + synth", buf);
    }

    // Transmit a 4-byte packet at the lowest power setting.
    ccStrobe(0x36);  // SIDLE
    ccStrobe(0x3B);  // SFTX
    ccWrite(0x08, 0x00);                   // PKTCTRL0: fixed length, no CRC
    ccWrite(0x06, 4);                      // PKTLEN
    ccWrite(0x3E, f < 500 ? 0x12 : 0x03);  // PATABLE: about -30 dBm
    ccSelect();
    SPI.transfer(0x7F);  // TX FIFO, burst
    for (uint8_t i = 0; i < 4; i++) SPI.transfer(0xA5);
    csHigh();
    ccStrobe(0x35);  // STX
    bool sawTx = false, done = false;
    uint8_t left = 0;
    for (int i = 0; i < 400 && !done; i++) {
        state = ccStatus(0x35) & 0x1F;
        left  = ccStatus(0x3A) & 0x7F;
        if (state == 0x13 || left < 4) sawTx = true;
        if (sawTx && state == 0x01 && left == 0) done = true;
        delayMicroseconds(200);
    }
    if (done) add(r, PASS, "Transmit", "4-byte packet sent, low power");
    else {
        snprintf(buf, sizeof(buf), "did not finish, state 0x%02X, %u left", state, left);
        add(r, FAIL, "Transmit", buf);
    }
    add(r, INFO, "RF output", "not measured. Needs a second radio");

    ccStrobe(0x36);
    ccStrobe(0x39);  // SPWD
    pull(p.miso, GPIO_FLOATING);
    radioPinsRelease();
}

void wiringCC1101(Wiring& w)
{
    const PinConfig& p = g.pins;
    wirePower33(w, "1 GND", "2 VCC");
    wire(w, "3 GDO0", hostPin(p.irq));
    wire(w, "4 CSN", hostPin(p.cs));
    wire(w, "5 SCK", hostPin(p.sck));
    wire(w, "6 MOSI", hostPin(p.mosi));
    wire(w, "7 MISO/GDO1", hostPin(p.miso));
    wire(w, "8 GDO2", hostPin(p.aux));
}

void wiringE07(Wiring& w)
{
    wiringCC1101(w);
    wire(w, "E07-M1101D", "pin 1 = square pad", DIM);
    wire(w, "SMD E07", "match by signal name", DIM);
}

// ------------------------------------------------------------ nRF24

uint8_t nrfRead(uint8_t reg)
{
    csLow();
    SPI.transfer(reg & 0x1F);
    uint8_t v = SPI.transfer(0xFF);
    csHigh();
    return v;
}

uint8_t nrfWrite(uint8_t reg, uint8_t v)
{
    csLow();
    uint8_t s = SPI.transfer(0x20 | (reg & 0x1F));
    SPI.transfer(v);
    csHigh();
    return s;
}

uint8_t nrfCmd(uint8_t cmd)
{
    csLow();
    uint8_t s = SPI.transfer(cmd);
    csHigh();
    return s;
}

void runNrf24(Results& r)
{
    const PinConfig& p = g.pins;
    int ce = p.aux;
    radioSpiBegin();
    radioPinsRelease();
    pinMode(ce, OUTPUT);
    digitalWrite(ce, LOW);
    auto finish = [&]() {
        pull(p.miso, GPIO_FLOATING);
        radioPinsRelease();
    };

    busy("nRF24 test", "checking SPI wires...");
    char buf[48];
    bool alive = checkSelect(r);
    if (alive) {
        nrfWrite(0x00, 0x08);  // power down, known state
        nrfCmd(0xE2);          // FLUSH_RX
        uint8_t status = nrfCmd(0xFF);
        uint8_t aw     = nrfRead(0x03);
        if ((status & 0x80) || (status & 0x0E) != 0x0E || aw < 1 || aw > 3) {
            snprintf(buf, sizeof(buf), "status 0x%02X aw 0x%02X. Check SCK, MOSI", status, aw);
            add(r, FAIL, "SCK + MOSI", buf);
            alive = false;
        } else {
            snprintf(buf, sizeof(buf), "nRF24 status 0x%02X", status);
            add(r, PASS, "SCK + MOSI", buf);
        }
    }
    if (!alive) {
        add(r, INFO, "Remaining checks", "skipped, no SPI link");
        finish();
        return;
    }

    nrfWrite(0x05, 0x55);
    uint8_t a = nrfRead(0x05);
    nrfWrite(0x05, 0x2A);
    uint8_t b = nrfRead(0x05);
    if (a == 0x55 && b == 0x2A) add(r, PASS, "Register read-back", "0x55 / 0x2A ok");
    else add(r, FAIL, "Register read-back", String("got 0x") + String(a, HEX) + " / 0x" + String(b, HEX));

    // CE and IRQ: send one packet with auto-ack off. The packet only goes out
    // if CE reaches the module, and TX_DS then pulls IRQ low.
    busy("nRF24 test", "checking CE / IRQ...");
    nrfWrite(0x00, 0x0E);  // power up, PTX, CRC on
    delay(5);
    nrfWrite(0x01, 0x00);  // EN_AA off
    nrfWrite(0x04, 0x00);  // no retries
    nrfWrite(0x05, 100);   // channel 100
    nrfWrite(0x06, 0x00);  // 1 Mbps, lowest power
    nrfCmd(0xE1);          // FLUSH_TX
    nrfWrite(0x07, 0x70);  // clear flags
    pinMode(p.irq, INPUT_PULLDOWN);
    delay(1);
    bool idleHigh = digitalRead(p.irq);
    csLow();
    SPI.transfer(0xA0);  // W_TX_PAYLOAD
    for (uint8_t i = 0; i < 4; i++) SPI.transfer(0xA5);
    csHigh();
    digitalWrite(ce, HIGH);
    delay(1);
    digitalWrite(ce, LOW);
    bool sent = false;
    for (int i = 0; i < 40 && !sent; i++) {
        delayMicroseconds(500);
        sent = nrfCmd(0xFF) & 0x20;
    }
    pinMode(p.irq, INPUT_PULLUP);
    delay(1);
    bool irqLow = !digitalRead(p.irq);
    nrfWrite(0x07, 0x70);
    delay(1);
    bool irqBack = digitalRead(p.irq);
    pinMode(p.irq, INPUT);

    String ceName = String("CE -> G") + ce, irqName = String("IRQ -> G") + p.irq;
    if (sent) add(r, PASS, ceName, "packet sent on CE pulse");
    else add(r, FAIL, ceName, "no transmit after CE pulse. Wire open?");
    if (!idleHigh) add(r, FAIL, irqName, "low at idle. Wire open or shorted");
    else if (!sent) add(r, WARNING, irqName, "idle level ok, edge untested (needs CE)");
    else if (!irqLow) add(r, FAIL, irqName, "did not go low on TX done. Wire open?");
    else if (!irqBack) add(r, FAIL, irqName, "stuck low after clearing flags");
    else add(r, PASS, irqName, "toggles");

    // Receiver: enter PRX and count channels with a carrier.
    busy("nRF24 test", "checking receiver...");
    nrfWrite(0x00, 0x0F);
    delay(2);
    int busyCh = 0, total = 0;
    for (int ch = 2; ch <= 80; ch += 2) {
        nrfWrite(0x05, ch);
        digitalWrite(ce, HIGH);
        delayMicroseconds(500);
        if (nrfRead(0x09) & 1) busyCh++;
        digitalWrite(ce, LOW);
        total++;
    }
    bool prx = nrfRead(0x00) == 0x0F;
    snprintf(buf, sizeof(buf), "carrier on %d of %d channels", busyCh, total);
    add(r, prx ? PASS : FAIL, "Receiver", prx ? buf : "would not enter RX mode");
    add(r, INFO, "RF output", "not measured. Needs a second radio");

    nrfWrite(0x00, 0x08);
    finish();
}

void wiringNrf24(Wiring& w)
{
    const PinConfig& p = g.pins;
    wirePower33(w, "1 GND", "2 VCC");
    wire(w, "3 CE", hostPin(p.aux));
    wire(w, "4 CSN", hostPin(p.cs));
    wire(w, "5 SCK", hostPin(p.sck));
    wire(w, "6 MOSI", hostPin(p.mosi));
    wire(w, "7 MISO", hostPin(p.miso));
    wire(w, "8 IRQ", hostPin(p.irq));
    wire(w, "add 10uF", "across VCC and GND", DIM);
}

// ------------------------------------------------------------ GPS

bool nmeaOk(const char* s)
{
    if (*s != '$') return false;
    uint8_t cs    = 0;
    const char* q = s + 1;
    while (*q && *q != '*') cs ^= (uint8_t)*q++;
    if (*q != '*' || !isxdigit(q[1]) || !isxdigit(q[2])) return false;
    return cs == (uint8_t)strtoul(q + 1, nullptr, 16);
}

// Returns field `n` (0 = sentence name) of a comma separated NMEA line as an int.
int nmeaInt(const char* s, int n)
{
    while (n-- > 0) {
        s = strchr(s, ',');
        if (!s) return 0;
        s++;
    }
    return atoi(s);
}

void runGps(Results& r)
{
    struct Try { UartPort port; bool swap; const char* name; } tries[] = {
        {PORT_EXT, false, "EXT"}, {PORT_EXT, true, "EXT"}, {PORT_GROVE, false, "Grove"}, {PORT_GROVE, true, "Grove"},
    };
    M5.Ex_I2C.release();
    uint32_t baud = 0;
    Try* hit      = nullptr;
    for (auto& t : tries) {
        int rx, tx;
        uartPins(t.port, t.swap, rx, tx);
        baud = gpsDetectBaud(rx, tx, true);
        if (baud) { hit = &t; break; }
    }
    if (!hit) {
        add(r, FAIL, "NMEA data", "nothing on EXT or Grove. Check TX wire, power");
        return;
    }
    add(r, PASS, "Module TX wire", String(hit->name) + " port, " + baud + " baud");
    if (hit->swap) add(r, WARNING, "TX/RX order", "works only swapped. Module TX is on our TX pin");

    busy("GPS test", "reading sentences (3 s)...");
    int good = 0, bad = 0, fix = 0, used = 0, inView = 0, len = 0;
    char line[100];
    uint32_t t0 = millis();
    while (millis() - t0 < 3000) {
        while (Serial1.available()) {
            char c = Serial1.read();
            if (c == '$') len = 0;
            if (c == '\r') continue;
            if (c != '\n') {
                if (len < (int)sizeof(line) - 1) line[len++] = c;
                continue;
            }
            line[len] = 0;
            len       = 0;
            if (line[0] != '$') continue;
            if (!nmeaOk(line)) { bad++; continue; }
            good++;
            if (strlen(line) > 6 && !strncmp(line + 3, "GGA", 3)) {
                fix  = nmeaInt(line, 6);
                used = nmeaInt(line, 7);
            }
            if (strlen(line) > 6 && !strncmp(line + 3, "GSV", 3)) inView = max(inView, nmeaInt(line, 3));
        }
        delay(2);
    }
    Serial1.end();

    String counts = String(good) + " ok, " + bad + " bad";
    if (!good) add(r, FAIL, "NMEA checksums", counts);
    else add(r, bad * 4 > good ? FAIL : bad ? WARNING : PASS, "NMEA checksums", counts);
    add(r, INFO, "Satellites", String(inView) + " in view, " + used + " used");
    if (fix) add(r, PASS, "Position fix", "has a fix");
    else add(r, INFO, "Position fix", "none yet. Normal indoors");
    add(r, INFO, "Module RX wire", "not tested. GPS only talks");
}

void wiringGps(Wiring& w)
{
    const PinConfig& p = g.pins;
    wire(w, "VCC", "EXT 6  5V OUT");
    wire(w, "GND", "EXT 4  GND");
    wire(w, "TX", hostPin(p.uartRx));
    wire(w, "RX", hostPin(p.uartTx));
    wire(w, "3.3V-only GPS", "use a 3.3V regulator", WARN);
    wire(w, "or Grove port", "TX wht G1, RX yel G2", DIM);
}

// ------------------------------------------------------------ I2C sensors

constexpr uint32_t I2C_HZ = 100000;

uint8_t crc8(const uint8_t* d, size_t n)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
    }
    return crc;
}

// Sends a command, waits, then reads `len` bytes (for parts with no register map).
bool cmdRead(m5::I2C_Class& i2c, uint8_t addr, const uint8_t* cmd, size_t n, uint32_t waitMs, uint8_t* out, size_t len)
{
    bool ok = i2c.start(addr, false, I2C_HZ) && i2c.write(cmd, n);
    ok      = i2c.stop() && ok;
    if (!ok) return false;
    if (!len) return true;
    delay(waitMs);
    ok = i2c.start(addr, true, I2C_HZ) && i2c.read(out, len, true);
    return i2c.stop() && ok;
}

String reading(float t, float rh, float hpa)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "%.1fC", t);
    if (!isnan(rh)) n += snprintf(buf + n, sizeof(buf) - n, " %.0f%%", rh);
    if (!isnan(hpa)) snprintf(buf + n, sizeof(buf) - n, " %.0fhPa", hpa);
    return buf;
}

bool sane(float t, float rh, float hpa)
{
    if (isnan(t) || t < -20 || t > 60) return false;
    if (!isnan(rh) && (rh <= 0 || rh > 100)) return false;
    if (!isnan(hpa) && (hpa < 300 || hpa > 1100)) return false;
    return true;
}

void report(Results& r, const String& name, float t, float rh = NAN, float hpa = NAN)
{
    if (sane(t, rh, hpa)) add(r, PASS, name, reading(t, rh, hpa));
    else add(r, FAIL, name, "reading out of range: " + reading(t, rh, hpa));
}

void testBosch(Results& r, m5::I2C_Class& i2c, uint8_t addr, const String& at)
{
    uint8_t id = i2c.readRegister8(addr, 0xD0, I2C_HZ);
    if (id == 0x61) { add(r, PASS, "BME680/688" + at, "ID ok. No reading test yet"); return; }
    if (id == 0x55) { add(r, PASS, "BMP180" + at, "ID ok. No reading test yet"); return; }
    bool bme = id == 0x60;
    if (!bme && id != 0x58 && id != 0x56 && id != 0x57) {
        uint8_t id0 = i2c.readRegister8(addr, 0x00, I2C_HZ);
        if (id0 == 0x50 || id0 == 0x60) add(r, PASS, "BMP388/390" + at, "ID ok. No reading test yet");
        else add(r, WARNING, "Unknown part" + at, String("ID 0x") + String(id, HEX) + ", not recognised");
        return;
    }
    String name = String(bme ? "BME280" : "BMP280") + at;
    uint8_t c[24], h[8] = {0}, d[8] = {0};
    if (!i2c.readRegister(addr, 0x88, c, 24, I2C_HZ)) { add(r, FAIL, name, "calibration read failed"); return; }
    auto u16 = [&](int i) { return (uint16_t)(c[i] | (c[i + 1] << 8)); };
    auto s16 = [&](int i) { return (int16_t)(c[i] | (c[i + 1] << 8)); };
    if (bme) i2c.writeRegister8(addr, 0xF2, 0x01, I2C_HZ);
    i2c.writeRegister8(addr, 0xF4, 0x25, I2C_HZ);  // forced mode, x1 oversampling
    delay(20);
    if (!i2c.readRegister(addr, 0xF7, d, bme ? 8 : 6, I2C_HZ)) { add(r, FAIL, name, "data read failed"); return; }
    double adcP = (d[0] << 12) | (d[1] << 4) | (d[2] >> 4);
    double adcT = (d[3] << 12) | (d[4] << 4) | (d[5] >> 4);
    double v1   = (adcT / 16384.0 - u16(0) / 1024.0) * s16(2);
    double v2   = (adcT / 131072.0 - u16(0) / 8192.0) * (adcT / 131072.0 - u16(0) / 8192.0) * s16(4);
    double fine = v1 + v2;
    float t     = fine / 5120.0;

    v1 = fine / 2.0 - 64000.0;
    v2 = v1 * v1 * s16(16) / 32768.0;
    v2 = v2 + v1 * s16(14) * 2.0;
    v2 = v2 / 4.0 + s16(12) * 65536.0;
    v1 = (s16(10) * v1 * v1 / 524288.0 + s16(8) * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * u16(6);
    float hpa = NAN;
    if (v1 != 0) {
        double pr = 1048576.0 - adcP;
        pr        = (pr - v2 / 4096.0) * 6250.0 / v1;
        v1        = s16(22) * pr * pr / 2147483648.0;
        v2        = pr * s16(20) / 32768.0;
        hpa       = (pr + (v1 + v2 + s16(18)) / 16.0) / 100.0;
    }

    float rh = NAN;
    if (bme) {
        uint8_t h1 = i2c.readRegister8(addr, 0xA1, I2C_HZ);
        i2c.readRegister(addr, 0xE1, h, 7, I2C_HZ);
        int16_t h2 = (int16_t)(h[0] | (h[1] << 8));
        uint8_t h3 = h[2];
        int16_t h4 = (int16_t)((int8_t)h[3] * 16) | (h[4] & 0x0F);
        int16_t h5 = (int16_t)((int8_t)h[5] * 16) | (h[4] >> 4);
        int8_t h6  = (int8_t)h[6];
        double adcH = (d[6] << 8) | d[7];
        double x    = fine - 76800.0;
        x = (adcH - (h4 * 64.0 + h5 / 16384.0 * x)) * (h2 / 65536.0 * (1.0 + h6 / 67108864.0 * x * (1.0 + h3 / 67108864.0 * x)));
        x = x * (1.0 - h1 * x / 524288.0);
        rh = x;
    }
    report(r, name, t, rh, hpa);
}

void testSht(Results& r, m5::I2C_Class& i2c, uint8_t addr, const String& at)
{
    uint8_t d[6];
    const uint8_t sht4[] = {0xFD};  // SHT4x: measure, high precision
    if (cmdRead(i2c, addr, sht4, 1, 12, d, 6) && crc8(d, 2) == d[2] && crc8(d + 3, 2) == d[5]) {
        float t  = -45 + 175.0f * ((d[0] << 8) | d[1]) / 65535.0f;
        float rh = -6 + 125.0f * ((d[3] << 8) | d[4]) / 65535.0f;
        report(r, "SHT4x" + at, t, constrain(rh, 0.1f, 100.0f));
        return;
    }
    const uint8_t sht3[] = {0x24, 0x00};  // SHT3x: single shot, high repeatability
    if (cmdRead(i2c, addr, sht3, 2, 20, d, 6) && crc8(d, 2) == d[2] && crc8(d + 3, 2) == d[5]) {
        float t  = -45 + 175.0f * ((d[0] << 8) | d[1]) / 65535.0f;
        float rh = 100.0f * ((d[3] << 8) | d[4]) / 65535.0f;
        report(r, "SHT3x" + at, t, rh);
        return;
    }
    add(r, FAIL, "SHT3x/4x" + at, "answers, but no valid measurement");
}

void testAht(Results& r, m5::I2C_Class& i2c, uint8_t addr, const String& at)
{
    uint8_t d[7];
    const uint8_t trig[] = {0xAC, 0x33, 0x00};
    if (!cmdRead(i2c, addr, trig, 3, 90, d, 7) || (d[0] & 0x80)) {
        add(r, FAIL, "AHT10/20" + at, "answers, but no measurement");
        return;
    }
    uint32_t rawH = ((uint32_t)d[1] << 12) | (d[2] << 4) | (d[3] >> 4);
    uint32_t rawT = ((uint32_t)(d[3] & 0x0F) << 16) | (d[4] << 8) | d[5];
    report(r, String(crc8(d, 6) == d[6] ? "AHT20" : "AHT10") + at, rawT / 1048576.0f * 200 - 50, rawH / 1048576.0f * 100);
}

void testScd4x(Results& r, m5::I2C_Class& i2c, uint8_t addr, const String& at)
{
    String name = "SCD4x" + at;
    uint8_t d[9];
    const uint8_t stop[] = {0x3F, 0x86}, serial[] = {0x36, 0x82}, start[] = {0x21, 0xB1}, readm[] = {0xEC, 0x05};
    cmdRead(i2c, addr, stop, 2, 0, nullptr, 0);
    delay(500);
    if (!cmdRead(i2c, addr, serial, 2, 2, d, 9) || crc8(d, 2) != d[2]) {
        add(r, FAIL, name, "no valid serial number reply");
        return;
    }
    busy("Sensor test", "SCD4x: measuring CO2 (5 s)...");
    cmdRead(i2c, addr, start, 2, 0, nullptr, 0);
    delay(5200);
    bool ok = cmdRead(i2c, addr, readm, 2, 2, d, 9) && crc8(d, 2) == d[2] && crc8(d + 3, 2) == d[5];
    cmdRead(i2c, addr, stop, 2, 0, nullptr, 0);
    if (!ok) { add(r, FAIL, name, "ID ok, but no measurement"); return; }
    int co2  = (d[0] << 8) | d[1];
    float t  = -45 + 175.0f * ((d[3] << 8) | d[4]) / 65535.0f;
    float rh = 100.0f * ((d[6] << 8) | d[7]) / 65535.0f;
    String s = String(co2) + "ppm " + reading(t, rh, NAN);
    if (co2 >= 300 && co2 <= 10000 && sane(t, rh, NAN)) add(r, PASS, name, s);
    else add(r, FAIL, name, "reading out of range: " + s);
}

const char* otherPart(uint8_t a)
{
    switch (a) {
        case 0x10: return "VEML7700?";
        case 0x23: case 0x5C: return "BH1750?";
        case 0x24: return "PN532?";
        case 0x29: return "VL53L0X / TSL2591?";
        case 0x39: return "TSL2561 / APDS9960?";
        case 0x3C: case 0x3D: return "OLED display?";
        case 0x40: return "HTU21 / SI7021 / INA219?";
        case 0x48: case 0x49: return "ADS1115 / TMP102?";
        case 0x52: case 0x53: return "ENS160?";
        case 0x58: return "SGP30?";
        case 0x59: return "SGP40 / SGP41?";
        case 0x5A: return "CCS811 / MLX90614?";
        case 0x61: return "SCD30?";
        default:   return "unknown part";
    }
}

int testBus(Results& r, m5::I2C_Class& i2c, const char* busName, bool internal)
{
    bool found[128];
    memset(found, 0, sizeof(found));
    i2c.scanID(found, I2C_HZ);
    int n = 0;
    for (int a = 8; a < 0x78; a++) {
        if (!found[a]) continue;
        // parts soldered on the Cardputer ADV itself
        if (internal && (a == 0x18 || a == 0x34 || a == 0x68 || a == 0x69)) continue;
        n++;
        char at[24];
        snprintf(at, sizeof(at), " 0x%02X %s", a, busName);
        if (a == 0x76 || a == 0x77) testBosch(r, i2c, a, at);
        else if (a == 0x44 || a == 0x45) testSht(r, i2c, a, at);
        else if (a == 0x38) testAht(r, i2c, a, at);
        else if (a == 0x62) testScd4x(r, i2c, a, at);
        else add(r, INFO, String("Device") + at, String(otherPart(a)) + " No test for it");
    }
    return n;
}

void runI2c(Results& r)
{
    busy("Sensor test", "scanning I2C buses...");
    int n = testBus(r, M5.In_I2C, "EXT", true);
    // Port 0 only: port 1 is the internal bus (keyboard, IMU).
    M5.Ex_I2C.begin(I2C_NUM_0, g.pins.groveA, g.pins.groveB);
    n += testBus(r, M5.Ex_I2C, "Grv", false);
    M5.Ex_I2C.release();
    if (!n) add(r, FAIL, "I2C device", "nothing answered. Check SDA, SCL, power");
}

void wiringI2c(Wiring& w)
{
    wire(w, "VCC / VIN", "3.3V or 5V, see module", WARN);
    wire(w, "GND", "EXT 4  GND");
    wire(w, "SDA", "EXT 8  G8");
    wire(w, "SCL", "EXT 10  G9");
    wire(w, "5V source", "EXT 6  5V OUT", DIM);
    wire(w, "or Grove port", "SDA yel G2, SCL wht G1", DIM);
    wire(w, "tests", "BME/BMP280 SHT3x/4x", DIM);
    wire(w, "", "AHT10/20 SCD4x", DIM);
}

// ------------------------------------------------------------ PN532

constexpr uint8_t PN_ADDR = 0x24;

struct Pn {
    m5::I2C_Class* i2c = nullptr;  // null means HSU (UART) on Serial1
};

// Finds a PN532 frame in `b`. Returns the payload length (TFI onwards),
// 0 for an ACK frame, -1 for a bad checksum, -2 if more bytes are needed.
int pnParse(const uint8_t* b, int n, uint8_t* out, int max)
{
    for (int i = 0; i + 3 < n; i++) {
        if (b[i] != 0x00 || b[i + 1] != 0xFF) continue;
        uint8_t len = b[i + 2], lcs = b[i + 3];
        if ((uint8_t)(len + lcs) != 0) continue;
        if (len == 0) return 0;
        if (i + 4 + len + 1 > n) return -2;
        uint8_t sum = 0;
        for (int k = 0; k <= len; k++) sum += b[i + 4 + k];
        if (sum != 0) return -1;
        int m = min<int>(len, max);
        memcpy(out, b + i + 4, m);
        return m;
    }
    return -2;
}

// Reads one frame. `want` is how many bytes to clock out on I2C.
int pnRead(Pn& pn, uint8_t* out, int max, int want, uint32_t timeoutMs)
{
    uint8_t b[48];
    int n = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        if (pn.i2c) {
            bool ok = pn.i2c->start(PN_ADDR, true, I2C_HZ) && pn.i2c->read(b, want, true);
            pn.i2c->stop();
            if (ok && (b[0] & 1)) {  // first byte is the ready flag
                int r = pnParse(b + 1, want - 1, out, max);
                return r == -2 ? -1 : r;
            }
            delay(5);
        } else {
            while (Serial1.available() && n < (int)sizeof(b)) {
                b[n++] = Serial1.read();
                int r = pnParse(b, n, out, max);
                if (r != -2) return r;
            }
            delay(1);
        }
    }
    return -3;
}

void pnSendRaw(Pn& pn, const uint8_t* f, int n)
{
    if (pn.i2c) {
        pn.i2c->start(PN_ADDR, false, I2C_HZ) && pn.i2c->write(f, n);
        pn.i2c->stop();
    } else {
        Serial1.write(f, n);
        Serial1.flush();
    }
}

// Sends a command and returns the reply data length (after the response
// code), or -1 on no ACK, timeout or a wrong reply.
int pnCmd(Pn& pn, const uint8_t* cmd, uint8_t n, uint8_t* out, int max, uint32_t timeoutMs)
{
    uint8_t f[40];
    int k    = 0;
    f[k++]   = 0x00;
    f[k++]   = 0x00;
    f[k++]   = 0xFF;
    f[k++]   = n + 1;
    f[k++]   = (uint8_t)(0 - (n + 1));
    f[k++]   = 0xD4;
    uint8_t sum = 0xD4;
    for (uint8_t i = 0; i < n; i++) {
        f[k++] = cmd[i];
        sum += cmd[i];
    }
    f[k++] = (uint8_t)(0 - sum);
    f[k++] = 0x00;
    if (!pn.i2c)
        while (Serial1.available()) Serial1.read();
    pnSendRaw(pn, f, k);

    uint8_t tmp[40];
    if (pnRead(pn, tmp, sizeof(tmp), 7, 150) != 0) return -1;
    int r = pnRead(pn, tmp, sizeof(tmp), 40, timeoutMs);
    if (r < 2 || tmp[0] != 0xD5 || tmp[1] != (uint8_t)(cmd[0] + 1)) return -1;
    int m = min(r - 2, max);
    memcpy(out, tmp + 2, m);
    return m;
}

void runPn532(Results& r)
{
    static const uint8_t GET_FW[] = {0x02};
    Pn pn;
    String where;
    uint8_t d[32];
    int n = -1;

    busy("PN532 test", "looking on I2C...");
    M5.Ex_I2C.begin(I2C_NUM_0, g.pins.groveA, g.pins.groveB);  // port 0 only, see runI2c
    if (M5.In_I2C.scanID(PN_ADDR, I2C_HZ)) {
        pn.i2c = &M5.In_I2C;
        where  = "I2C on EXT";
    } else if (M5.Ex_I2C.scanID(PN_ADDR, I2C_HZ)) {
        pn.i2c = &M5.Ex_I2C;
        where  = "I2C on Grove";
    }
    if (pn.i2c) {
        for (int i = 0; i < 3 && n < 0; i++) n = pnCmd(pn, GET_FW, 1, d, sizeof(d), 300);
    } else {
        M5.Ex_I2C.release();
        busy("PN532 test", "looking on UART...");
        struct Try { UartPort port; bool swap; const char* name; } tries[] = {
            {PORT_EXT, false, "UART on EXT"}, {PORT_EXT, true, "UART on EXT, RX/TX swapped"},
            {PORT_GROVE, false, "UART on Grove"}, {PORT_GROVE, true, "UART on Grove, RX/TX swapped"},
        };
        static const uint8_t WAKE[16] = {0x55, 0x55};
        for (auto& t : tries) {
            int rx, tx;
            uartPins(t.port, t.swap, rx, tx);
            Serial1.end();
            Serial1.begin(115200, SERIAL_8N1, rx, tx);
            Serial1.write(WAKE, sizeof(WAKE));
            Serial1.flush();
            delay(5);
            n = pnCmd(pn, GET_FW, 1, d, sizeof(d), 200);
            if (n >= 0) { where = t.name; break; }
        }
    }
    auto finish = [&]() {
        if (pn.i2c) M5.Ex_I2C.release();
        else Serial1.end();
    };
    if (n < 0) {
        add(r, FAIL, "PN532 link", pn.i2c ? "answers at 0x24 but no valid reply" : "no reply on I2C 0x24 or UART. Check mode switch, wires");
        finish();
        return;
    }
    add(r, PASS, "Data wires", where);

    char buf[48];
    if (n >= 3 && d[0] == 0x32) {
        snprintf(buf, sizeof(buf), "PN532 firmware %d.%d", d[1], d[2]);
        add(r, PASS, "Chip ID", buf);
    } else {
        snprintf(buf, sizeof(buf), "IC 0x%02X, expected 0x32", d[0]);
        add(r, FAIL, "Chip ID", buf);
    }

    static const uint8_t SAM[] = {0x14, 0x01, 0x14, 0x01};  // normal mode
    add(r, pnCmd(pn, SAM, 4, d, sizeof(d), 300) >= 0 ? PASS : FAIL, "SAM configuration", "normal mode");

    static const uint8_t ROM[] = {0x00, 0x01}, RAM[] = {0x00, 0x02};  // Diagnose
    int rom = pnCmd(pn, ROM, 2, d, sizeof(d), 1500);
    uint8_t romRes = d[0];
    int ram = pnCmd(pn, RAM, 2, d, sizeof(d), 1500);
    if (rom >= 1 && ram >= 1 && romRes == 0x00 && d[0] == 0x00) add(r, PASS, "ROM + RAM self-test", "both ok");
    else add(r, WARNING, "ROM + RAM self-test", "unexpected reply from chip");

    busy("PN532 test", "hold a tag on the antenna (3 s)");
    static const uint8_t LIST[] = {0x4A, 0x01, 0x00};  // one ISO14443A target
    n = pnCmd(pn, LIST, 3, d, sizeof(d), 3000);
    if (n >= 6 && d[0] >= 1) {
        String uid;
        for (int i = 0; i < d[5] && 6 + i < n; i++) {
            snprintf(buf, sizeof(buf), "%02X", d[6 + i]);
            uid += buf;
        }
        add(r, PASS, "RF field + tag read", "UID " + uid);
    } else {
        static const uint8_t ACK[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};  // cancels the search
        pnSendRaw(pn, ACK, sizeof(ACK));
        add(r, INFO, "RF field + tag read", "no tag seen. Hold a card on it and re-run");
    }
    finish();
}

void wiringPn532(Wiring& w)
{
    const PinConfig& p = g.pins;
    wire(w, "VCC", "EXT 6  5V OUT");
    wire(w, "GND", "EXT 4  GND");
    wire(w, "I2C: SDA", "EXT 8  G8");
    wire(w, "I2C: SCL", "EXT 10  G9");
    wire(w, "UART: TXD", hostPin(p.uartRx));
    wire(w, "UART: RXD", hostPin(p.uartTx));
    wire(w, "or Grove port", "I2C or UART, auto", DIM);
    wire(w, "mode switch", "set to I2C or HSU", WARN);
    wire(w, "3.3V-only board", "use a 3.3V regulator", WARN);
    wire(w, "SPI mode", "not supported", DIM);
}

// ------------------------------------------------------------ ESP32 boards

// Talks to the ROM bootloader (the same serial protocol esptool uses), so it
// works whatever firmware is on the board and changes nothing on it.

void slipSend(const uint8_t* d, size_t n)
{
    Serial1.write(0xC0);
    for (size_t i = 0; i < n; i++) {
        if (d[i] == 0xC0) { Serial1.write(0xDB); Serial1.write(0xDC); }
        else if (d[i] == 0xDB) { Serial1.write(0xDB); Serial1.write(0xDD); }
        else Serial1.write(d[i]);
    }
    Serial1.write(0xC0);
    Serial1.flush();
}

int slipRecv(uint8_t* out, int max, uint32_t timeoutMs)
{
    int n = -1;  // -1 until the opening 0xC0
    bool esc = false;
    uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        if (!Serial1.available()) { delay(1); continue; }
        uint8_t c = Serial1.read();
        if (c == 0xC0) {
            if (n > 0) return n;
            n = 0;
            continue;
        }
        if (n < 0) continue;
        if (esc) { c = c == 0xDC ? 0xC0 : 0xDB; esc = false; }
        else if (c == 0xDB) { esc = true; continue; }
        if (n < max) out[n++] = c;
    }
    return -1;
}

// Sends one bootloader command. On success returns the body length and fills
// `value` (the 32-bit header field) and `body`.
int espCmd(uint8_t op, const uint8_t* data, uint16_t len, uint32_t& value, uint8_t* body, int max, uint32_t timeoutMs)
{
    uint8_t pkt[64] = {0x00, op, (uint8_t)len, (uint8_t)(len >> 8), 0, 0, 0, 0};
    memcpy(pkt + 8, data, len);
    slipSend(pkt, 8 + len);
    uint8_t in[96];
    uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        int n = slipRecv(in, sizeof(in), timeoutMs);
        if (n < 8 || in[0] != 0x01 || in[1] != op) continue;
        value   = in[4] | (in[5] << 8) | (in[6] << 16) | ((uint32_t)in[7] << 24);
        int blen = min(n - 8, max);
        memcpy(body, in + 8, blen);
        return blen;
    }
    return -1;
}

bool espSync()
{
    uint8_t sync[36] = {0x07, 0x07, 0x12, 0x20};
    memset(sync + 4, 0x55, 32);
    uint32_t v;
    uint8_t body[16];
    for (int i = 0; i < 6; i++) {
        if (espCmd(0x08, sync, sizeof(sync), v, body, sizeof(body), 120) >= 0) {
            uint32_t t0 = millis();  // the ROM answers a sync several times over
            while (millis() - t0 < 150)
                while (Serial1.available()) Serial1.read();
            return true;
        }
    }
    return false;
}

bool espReadReg(uint32_t addr, uint32_t& value)
{
    uint8_t d[4] = {(uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16), (uint8_t)(addr >> 24)}, body[8];
    int n = espCmd(0x0A, d, 4, value, body, sizeof(body), 300);
    return n >= 2 && body[0] == 0;
}

struct EspChip {
    const char* name;
    uint32_t macReg;  // efuse word holding the low 4 MAC bytes; 0 = not read
};

// Open-drain style: pull the line low or let go. Never drives it high.
void lineLow(int pin)
{
    pinMode(pin, OUTPUT_OPEN_DRAIN);
    digitalWrite(pin, LOW);
}
void lineRelease(int pin) { pinMode(pin, INPUT); }

// Resets the target (EN on the RST pin) with BOOT (on the IRQ pin) held low or
// released, and returns whatever the ROM printed afterwards.
String espReset(bool download)
{
    const PinConfig& p = g.pins;
    if (download) lineLow(p.irq);
    lineLow(p.rst);
    delay(100);
    while (Serial1.available()) Serial1.read();
    lineRelease(p.rst);
    String banner;
    uint32_t t0 = millis();
    while (millis() - t0 < 350) {
        while (Serial1.available() && banner.length() < 300) {
            char c = Serial1.read();
            if (c >= 32 && c < 127) banner += c;
        }
        delay(1);
    }
    lineRelease(p.irq);
    return banner;
}

void runEsp32(Results& r)
{
    const PinConfig& p = g.pins;
    M5.Ex_I2C.release();
    Serial1.setRxBufferSize(1024);
    bool synced = false, swapped = false;
    String banner;

    busy("ESP32 test", "resetting into bootloader...");
    for (int attempt = 0; attempt < 2 && !synced; attempt++) {
        swapped = attempt == 1;
        Serial1.end();
        Serial1.begin(115200, SERIAL_8N1, swapped ? p.uartTx : p.uartRx, swapped ? p.uartRx : p.uartTx);
        String b = espReset(true);
        if (b.length() > banner.length()) banner = b;
        synced = espSync();
    }
    if (!synced) {
        // EN/BOOT may not be wired: let the user do it by hand.
        message("ESP32 test", "No bootloader reply yet.\n\nOn the board: hold BOOT, tap\nRESET, release BOOT.\n\nThen press any key.", WARN);
        busy("ESP32 test", "trying again...");
        for (int attempt = 0; attempt < 2 && !synced; attempt++) {
            swapped = attempt == 1;
            Serial1.end();
            Serial1.begin(115200, SERIAL_8N1, swapped ? p.uartTx : p.uartRx, swapped ? p.uartRx : p.uartTx);
            synced = espSync();
        }
    }

    bool sawBanner   = banner.length() > 8;
    bool sawDownload = banner.indexOf("ownload") >= 0 || banner.indexOf("DOWNLOAD") >= 0;
    if (synced) {
        add(r, PASS, "Board TX + RX wires", "bootloader answered at 115200");
        if (swapped) add(r, WARNING, "TX/RX order", "works only swapped. Board TX is on our TX pin");
        if (sawDownload) add(r, PASS, "EN + BOOT wires", "auto-reset into download mode");
        else add(r, INFO, "EN + BOOT wires", "not proven. Download mode was entered by hand");
    } else {
        if (sawBanner) {
            add(r, PASS, "Board TX wire", "boot text received");
            if (sawDownload) add(r, FAIL, "Board RX wire", "in download mode but no reply. Check RX wire");
            else add(r, FAIL, "Download mode", "board booted normally. BOOT not held low");
        } else {
            add(r, FAIL, "Serial link", "nothing received. Check power, GND, TX wire");
        }
        Serial1.end();
        lineRelease(p.rst);
        lineRelease(p.irq);
        return;
    }

    // Identify the chip: newer ROMs report a chip ID, older ones are told
    // apart by a magic word in ROM.
    static const struct { uint32_t id; EspChip chip; } BY_ID[] = {
        {2, {"ESP32-S2", 0x3F41A044}},  {5, {"ESP32-C3", 0x60008844}},  {9, {"ESP32-S3", 0x60007044}},
        {12, {"ESP32-C2", 0}},          {13, {"ESP32-C6", 0x600B0844}}, {16, {"ESP32-H2", 0x600B0844}},
        {18, {"ESP32-P4", 0}},          {20, {"ESP32-C61", 0}},         {23, {"ESP32-C5", 0}},
    };
    static const struct { uint32_t magic; EspChip chip; } BY_MAGIC[] = {
        {0x00F01D83, {"ESP32", 0x3FF5A004}},    {0x000007C6, {"ESP32-S2", 0x3F41A044}},
        {0xFFF0C101, {"ESP8266", 0}},           {0x6921506F, {"ESP32-C3", 0x60008844}},
        {0x1B31506F, {"ESP32-C3", 0x60008844}}, {0x00000009, {"ESP32-S3", 0x60007044}},
        {0x2CE0806F, {"ESP32-C6", 0x600B0844}},
    };
    const EspChip* chip = nullptr;
    char buf[48];
    uint32_t v = 0;
    uint8_t body[32];
    int n = espCmd(0x14, nullptr, 0, v, body, sizeof(body), 300);  // GET_SECURITY_INFO
    uint32_t chipId = 0xFFFFFFFF;
    if (n >= 24 && body[n - 4] == 0) chipId = body[12] | (body[13] << 8) | (body[14] << 16) | ((uint32_t)body[15] << 24);
    for (auto& e : BY_ID)
        if (e.id == chipId) chip = &e.chip;
    uint32_t magic = 0;
    if (!chip && espReadReg(0x40001000, magic))
        for (auto& e : BY_MAGIC)
            if (e.magic == magic) chip = &e.chip;

    if (chip) add(r, PASS, "Chip", chip->name);
    else if (chipId != 0xFFFFFFFF) {
        snprintf(buf, sizeof(buf), "unlisted Espressif chip, ID %lu", (unsigned long)chipId);
        add(r, PASS, "Chip", buf);
    } else {
        snprintf(buf, sizeof(buf), "not recognised, magic 0x%08lX", (unsigned long)magic);
        add(r, WARNING, "Chip", buf);
    }

    uint32_t lo = 0, hi = 0;
    if (chip && chip->macReg && espReadReg(chip->macReg, lo) && espReadReg(chip->macReg + 4, hi) && (lo || (hi & 0xFFFF))) {
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", (uint8_t)(hi >> 8), (uint8_t)hi, (uint8_t)(lo >> 24),
                 (uint8_t)(lo >> 16), (uint8_t)(lo >> 8), (uint8_t)lo);
        add(r, PASS, "MAC (efuse read)", buf);
    } else {
        add(r, INFO, "MAC", "not read for this chip");
    }
    add(r, INFO, "Flash, radio, GPIO", "not tested from the bootloader");

    espReset(false);  // back to normal boot, if EN is wired
    Serial1.end();
    lineRelease(p.rst);
    lineRelease(p.irq);
}

void wiringEsp32(Wiring& w)
{
    const PinConfig& p = g.pins;
    wire(w, "GND", "EXT 4  GND");
    wire(w, "TX (UART0)", hostPin(p.uartRx));
    wire(w, "RX (UART0)", hostPin(p.uartTx));
    wire(w, "EN / RST", hostPin(p.rst));
    wire(w, "BOOT strap", hostPin(p.irq));
    wire(w, "5V / VIN", "EXT 6, or its own USB", DIM);
    wire(w, "EN + BOOT", "optional. Else by hand", DIM);
    wire(w, "BOOT strap is", "G0, or G9 on C3/C6", DIM);
    wire(w, "", "G28 on C5, G35 on P4", DIM);
    wire(w, "never 5V on", "TX, RX, EN or BOOT", WARN);
}

// ------------------------------------------------------------ supply voltage

void runVoltage(Results& r)
{
    busy("Voltage test", "looking for Unit VMeter...");
    const char* where = "";
    bool cal          = false;
    if (!vmeterBegin(&where, &cal)) {
        add(r, FAIL, "Unit VMeter", "not found. Plug it into the Grove port");
        return;
    }
    add(r, cal ? PASS : WARNING, "Unit VMeter", String(where) + " port, " + (cal ? "factory calibration ok" : "no valid calibration"));

    busy("Voltage test", "measuring...");
    float sum = 0, lo = 0, hi = 0, v = 0;
    int n = 0;
    for (int i = 0; i < 20; i++) {
        if (!vmeterRead(v)) continue;
        if (!n || v < lo) lo = v;
        if (!n || v > hi) hi = v;
        sum += v;
        n++;
    }
    vmeterEnd();
    if (!n) {
        add(r, FAIL, "Rail voltage", "could not read the unit");
        return;
    }
    v = sum / n;
    bool ok;
    const char* rail = vmeterRail(v, &ok);
    char buf[48];
    snprintf(buf, sizeof(buf), "%.3f V: %s", fabsf(v), rail);
    add(r, ok ? PASS : FAIL, "Rail voltage", fabsf(v) < 0.2f ? String(buf) + ". Probes connected?" : String(buf));
    if (v < -0.2f) add(r, WARNING, "Polarity", "probes are reversed");
    float span = (hi - lo) * 1000;
    snprintf(buf, sizeof(buf), "%.0f mV over %d samples", span, n);
    add(r, span > 100 ? WARNING : PASS, "Stability", buf);
}

void wiringVoltage(Wiring& w)
{
    wire(w, "Unit VMeter", "Grove port");
    wire(w, "+ probe", "rail under test (VCC)");
    wire(w, "- probe", "GND of that rail");
    wire(w, "passes on", "3.3V +/-5%, 5V 4.5-5.25", DIM);
    wire(w, "input limit", "+/-36 V", WARN);
    wire(w, "check 3.3V", "before fitting a module", DIM);
}

// ------------------------------------------------------------ screens

const Profile PROFILES[] = {
    {"Supply voltage", wiringVoltage, runVoltage},
    {"nRF24L01+ / E01", wiringNrf24, runNrf24},
    {"CC1101 8-pin", wiringCC1101, runCC1101},
    {"Ebyte E07 (CC1101)", wiringE07, runCC1101},
    {"GPS 4-pin", wiringGps, runGps},
    {"I2C env sensor", wiringI2c, runI2c},
    {"PN532 NFC", wiringPn532, runPn532},
    {"ESP32 board / Stamp C5", wiringEsp32, runEsp32},
};
constexpr int NPROFILES = sizeof(PROFILES) / sizeof(PROFILES[0]);

// Returns true to run the test, false to go back.
bool wiringScreen(const Profile& p)
{
    Wiring w;
    p.wiring(w);
    constexpr int VISIBLE = ROWS - 1;
    int top = 0, n = w.size();
    for (;;) {
        frame(p.name, n > VISIBLE ? "ENT run test  ;/. scroll  ` back" : "ENT run test  ` back");
        text(4, TOP, "MODULE PIN", TEAL);
        text(92, TOP, "CARDPUTER", TEAL);
        for (int i = 0; i < VISIBLE && top + i < n; i++) {
            const WireRow& row = w[top + i];
            int y = TOP + (i + 1) * LINE;
            text(4, y, row.mod.c_str(), row.color == FG ? ACCENT : row.color);
            text(92, y, row.host.c_str(), row.color);
        }
        push();
        Key k;
        while (!(k = poll())) delay(10);
        if (k.type == K_BACK) return false;
        if (k.type == K_ENTER) return true;
        if (k.type == K_UP && top > 0) top--;
        if (k.type == K_DOWN && top + VISIBLE < n) top++;
    }
}

struct Line {
    String s;
    uint16_t color;
};

enum Action { ACT_BACK, ACT_RERUN, ACT_WIRING };

Action resultsScreen(const Profile& p, const Results& res, bool pass, int runs, int passes)
{
    static const char* const TAG[] = {" OK ", "FAIL", "WARN", "info"};
    static const uint16_t COL[]    = {GOOD, BAD, WARN, DIM};
    std::vector<Line> lines;
    for (auto& r : res) {
        lines.push_back({String(TAG[r.st]) + " " + r.name, COL[r.st]});
        String d = r.detail;
        while (d.length()) {  // wrap the detail at 34 columns
            int cut = d.length() > 34 ? d.lastIndexOf(' ', 34) : (int)d.length();
            if (cut <= 0) cut = 34;
            lines.push_back({"     " + d.substring(0, cut), r.st == FAIL ? FG : DIM});
            d = d.substring(cut);
            d.trim();
        }
    }

    constexpr int LIST_Y = TOP + 24;
    constexpr int VISIBLE = (BOTTOM - LIST_Y) / LINE;
    int top = 0, n = lines.size();
    // start at the first failure so it is visible without scrolling
    if (!pass)
        for (int i = 0; i < n; i++)
            if (lines[i].color == BAD) { top = min(i, max(0, n - VISIBLE)); break; }
    for (;;) {
        frame(p.name, "ENT next module  w wiring  ` back");
        canvas.fillRoundRect(2, TOP, W - 4, 21, 3, pass ? GOOD : BAD);
        canvas.setFont(&fonts::Font4);
        canvas.setTextColor(BG);
        canvas.setCursor(8, TOP);
        canvas.print(pass ? "PASS" : "FAIL");
        canvas.setFont(&fonts::Font0);
        char buf[32];
        snprintf(buf, sizeof(buf), "run %d", runs);
        textRight(W - 8, TOP + 2, buf, BG);
        snprintf(buf, sizeof(buf), "%d pass  %d fail", passes, runs - passes);
        textRight(W - 8, TOP + 11, buf, BG);
        for (int i = 0; i < VISIBLE && top + i < n; i++) text(4, LIST_Y + i * LINE, lines[top + i].s.c_str(), lines[top + i].color);
        if (n > VISIBLE) {
            int th = (BOTTOM - LIST_Y) * VISIBLE / n;
            int ty = LIST_Y + (BOTTOM - LIST_Y - th) * top / (n - VISIBLE);
            canvas.fillRect(W - 3, ty, 2, th, DIM);
        }
        push();
        Key k;
        while (!(k = poll())) delay(10);
        if (k.type == K_BACK) return ACT_BACK;
        if (k.type == K_ENTER) return ACT_RERUN;
        if (k.is('w')) return ACT_WIRING;
        if (k.type == K_UP && top > 0) top--;
        if (k.type == K_DOWN && top + VISIBLE < n) top++;
    }
}

void runProfile(const Profile& p)
{
    int runs = 0, passes = 0;
    for (;;) {
        if (!wiringScreen(p)) return;
        for (;;) {
            Results res;
            p.run(res);
            bool pass = !res.empty();
            for (auto& r : res)
                if (r.st == FAIL) pass = false;
            runs++;
            if (pass) passes++;
            Action a = resultsScreen(p, res, pass, runs, passes);
            if (a == ACT_BACK) return;
            if (a == ACT_WIRING) break;
        }
    }
}

}  // namespace

void toolRig()
{
    static int sel = 0;
    const char* names[NPROFILES];
    for (int i = 0; i < NPROFILES; i++) names[i] = PROFILES[i].name;
    for (;;) {
        int c = menu("Test rig: pick module", names, NPROFILES, sel);
        if (c < 0) return;
        sel = c;
        runProfile(PROFILES[c]);
    }
}
