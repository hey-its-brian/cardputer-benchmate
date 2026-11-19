#include "radio.h"
#include "config.h"
#include "ui.h"

using namespace ui;

RadioLibHal* radioHal()
{
    static ArduinoHal hal(SPI, SPISettings(2000000, MSBFIRST, SPI_MODE0));
    return &hal;
}

Module* radioModule(uint32_t irq, uint32_t rst, uint32_t aux)
{
    radioSpiBegin();
    return new Module(radioHal(), g.pins.cs, irq, rst, aux);
}

const char* radioErr(int16_t code)
{
    switch (code) {
        case RADIOLIB_ERR_NONE:                  return "OK";
        case RADIOLIB_ERR_CHIP_NOT_FOUND:        return "chip not found (check wiring/CS)";
        case RADIOLIB_ERR_PACKET_TOO_LONG:       return "packet too long";
        case RADIOLIB_ERR_TX_TIMEOUT:            return "TX timeout (IRQ pin?)";
        case RADIOLIB_ERR_RX_TIMEOUT:            return "RX timeout";
        case RADIOLIB_ERR_CRC_MISMATCH:          return "CRC mismatch";
        case RADIOLIB_ERR_INVALID_BANDWIDTH:     return "invalid bandwidth";
        case RADIOLIB_ERR_INVALID_SPREADING_FACTOR: return "invalid SF";
        case RADIOLIB_ERR_INVALID_CODING_RATE:   return "invalid coding rate";
        case RADIOLIB_ERR_INVALID_FREQUENCY:     return "freq out of range for chip";
        case RADIOLIB_ERR_INVALID_OUTPUT_POWER:  return "invalid TX power";
        case RADIOLIB_ERR_SPI_WRITE_FAILED:      return "SPI write verify failed";
        case RADIOLIB_ERR_UNSUPPORTED:           return "unsupported by chip";
        case RADIOLIB_ERR_INVALID_BIT_RATE:      return "invalid bit rate";
        case RADIOLIB_ERR_INVALID_RX_BANDWIDTH:  return "invalid RX bandwidth";
        case RADIOLIB_ERR_ACK_NOT_RECEIVED:      return "no ACK from receiver";
        case RADIOLIB_ERR_SPI_CMD_TIMEOUT:       return "BUSY timeout (BUSY pin?)";
        case RADIOLIB_ERR_SPI_CMD_INVALID:       return "SPI cmd invalid";
        case RADIOLIB_ERR_SPI_CMD_FAILED:        return "SPI cmd failed (TCXO?)";
        default:                                 return "error";
    }
}

// ---------------------------------------------------------------- RX screen

static volatile bool s_rxFlag = false;
static void ARDUINO_ISR_ATTR onPacket() { s_rxFlag = true; }

struct RxEntry {
    uint32_t n;
    float rssi, snr;
    int16_t state;
    uint8_t len;
    uint8_t data[48];
};

static void renderPayload(char* out, size_t outLen, const RxEntry& e, bool hex)
{
    size_t o = 0;
    for (uint8_t i = 0; i < min<uint8_t>(e.len, sizeof(e.data)) && o + 4 < outLen; i++) {
        uint8_t b = e.data[i];
        if (hex) o += snprintf(out + o, outLen - o, "%02X", b);
        else out[o++] = (b >= 32 && b < 127) ? (char)b : '.';
    }
    out[o] = 0;
}

