#include "config.h"
#include "radio.h"
#include "tools.h"
#include "ui.h"

using namespace ui;

static const char* const CHIPS[]  = {"SX1262/1268", "SX1276-79"};
static const char* const BWS[]    = {"62.5", "125", "250", "500"};
static const float BW_VAL[]       = {62.5f, 125.0f, 250.0f, 500.0f};
static const char* const SFS[]    = {"7", "8", "9", "10", "11", "12"};
static const char* const CRS[]    = {"4/5", "4/6", "4/7", "4/8"};
static const char* const SYNCS[]  = {"0x12 private", "0x34 LoRaWAN", "0x2B Meshtastic"};
static const uint8_t SYNC_VAL[]   = {0x12, 0x34, 0x2B};
static const char* const TCXOS[]  = {"auto", "1.6 V", "1.8 V", "3.0 V", "none (XTAL)"};
static const float TCXO_VAL[]     = {-1, 1.6f, 1.8f, 3.0f, 0.0f};
static const char* const YESNO[]  = {"no", "yes"};

struct LoraCfg {
    int chip = 0;
    float freq = 915.0f;
    int bw = 1, sf = 2, cr = 0, sync = 0, tcxo = 0, dio2 = 1;
    float power = 10;
};

class LoraSession {
public:
    Module* mod      = nullptr;
    SX1262* sx126    = nullptr;
    SX1276* sx127    = nullptr;
    PhysicalLayer* phy = nullptr;
    float tcxoUsed   = 0;
    String detail;

    ~LoraSession()
    {
        if (sx126) sx126->sleep();
        if (sx127) sx127->sleep();
        delete sx126;
        delete sx127;
        delete mod;
        radioPinsRelease();
    }

    int16_t open(const LoraCfg& c)
    {
        const PinConfig& p = g.pins;
        float bw = BW_VAL[c.bw];
        uint8_t sf = 7 + c.sf, cr = 5 + c.cr;
        int16_t st;
        if (c.chip == 0) {
            mod   = radioModule(p.irq, p.rst, p.aux);
            sx126 = new SX1262(mod);
            phy   = sx126;
            // auto never tries 3.0 V so a 1.8 V TCXO is not over-driven
            float tries[] = {1.8f, 0.0f, 1.6f};
            int ntries    = 3;
            if (TCXO_VAL[c.tcxo] >= 0) {
                tries[0] = TCXO_VAL[c.tcxo];
                ntries   = 1;
            }
            for (int i = 0; i < ntries; i++) {
                tcxoUsed = tries[i];
                st = sx126->begin(c.freq, bw, sf, cr, SYNC_VAL[c.sync], (int8_t)c.power, 8, tcxoUsed);
                if (st == RADIOLIB_ERR_NONE || st == RADIOLIB_ERR_CHIP_NOT_FOUND) break;
            }
            if (st == RADIOLIB_ERR_NONE && c.dio2) sx126->setDio2AsRfSwitch(true);
            if (st == RADIOLIB_ERR_NONE) detail = tcxoUsed > 0 ? "TCXO " + String(tcxoUsed, 1) + " V" : "XTAL";
        } else {
            mod   = radioModule(p.irq, p.rst, p.aux);
            sx127 = new SX1276(mod);
            phy   = sx127;
            st    = sx127->begin(c.freq, bw, sf, cr, SYNC_VAL[c.sync], (int8_t)c.power, 8, 0);
            if (st == RADIOLIB_ERR_NONE) detail = "version 0x" + String(sx127->getChipVersion(), HEX);
        }
        return st;
    }

    float liveRssi()
    {
        if (sx126) return sx126->getRSSI(false);
        if (sx127) return sx127->getRSSI(false, true);
        return NAN;
    }
};

