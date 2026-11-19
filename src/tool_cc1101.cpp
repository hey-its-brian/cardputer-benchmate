#include "config.h"
#include "radio.h"
#include "tools.h"
#include "ui.h"

using namespace ui;

static const char* const SPANS[]  = {"1 MHz", "2 MHz", "5 MHz", "10 MHz", "20 MHz"};
static const float SPAN_VAL[]     = {1, 2, 5, 10, 20};
static const char* const BRS[]    = {"1.2 kbps", "4.8 kbps", "38.4 kbps", "100 kbps"};
static const float BR_VAL[]       = {1.2f, 4.8f, 38.4f, 100.0f};

struct CcCfg {
    float freq  = 433.92f;
    int span    = 2;
    int br      = 1;
    float power = 0;
};

static bool freqValid(float f)
{
    return (f >= 300 && f <= 348) || (f >= 387 && f <= 464) || (f >= 779 && f <= 928);
}

// Sweeps center +/- span/2 reading the CC1101's RSSI register in direct RX mode.
static void spectrum(CC1101& r, CcCfg& c)
{
    constexpr int BINS = 116, GH = 78;
    float cur[BINS], peak[BINS];
    for (int i = 0; i < BINS; i++) cur[i] = peak[i] = -120;
    r.setRxBandwidth(SPAN_VAL[c.span] >= 10 ? 325 : 162);

    for (;;) {
        float span = SPAN_VAL[c.span];
        float f0   = c.freq - span / 2;
        float best = -200, bestF = c.freq;
        for (int i = 0; i < BINS; i++) {
            float f = f0 + span * i / (BINS - 1);
            if (!freqValid(f)) { cur[i] = -120; continue; }
            r.setFrequency(f);
            r.receiveDirectAsync();
            delayMicroseconds(700);
            float v = r.getRSSI();
            cur[i]  = v;
            peak[i] = max(peak[i] - 0.4f, v);
            if (v > best) { best = v; bestF = f; }
        }

        frame("CC1101 spectrum", ",/ freq  ;. span  r reset  ` back");
        textf(4, TOP, FG, "%.3f MHz +/-%.1f", c.freq, span / 2);
        textf(140, TOP, rssiColor(best), "pk %.0f@%.2f", best, bestF);
        int gy = TOP + 12, gx = 4;
        canvas.drawRect(gx - 1, gy - 1, BINS * 2 + 2, GH + 2, PANEL);
        canvas.drawFastVLine(gx + BINS, gy, GH, PANEL);  // center marker
        for (int i = 0; i < BINS; i++) {
            int h  = constrain((int)((cur[i] + 120) * GH / 90), 0, GH);   // -120..-30 dBm
            int ph = constrain((int)((peak[i] + 120) * GH / 90), 0, GH);
            canvas.fillRect(gx + i * 2, gy + GH - h, 2, h, rssiColor(cur[i]));
            canvas.drawFastHLine(gx + i * 2, gy + GH - ph, 2, ACCENT);
        }
        textf(4, gy + GH + 3, DIM, "%.2f", f0);
        textRight(W - 4, gy + GH + 3, String(f0 + span, 2).c_str(), DIM);
        push();

        Key k = poll();
        if (k.type == K_BACK) break;
        if (k.type == K_LEFT) c.freq -= span / 4;
        if (k.type == K_RIGHT) c.freq += span / 4;
        if (k.type == K_UP) c.span = min(c.span + 1, 4);
        if (k.type == K_DOWN) c.span = max(c.span - 1, 0);
        if (k.is('r') || k.type == K_UP || k.type == K_DOWN || k.type == K_LEFT || k.type == K_RIGHT)
            for (auto& p : peak) p = -120;
    }
    r.packetMode();
    r.setPromiscuousMode(false);
    r.standby();
}