void packetRxScreen(const PacketScreenOps& ops)
{
    constexpr int LOG = 16;
    RxEntry log[LOG];
    int count = 0, crcErr = 0;
    bool hex = false;
    float live = NAN;
    uint32_t liveAt = 0;

    s_rxFlag = false;
    ops.phy->setPacketReceivedAction(onPacket);
    int16_t st = ops.restartRx ? (ops.restartRx(), 0) : ops.phy->startReceive();
    if (st != RADIOLIB_ERR_NONE) {
        message(ops.title, (String("startReceive: ") + radioErr(st) + " (" + st + ")").c_str(), BAD);
        ops.phy->clearPacketReceivedAction();
        return;
    }

    for (;;) {
        if (s_rxFlag) {
            s_rxFlag   = false;
            RxEntry& e = log[count % LOG];
            size_t len = ops.phy->getPacketLength();
            uint8_t buf[256];
            e.state = ops.phy->readData(buf, min<size_t>(len, sizeof(buf)));
            e.n     = count + 1;
            e.len   = min<size_t>(len, 255);
            memcpy(e.data, buf, min<size_t>(len, sizeof(e.data)));
            e.rssi  = ops.hasRssi ? ops.phy->getRSSI() : NAN;
            e.snr   = ops.hasSnr ? ops.phy->getSNR() : NAN;
            if (e.state == RADIOLIB_ERR_CRC_MISMATCH) crcErr++;
            count++;
            if (ops.restartRx) ops.restartRx();
            else ops.phy->startReceive();
        }
        if (ops.liveRssi && millis() - liveAt > 250) {
            live   = ops.liveRssi();
            liveAt = millis();
        }

        frame(ops.title, "TAB hex/text  c clear  ` back");
        textf(4, TOP, FG, "RX %d", count);
        textf(54, TOP, crcErr ? BAD : DIM, "CRC err %d", crcErr);
        if (!isnan(live)) {
            textf(130, TOP, rssiColor(live), "%6.1f dBm", live);
            bar(4, TOP + 10, W - 8, 3, (live + 130) / 90.0f, rssiColor(live));
        }
        int y = TOP + 16;
        if (!count) text(4, y + 20, "listening...", DIM);
        for (int i = 0; i < min(count, LOG) && y < BOTTOM - 8; i++) {
            const RxEntry& e = log[(count - 1 - i) % LOG];
            char meta[40];
            int m = snprintf(meta, sizeof(meta), "#%lu %uB", (unsigned long)e.n, e.len);
            if (!isnan(e.rssi)) m += snprintf(meta + m, sizeof(meta) - m, " %.0fdBm", e.rssi);
            if (!isnan(e.snr)) snprintf(meta + m, sizeof(meta) - m, " %.1fdB", e.snr);
            text(4, y, meta, e.state == RADIOLIB_ERR_NONE ? TEAL : BAD);
            if (e.state != RADIOLIB_ERR_NONE) {
                textRight(W - 4, y, radioErr(e.state), BAD);
            }
            y += LINE;
            char pl[100];
            renderPayload(pl, sizeof(pl), e, hex);
            pl[38] = 0;
            text(10, y, pl, FG);
            y += LINE + 2;
        }
        push();

        Key k = poll();
        if (k.type == K_BACK) break;
        if (k.type == K_TAB) hex = !hex;
        if (k.is('c')) count = crcErr = 0;
        delay(5);
    }
    ops.phy->clearPacketReceivedAction();
    ops.phy->standby();
}

// ---------------------------------------------------------------- TX screen

void packetTxScreen(const PacketScreenOps& ops)
{
    String msg = "BenchMate";
    uint32_t n = 0, ok = 0, fail = 0;
    int16_t last = 1;  // 1 = nothing sent yet
    uint32_t lastMs = 0, airUs = 0;
    bool autoTx = false;
    uint32_t lastAuto = 0;

    for (;;) {
        bool send = false;
        Key k = poll();
        if (k.type == K_BACK) break;
        if (k.type == K_ENTER) send = true;
        if (k.is('a')) autoTx = !autoTx;
        if (k.is('e')) editText("Payload", msg, 40);
        if (autoTx && millis() - lastAuto > 2000) {
            send     = true;
            lastAuto = millis();
        }

        if (send) {
            n++;
            String p = msg + " #" + n;
            frame(ops.title);
            text(4, TOP + 40, "transmitting...", WARN);
            push();
            uint32_t t0 = millis();
            last = ops.transmit ? ops.transmit((uint8_t*)p.c_str(), p.length())
                                : ops.phy->transmit((uint8_t*)p.c_str(), p.length());
            lastMs = millis() - t0;
            airUs  = ops.phy->getTimeOnAir(p.length());
            (last == RADIOLIB_ERR_NONE ? ok : fail)++;
        }

        frame(ops.title, "ENT send  a auto  e edit  ` back");
        textf(4, TOP + 2, DIM, "payload: ");
        textf(58, TOP + 2, FG, "%s #%lu", msg.c_str(), (unsigned long)(n + 1));
        textf(4, TOP + 16, FG, "sent %lu", (unsigned long)n);
        textf(70, TOP + 16, GOOD, "ok %lu", (unsigned long)ok);
        textf(130, TOP + 16, fail ? BAD : DIM, "fail %lu", (unsigned long)fail);
        if (last != 1) {
            textf(4, TOP + 32, last == RADIOLIB_ERR_NONE ? GOOD : BAD, "last: %s (%d)", radioErr(last), last);
            textf(4, TOP + 44, DIM, "call %lu ms   airtime %.1f ms", (unsigned long)lastMs, airUs / 1000.0f);
        }
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(autoTx ? ACCENT : DIM);
        canvas.setCursor(4, TOP + 66);
        canvas.print(autoTx ? "AUTO TX every 2 s" : "manual");
        canvas.setFont(&fonts::Font0);
        push();
        delay(10);
    }
    ops.phy->standby();
}