// Live RSSI meter with a scrolling history graph; good for checking the
// antenna path and spotting local noise.
static void noiseFloor(LoraSession& s, float freq)
{
    constexpr int GW = W - 8, GH = 70;
    float hist[GW];
    for (auto& h : hist) h = NAN;
    int head = 0;
    float mn = 0, mx = -200, sum = 0;
    int samples = 0;
    s.phy->startReceive();
    for (;;) {
        float r = s.liveRssi();
        hist[head] = r;
        head = (head + 1) % GW;
        mn = samples ? min(mn, r) : r;
        mx = max(mx, r);
        sum += r;
        samples++;

        frame("LoRa noise floor", "r reset stats  ` back");
        textf(4, TOP, FG, "%.3f MHz", freq);
        textf(110, TOP, rssiColor(r), "now %6.1f dBm", r);
        textf(4, TOP + 10, DIM, "min %.1f  avg %.1f  max %.1f", mn, sum / samples, mx);
        int gy = TOP + 22;
        canvas.drawRect(3, gy - 1, GW + 2, GH + 2, PANEL);
        for (int i = 0; i < GW; i++) {
            float v = hist[(head + i) % GW];
            if (isnan(v)) continue;
            int h = constrain((int)((v + 140) * GH / 100), 0, GH);  // -140..-40 dBm
            canvas.drawFastVLine(4 + i, gy + GH - h, h, rssiColor(v));
        }
        push();
        Key k = poll();
        if (k.type == K_BACK) break;
        if (k.is('r')) { samples = 0; mx = -200; sum = 0; }
        delay(40);
    }
    s.phy->standby();
}

void toolLora()
{
    static LoraCfg cfg;
    static bool first = true;
    if (first) { cfg.freq = g.loraFreq; first = false; }
    static int sel = 0;

    Field f[] = {
        Field::choice("Chip", &cfg.chip, CHIPS, 2),
        Field::num("Freq MHz", &cfg.freq, 0.1f, 137, 1020, 3),
        Field::choice("Bandwidth kHz", &cfg.bw, BWS, 4),
        Field::choice("Spreading factor", &cfg.sf, SFS, 6),
        Field::choice("Coding rate", &cfg.cr, CRS, 4),
        Field::num("TX power dBm", &cfg.power, 1, -9, 22, 0),
        Field::choice("Sync word", &cfg.sync, SYNCS, 3),
        Field::choice("TCXO (SX126x)", &cfg.tcxo, TCXOS, 5),
        Field::choice("DIO2 RF switch", &cfg.dio2, YESNO, 2),
        Field::action("Detect / chip info"),
        Field::action("RX packet monitor"),
        Field::action("TX test packets"),
        Field::action("Noise floor / RSSI"),
    };
    constexpr int FIRST_ACTION = 9;

    for (;;) {
        int a = form("LoRa SX126x / SX127x", f, sizeof(f) / sizeof(f[0]), sel);
        if (a < 0) return;

        busy("LoRa", "initialising radio...");
        LoraSession s;
        int16_t st = s.open(cfg);
        if (st != RADIOLIB_ERR_NONE) {
            String m = String(CHIPS[cfg.chip]) + "\nbegin() = " + st + "\n" + radioErr(st) +
                       "\n\nCS G" + g.pins.cs + "  IRQ G" + g.pins.irq + "  RST G" + g.pins.rst +
                       "  BUSY G" + g.pins.aux;
            message("LoRa: init failed", m.c_str(), BAD);
            continue;
        }
        switch (a - FIRST_ACTION) {
            case 0: {
                String m = String(CHIPS[cfg.chip]) + " responding\n" + s.detail + "\n" +
                           String(cfg.freq, 3) + " MHz  BW " + BWS[cfg.bw] + "  SF" + (7 + cfg.sf) +
                           "\nairtime 16B: " + String(s.phy->getTimeOnAir(16) / 1000.0f, 1) +
                           " ms\nlive RSSI: " + String(s.liveRssi(), 1) + " dBm";
                message("LoRa: chip OK", m.c_str(), GOOD);
                break;
            }
            case 1: {
                PacketScreenOps ops;
                ops.title    = "LoRa RX";
                ops.phy      = s.phy;
                ops.hasSnr   = true;
                ops.liveRssi = [&s]() { return s.liveRssi(); };
                packetRxScreen(ops);
                break;
            }
            case 2: {
                PacketScreenOps ops;
                ops.title = "LoRa TX";
                ops.phy   = s.phy;
                packetTxScreen(ops);
                break;
            }
            case 3: noiseFloor(s, cfg.freq); break;
        }
    }
}
