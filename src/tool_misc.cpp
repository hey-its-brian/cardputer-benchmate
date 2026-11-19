#include <WiFi.h>
#include <esp_chip_info.h>
#include <esp_mac.h>
#include "config.h"
#include "tools.h"
#include "ui.h"

using namespace ui;

// ---------------------------------------------------------------- WiFi scan

static const char* authName(wifi_auth_mode_t m)
{
    switch (m) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2E";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
        default:                        return "?";
    }
}

void toolWifi()
{
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    int n = -1, scroll = 0;
    bool byChannel = false;

    for (;;) {
        if (n < 0) {
            busy("WiFi scan", "scanning 2.4 GHz...");
            n = WiFi.scanNetworks(false, true);
            scroll = 0;
        }
        frame("WiFi scan", "r rescan  TAB view  ;. scroll  ` back");
        textRight(W - 40, 3, (String(max(n, 0)) + " APs").c_str(), DIM);
        if (n <= 0) {
            text(4, TOP + 30, n == 0 ? "no networks found" : "scan failed", WARN);
        } else if (!byChannel) {
            int visible = ROWS;
            scroll = constrain(scroll, 0, max(0, n - visible));
            for (int i = 0; i < visible && scroll + i < n; i++) {
                int idx = scroll + i, y = TOP + i * LINE;
                int32_t rssi = WiFi.RSSI(idx);
                String ssid = WiFi.SSID(idx);
                if (!ssid.length()) ssid = "<hidden>";
                ssid = ssid.substring(0, 18);
                textf(2, y, rssiColor(rssi), "%4ld", (long)rssi);
                text(30, y, ssid.c_str(), FG);
                textf(144, y, DIM, "ch%-2ld", (long)WiFi.channel(idx));
                text(180, y, authName(WiFi.encryptionType(idx)), WiFi.encryptionType(idx) == WIFI_AUTH_OPEN ? WARN : DIM);
            }
        } else {
            // per-channel occupancy: count and strongest AP
            int cnt[15] = {0};
            int best[15];
            for (auto& b : best) b = -120;
            for (int i = 0; i < n; i++) {
                int ch = WiFi.channel(i);
                if (ch < 1 || ch > 14) continue;
                cnt[ch]++;
                best[ch] = max(best[ch], (int)WiFi.RSSI(i));
            }
            int gh = 80, gy = TOP + 6;
            for (int ch = 1; ch <= 13; ch++) {
                int x = 6 + (ch - 1) * 18;
                int h = cnt[ch] ? constrain((best[ch] + 100) * gh / 70, 2, gh) : 0;
                if (h) canvas.fillRect(x, gy + gh - h, 14, h, rssiColor(best[ch]));
                if (cnt[ch]) textf(x + 3, gy + gh - h - 9, FG, "%d", cnt[ch]);
                textf(x + (ch < 10 ? 4 : 1), gy + gh + 3, DIM, "%d", ch);
            }
        }
        push();
        Key k = poll();
        if (k.type == K_BACK) break;
        if (k.is('r')) n = -1;
        if (k.type == K_TAB) byChannel = !byChannel;
        if (k.type == K_DOWN) scroll++;
        if (k.type == K_UP) scroll = max(0, scroll - 1);
        delay(15);
    }
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
}

// ---------------------------------------------------------------- System info

