#include "config.h"
#include <Preferences.h>
#include <SPI.h>

Settings g;

static constexpr uint16_t SETTINGS_VERSION = 2;

void settingsLoad()
{
    Preferences p;
    if (!p.begin("benchmate", true)) return;
    Settings tmp;
    if (p.getBytesLength("cfg") == sizeof(Settings)) {
        p.getBytes("cfg", &tmp, sizeof(Settings));
        if (tmp.version == SETTINGS_VERSION) g = tmp;
    }
    p.end();
}

void settingsSave()
{
    Preferences p;
    if (!p.begin("benchmate", false)) return;
    g.version = SETTINGS_VERSION;
    p.putBytes("cfg", &g, sizeof(Settings));
    p.end();
}

void settingsReset()
{
    g = Settings();
}

void radioSpiBegin()
{
    static bool started = false;
    static PinConfig last;
    const PinConfig& p = g.pins;

    pinMode(p.sdCs, OUTPUT);
    digitalWrite(p.sdCs, HIGH);
    pinMode(p.cs, OUTPUT);
    digitalWrite(p.cs, HIGH);

    if (started && last.sck == p.sck && last.miso == p.miso && last.mosi == p.mosi) return;
    if (started) SPI.end();
    SPI.begin(p.sck, p.miso, p.mosi, -1);
    started = true;
    last    = p;
}

void radioPinsRelease()
{
    const PinConfig& p = g.pins;
    detachInterrupt(p.irq);
    detachInterrupt(p.aux);
    pinMode(p.irq, INPUT);
    pinMode(p.aux, INPUT);
    pinMode(p.rst, INPUT);
    pinMode(p.cs, OUTPUT);
    digitalWrite(p.cs, HIGH);
}

void uartPins(UartPort port, bool swap, int& rx, int& tx)
{
    if (port == PORT_EXT) {
        rx = g.pins.uartRx;
        tx = g.pins.uartTx;
    } else {
        rx = g.pins.groveB;
        tx = g.pins.groveA;
    }
    if (swap) std::swap(rx, tx);
}
