// BenchMate: bench testing firmware for the M5Stack Cardputer ADV.
#include <M5Cardputer.h>
#include "config.h"
#include "tools.h"
#include "ui.h"

using namespace ui;

struct Tool {
    const char* name;
    void (*run)();
};

static const Tool TOOLS[] = {
    {"Test rig", toolRig},
    {"Auto detect", toolDetect},
    {"GPS / GNSS", toolGps},
    {"LoRa SX126x/127x", toolLora},
    {"CC1101 sub-GHz", toolCC1101},
    {"nRF24 2.4 GHz", toolNrf24},
    {"Voltmeter", toolVmeter},
    {"I2C scanner", toolI2c},
    {"UART terminal", toolUart},
    {"GPIO / logic probe", toolGpio},
    {"WiFi scan", toolWifi},
    {"System info", toolSystem},
    {"Settings / pins", toolSettings},
};
constexpr int NTOOLS = sizeof(TOOLS) / sizeof(TOOLS[0]);

static void splash()
{
    frame("BenchMate");
    canvas.setFont(&fonts::Font4);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(ACCENT);
    canvas.drawString("BenchMate", W / 2, 55);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(DIM);
    canvas.drawString("bench test toolkit  v" BENCHMATE_VERSION, W / 2, 80);
    const char* board = M5.getBoard() == m5::board_t::board_M5CardputerADV ? "Cardputer ADV detected"
                                                                          : "warning: not a Cardputer ADV";
    canvas.setTextColor(M5.getBoard() == m5::board_t::board_M5CardputerADV ? GOOD : WARN);
    canvas.drawString(board, W / 2, 96);
    canvas.setTextDatum(top_left);
    push();
    delay(900);
}

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    settingsLoad();
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(g.brightness);
    ui::begin();
    radioPinsRelease();
    pinMode(g.pins.sdCs, OUTPUT);
    digitalWrite(g.pins.sdCs, HIGH);
    splash();
}

void loop()
{
    static int sel = 0;
    const char* names[NTOOLS];
    for (int i = 0; i < NTOOLS; i++) names[i] = TOOLS[i].name;
    int choice = menu("BenchMate", names, NTOOLS, sel, ";/. move  1-9 jump  ENT open");
    if (choice < 0) return;
    sel = choice;
    TOOLS[choice].run();
}