void toolSystem()
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const char* board = M5.getBoard() == m5::board_t::board_M5CardputerADV ? "Cardputer ADV"
                        : M5.getBoard() == m5::board_t::board_M5Cardputer  ? "Cardputer (v1)"
                                                                           : "unknown";
    for (;;) {
        frame("System info", "` back");
        int y = TOP;
        textf(4, y, FG, "board  %s", board); y += LINE;
        textf(4, y, FG, "chip   ESP32-S3 rev%d  %lu MHz", chip.revision, (unsigned long)getCpuFrequencyMhz()); y += LINE;
        textf(4, y, FG, "flash  %lu MB   psram %lu KB", (unsigned long)(ESP.getFlashChipSize() >> 20),
              (unsigned long)(ESP.getPsramSize() >> 10)); y += LINE;
        textf(4, y, FG, "heap   %lu free / %lu min KB", (unsigned long)(ESP.getFreeHeap() >> 10),
              (unsigned long)(ESP.getMinFreeHeap() >> 10)); y += LINE;
        textf(4, y, FG, "mac    %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]); y += LINE;
        uint32_t up = millis() / 1000;
        textf(4, y, FG, "uptime %luh %02lum %02lus", (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60),
              (unsigned long)(up % 60)); y += LINE;
        int16_t mv = M5.Power.getBatteryVoltage();
        int lvl    = M5.Power.getBatteryLevel();
        textf(4, y, lvl > 15 ? GOOD : BAD, "batt   %d mV  %d%%", mv, lvl);
        bar(130, y, 100, 7, lvl / 100.0f, lvl > 15 ? GOOD : BAD); y += LINE;
        if (M5.Imu.isEnabled()) {
            M5.Imu.update();
            auto d = M5.Imu.getImuData();
            textf(4, y, TEAL, "accel  %+.2f %+.2f %+.2f g", d.accel.x, d.accel.y, d.accel.z); y += LINE;
            textf(4, y, TEAL, "gyro   %+.0f %+.0f %+.0f dps", d.gyro.x, d.gyro.y, d.gyro.z); y += LINE;
        } else {
            text(4, y, "imu    not detected", DIM); y += LINE;
        }
        textf(4, y, DIM, "BenchMate %s", BENCHMATE_VERSION);
        push();
        if (poll().type == K_BACK) break;
        delay(100);
    }
}

// ---------------------------------------------------------------- Settings

void toolSettings()
{
    static int sel = 0;
    PinConfig& p = g.pins;
    float v[] = {(float)p.cs, (float)p.irq, (float)p.rst, (float)p.aux, (float)p.sck, (float)p.miso,
                 (float)p.mosi, (float)p.uartTx, (float)p.uartRx, g.loraFreq, g.subGhzFreq, (float)g.brightness};
    Field f[] = {
        Field::num("SPI CS", &v[0], 1, 0, 48, 0),
        Field::num("IRQ (DIO1/DIO0/GDO0)", &v[1], 1, 0, 48, 0),
        Field::num("RST", &v[2], 1, 0, 48, 0),
        Field::num("AUX (BUSY/CE/GDO2)", &v[3], 1, 0, 48, 0),
        Field::num("SPI SCK", &v[4], 1, 0, 48, 0),
        Field::num("SPI MISO", &v[5], 1, 0, 48, 0),
        Field::num("SPI MOSI", &v[6], 1, 0, 48, 0),
        Field::num("EXT UART TX", &v[7], 1, 0, 48, 0),
        Field::num("EXT UART RX", &v[8], 1, 0, 48, 0),
        Field::num("Default LoRa MHz", &v[9], 1, 137, 1020, 3),
        Field::num("Default sub-GHz MHz", &v[10], 0.05f, 300, 928, 3),
        Field::num("Brightness", &v[11], 10, 10, 255, 0),
        Field::action("Save"),
        Field::action("Restore defaults"),
    };
    int a = form("Settings / pins", f, sizeof(f) / sizeof(f[0]), sel);
    if (a < 0) return;
    if (a == 13) {
        settingsReset();
        settingsSave();
        message("Settings", "Defaults restored and saved.", GOOD);
    } else {
        p.cs = v[0]; p.irq = v[1]; p.rst = v[2]; p.aux = v[3];
        p.sck = v[4]; p.miso = v[5]; p.mosi = v[6]; p.uartTx = v[7]; p.uartRx = v[8];
        g.loraFreq = v[9]; g.subGhzFreq = v[10]; g.brightness = v[11];
        settingsSave();
        message("Settings", "Saved. New pins apply to the next test you start.", GOOD);
    }
    M5Cardputer.Display.setBrightness(g.brightness);
}
