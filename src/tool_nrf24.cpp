#include "config.h"
#include "radio.h"
#include "tools.h"
#include "ui.h"

using namespace ui;

static const char* const RATES[] = {"250 kbps", "1 Mbps", "2 Mbps"};
static const int16_t RATE_VAL[]  = {250, 1000, 2000};
static const char* const PWRS[]  = {"-18 dBm", "-12 dBm", "-6 dBm", "0 dBm"};
static const int8_t PWR_VAL[]    = {-18, -12, -6, 0};
static const char* const ACKS[]  = {"off", "on"};

static uint8_t PIPE_ADDR[5] = {0x42, 0x45, 0x4E, 0x43, 0x48};  // "BENCH"

struct NrfCfg {
    float channel = 76;
    int rate = 1, pwr = 1, ack = 0;
};

// Classic 2.4 GHz scanner: hop all 126 channels and sample the RPD
// (received power > -64 dBm) bit on each.
static void scanner(nRF24& r)
{
    constexpr int CH = 126, GH = 80;
    float level[CH] = {0}, peak[CH] = {0};
    uint32_t sweeps = 0;
    r.setAutoAck(false);
    r.setBitRate(2000);

    for (;;) {
        for (int pass = 0; pass < 4; pass++) {
            for (int ch = 0; ch < CH; ch++) {
                r.setFrequency(2400 + ch);
                r.startReceive();
                delayMicroseconds(170);
                bool hit = r.isCarrierDetected();
                r.standby();
                level[ch] = level[ch] * 0.85f + (hit ? 0.15f : 0);
                peak[ch]  = max(peak[ch] * 0.995f, level[ch]);
            }
            sweeps++;
        }

        frame("nRF24 2.4 GHz scan", "r reset peaks  ` back");
        int busiest = 0;
        for (int ch = 1; ch < CH; ch++) if (level[ch] > level[busiest]) busiest = ch;
        textf(4, TOP, DIM, "sweeps %lu", (unsigned long)sweeps);
        textf(100, TOP, level[busiest] > 0.05f ? ACCENT : DIM, "busiest ch%d (%d MHz)", busiest, 2400 + busiest);
        int gy = TOP + 12, gx = 2;
        // Wi-Fi channels 1, 6, 11 for orientation
        for (int wch : {12, 37, 62}) {
            int x = gx + wch * 236 / CH;
            canvas.fillRect(x - 11 * 236 / CH, gy, 22 * 236 / CH, GH, 0x10A2);
        }
        for (int ch = 0; ch < CH; ch++) {
            int x  = gx + ch * 236 / CH;
            int w  = max(1, gx + (ch + 1) * 236 / CH - x);
            int h  = constrain((int)(level[ch] * GH * 1.5f), 0, GH);
            int ph = constrain((int)(peak[ch] * GH * 1.5f), 0, GH);
            uint16_t c = level[ch] > 0.4f ? BAD : level[ch] > 0.12f ? WARN : GOOD;
            if (h) canvas.fillRect(x, gy + GH - h, w, h, c);
            if (ph) canvas.drawFastHLine(x, gy + GH - ph, w, ACCENT);
        }
        canvas.drawFastHLine(gx, gy + GH, 236, PANEL);
        text(gx, gy + GH + 3, "2400", DIM);
        text(gx + 12 * 236 / CH - 6, gy + GH + 3, "W1", DIM);
        text(gx + 37 * 236 / CH - 6, gy + GH + 3, "W6", DIM);
        text(gx + 62 * 236 / CH - 9, gy + GH + 3, "W11", DIM);
        textRight(W - 2, gy + GH + 3, "2525", DIM);
        push();

        Key k = poll();
        if (k.type == K_BACK) break;
        if (k.is('r')) for (int i = 0; i < CH; i++) peak[i] = level[i] = 0;
    }
    r.standby();
}

void toolNrf24()
{
    static NrfCfg cfg;
    static int sel = 0;
    Field f[] = {
        Field::num("Channel (2400+n)", &cfg.channel, 1, 0, 125, 0),
        Field::choice("Data rate", &cfg.rate, RATES, 3),
        Field::choice("TX power", &cfg.pwr, PWRS, 4),
        Field::choice("Auto-ACK", &cfg.ack, ACKS, 2),
        Field::action("Detect / chip info"),
        Field::action("2.4 GHz channel scanner"),
        Field::action("RX packets"),
        Field::action("TX test packets"),
    };
    constexpr int FIRST_ACTION = 4;

    for (;;) {
        int a = form("nRF24L01(+)", f, sizeof(f) / sizeof(f[0]), sel);
        if (a < 0) return;

        busy("nRF24", "initialising radio...");
        // nRF24: CS, IRQ, CE  (CE goes in RadioLib's "rst" slot)
        Module* mod = radioModule(g.pins.irq, g.pins.aux, RADIOLIB_NC);
        nRF24 r(mod);
        int16_t st = r.begin(2400 + (int)cfg.channel, RATE_VAL[cfg.rate], PWR_VAL[cfg.pwr], 5);
        if (st == RADIOLIB_ERR_NONE) {
            r.setAutoAck(cfg.ack);
            r.setTransmitPipe(PIPE_ADDR);
            r.setReceivePipe(0, PIPE_ADDR);
        }
        if (st != RADIOLIB_ERR_NONE) {
            String m = String("begin() = ") + st + "\n" + radioErr(st) + "\n\nCS G" + g.pins.cs +
                       "  CE G" + g.pins.aux + "  IRQ G" + g.pins.irq +
                       "\nTip: add 10uF across VCC/GND on the module.";
            message("nRF24: init failed", m.c_str(), BAD);
        } else {
            switch (a - FIRST_ACTION) {
                case 0: {
                    String m = String("nRF24 responding on SPI\nchannel ") + (int)cfg.channel + " (" +
                               (2400 + (int)cfg.channel) + " MHz)\n" + RATES[cfg.rate] + ", " + PWRS[cfg.pwr] +
                               "\naddress 'BENCH', ACK " + ACKS[cfg.ack] +
                               "\n\nNote: 250 kbps fails on non-plus clones.";
                    if (cfg.rate == 0 && r.setBitRate(250) != RADIOLIB_ERR_NONE)
                        m += "\n250 kbps rejected -> not an L01+";
                    message("nRF24: chip OK", m.c_str(), GOOD);
                    break;
                }
                case 1: scanner(r); break;
                case 2: {
                    PacketScreenOps ops;
                    ops.title   = "nRF24 RX";
                    ops.phy     = &r;
                    ops.hasRssi = false;
                    packetRxScreen(ops);
                    break;
                }
                case 3: {
                    PacketScreenOps ops;
                    ops.title    = "nRF24 TX";
                    ops.phy      = &r;
                    ops.transmit = [&r](uint8_t* d, size_t n) {
                        return r.transmit(d, min<size_t>(n, 32), 0);
                    };
                    packetTxScreen(ops);
                    break;
                }
            }
        }
        r.sleep();
        delete mod;
        radioPinsRelease();
    }
}
