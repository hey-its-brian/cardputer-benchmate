#include "config.h"
#include "tools.h"
#include "ui.h"

using namespace ui;

static const char* const PORTS[] = {"EXT G13/G15", "Grove G1/G2"};
static const char* const BAUDS[] = {"1200", "2400", "4800", "9600", "19200", "38400", "57600",
                                    "115200", "230400", "460800", "921600"};
static const uint32_t BAUD_VAL[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
static const char* const YESNO[] = {"no", "yes"};
static const char* const EOLS[]  = {"none", "CR", "LF", "CR+LF"};
static const char* const EOL_VAL[] = {"", "\r", "\n", "\r\n"};

static HardwareSerial& uart = Serial1;

static void terminal(int rx, int tx, uint32_t baud, int eol)
{
    constexpr size_t MAX_LINES = 40;
    std::vector<String> lines;
    String cur, input;
    bool hex = false;
    uint32_t rxBytes = 0, txBytes = 0;
    int hexCount = 0;

    auto pushLine = [&]() {
        lines.push_back(cur);
        if (lines.size() > MAX_LINES) lines.erase(lines.begin());
        cur = "";
        hexCount = 0;
    };

    uart.end();
    uart.setRxBufferSize(2048);
    uart.begin(baud, SERIAL_8N1, rx, tx);

    for (;;) {
        while (uart.available()) {
            uint8_t b = uart.read();
            rxBytes++;
            if (hex) {
                char h[4];
                snprintf(h, sizeof(h), "%02X ", b);
                cur += h;
                if (++hexCount >= 13) pushLine();
            } else {
                if (b == '\r') continue;
                if (b == '\n') { pushLine(); continue; }
                cur += (b >= 32 && b < 127) ? (char)b : '.';
                if (cur.length() >= 39) pushLine();
            }
        }

        frame("UART terminal", "ENT send  TAB hex  ^L clear  ` back");
        textRight(W - 40, 3, (String(baud) + (hex ? " HEX" : "")).c_str(), DIM);
        constexpr int VIS = ROWS - 2;
        int y = TOP;
        int total = lines.size() + 1;
        int startIdx = max(0, total - VIS);
        for (int i = startIdx; i < total; i++) {
            const String& s = i < (int)lines.size() ? lines[i] : cur;
            text(1, y, s.c_str(), FG);
            y += LINE;
        }
        canvas.fillRect(0, BOTTOM - 10, W, 11, PANEL);
        String shown = "> " + input + ((millis() / 400) % 2 ? "_" : " ");
        if (shown.length() > 24) shown = shown.substring(shown.length() - 24);
        text(1, BOTTOM - 9, shown.c_str(), ACCENT);
        String counts = String("rx") + rxBytes + " tx" + txBytes;
        textRight(W - 2, BOTTOM - 9, counts.c_str(), DIM);
        push();

        Key k = poll(true);
        if (k.type == K_BACK) break;
        if (k.type == K_TAB) { hex = !hex; if (cur.length()) pushLine(); }
        if (k.type == K_DEL && input.length()) input.remove(input.length() - 1);
        if (k.type == K_CHAR && k.ctrl && (k.c == 'l' || k.c == 'L')) { lines.clear(); cur = ""; hexCount = 0; continue; }
        if (k.type == K_CHAR && input.length() < 120) input += k.c;
        if (k.type == K_ENTER) {
            String out = input + EOL_VAL[eol];
            uart.write((const uint8_t*)out.c_str(), out.length());
            txBytes += out.length();
            input = "";
        }
        delay(5);
    }
    uart.end();
}

static void loopback(int rx, int tx)
{
    String report = String("Jumper G") + tx + " (TX) -> G" + rx + " (RX)\n";
    static const uint32_t rates[] = {9600, 115200, 921600};
    bool allOk = true;
    for (uint32_t b : rates) {
        busy("Loopback", (String("testing ") + b + " baud...").c_str());
        uart.end();
        uart.begin(b, SERIAL_8N1, rx, tx);
        delay(20);
        while (uart.available()) uart.read();
        uint8_t out[128], in[128];
        for (int i = 0; i < 128; i++) out[i] = (uint8_t)(esp_random() & 0xFF);
        uart.write(out, sizeof(out));
        uart.flush();
        uint32_t t0 = millis();
        size_t got = 0;
        while (got < sizeof(in) && millis() - t0 < 500) {
            if (uart.available()) in[got++] = uart.read();
        }
        int bad = 0;
        for (size_t i = 0; i < got; i++) if (in[i] != out[i]) bad++;
        bool ok = got == sizeof(in) && !bad;
        allOk &= ok;
        report += String(b) + ": " + (ok ? "PASS" : "FAIL") + "  " + got + "/128 rx, " + bad + " bad\n";
    }
    uart.end();
    message(allOk ? "Loopback: PASS" : "Loopback: FAIL", report.c_str(), allOk ? GOOD : BAD);
}

void toolUart()
{
    static int port = PORT_EXT, baud = 7, swap = 0, eol = 3, sel = 4;
    Field f[] = {
        Field::choice("Port", &port, PORTS, 2),
        Field::choice("Baud", &baud, BAUDS, 11),
        Field::choice("Swap RX/TX", &swap, YESNO, 2),
        Field::choice("Send line ending", &eol, EOLS, 4),
        Field::action("Open terminal"),
        Field::action("Loopback self-test"),
    };
    for (;;) {
        int a = form("UART", f, 6, sel);
        if (a < 0) return;
        int rx, tx;
        uartPins((UartPort)port, swap, rx, tx);
        if (port == PORT_GROVE) M5.Ex_I2C.release();
        if (a == 4) terminal(rx, tx, BAUD_VAL[baud], eol);
        else loopback(rx, tx);
    }
}