static void rssiMeter(CC1101& r, float freq)
{
    constexpr int GW = W - 8, GH = 72;
    float hist[GW];
    for (auto& h : hist) h = NAN;
    int head = 0;
    float mx = -200;
    r.setFrequency(freq);
    r.receiveDirectAsync();
    for (;;) {
        float v = r.getRSSI();
        hist[head] = v;
        head = (head + 1) % GW;
        mx = max(mx, v);
        frame("CC1101 RSSI", "r reset peak  ` back");
        textf(4, TOP, FG, "%.3f MHz", freq);
        textf(100, TOP, rssiColor(v), "%6.1f dBm", v);
        textf(176, TOP, ACCENT, "pk %.0f", mx);
        int gy = TOP + 14;
        canvas.drawRect(3, gy - 1, GW + 2, GH + 2, PANEL);
        for (int i = 0; i < GW; i++) {
            float s = hist[(head + i) % GW];
            if (isnan(s)) continue;
            int h = constrain((int)((s + 120) * GH / 90), 0, GH);
            canvas.drawFastVLine(4 + i, gy + GH - h, h, rssiColor(s));
        }
        push();
        Key k = poll();
        if (k.type == K_BACK) break;
        if (k.is('r')) mx = -200;
        delay(20);
    }
    r.packetMode();
    r.setPromiscuousMode(false);
    r.standby();
}

void toolCC1101()
{
    static CcCfg cfg;
    static bool first = true;
    if (first) { cfg.freq = g.subGhzFreq; first = false; }
    static int sel = 0;

    Field f[] = {
        Field::num("Freq MHz", &cfg.freq, 0.05f, 300, 928, 3),
        Field::choice("Sweep span", &cfg.span, SPANS, 5),
        Field::choice("Packet bitrate", &cfg.br, BRS, 4),
        Field::num("TX power dBm", &cfg.power, 1, -30, 10, 0),
        Field::action("Detect / chip info"),
        Field::action("Spectrum sweep"),
        Field::action("RSSI meter"),
        Field::action("RX packets (2-FSK)"),
        Field::action("TX test packets"),
    };
    constexpr int FIRST_ACTION = 4;

    for (;;) {
        int a = form("CC1101 sub-GHz", f, sizeof(f) / sizeof(f[0]), sel);
        if (a < 0) return;
        if (!freqValid(cfg.freq)) {
            message("CC1101", "Frequency outside CC1101 bands:\n300-348, 387-464, 779-928 MHz", WARN);
            continue;
        }

        busy("CC1101", "initialising radio...");
        Module* mod = radioModule(g.pins.irq, RADIOLIB_NC, g.pins.aux);
        CC1101 r(mod);
        int8_t pwr = (int8_t)cfg.power;
        int16_t st = r.begin(cfg.freq, BR_VAL[cfg.br], 5.0f, 135.0f, pwr, 16);
        if (st == RADIOLIB_ERR_INVALID_OUTPUT_POWER) {
            // CC1101 only supports -30/-20/-15/-10/0/5/7/10 dBm
            pwr = 0;
            st  = r.begin(cfg.freq, BR_VAL[cfg.br], 5.0f, 135.0f, pwr, 16);
        }
        if (st != RADIOLIB_ERR_NONE) {
            String m = String("begin() = ") + st + "\n" + radioErr(st) + "\n\nCS G" + g.pins.cs +
                       "  GDO0 G" + g.pins.irq + "  GDO2 G" + g.pins.aux +
                       "\nVCC must be 3.3 V (not 5 V)";
            message("CC1101: init failed", m.c_str(), BAD);
        } else {
            switch (a - FIRST_ACTION) {
                case 0: {
                    String m = "CC1101 responding\nchip version 0x" + String(r.getChipVersion(), HEX) +
                               "\nTX power " + pwr + " dBm\nRSSI now: ";
                    r.receiveDirectAsync();
                    delay(2);
                    m += String(r.getRSSI(), 1) + " dBm";
                    r.packetMode();
                    r.setPromiscuousMode(false);
                    message("CC1101: chip OK", m.c_str(), GOOD);
                    break;
                }
                case 1: spectrum(r, cfg); break;
                case 2: rssiMeter(r, cfg.freq); break;
                case 3: {
                    PacketScreenOps ops;
                    ops.title = "CC1101 RX";
                    ops.phy   = &r;
                    packetRxScreen(ops);
                    break;
                }
                case 4: {
                    PacketScreenOps ops;
                    ops.title = "CC1101 TX";
                    ops.phy   = &r;
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
