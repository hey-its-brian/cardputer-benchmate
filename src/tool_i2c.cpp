#include "config.h"
#include "tools.h"
#include "ui.h"

using namespace ui;

struct KnownDev {
    uint8_t addr;
    const char* name;
};

// Common breakout parts; several addresses are shared, so names are hints.
static const KnownDev KNOWN[] = {
    {0x0D, "QMC5883L mag"},     {0x10, "VEML6075/GPS"},  {0x18, "ES8311 codec / LIS3DH"},
    {0x19, "LIS3DH/LSM303"},    {0x1E, "HMC5883L mag"},  {0x20, "PCF8574/MCP23017"},
    {0x21, "PCF8574/MCP23017"}, {0x23, "BH1750 lux"},    {0x24, "PN532 NFC"},
    {0x27, "PCF8574 LCD"},      {0x28, "BNO055/CAP1188"}, {0x29, "VL53L0X/TCS34725"},
    {0x34, "TCA8418 keyboard"}, {0x36, "MAX17048/Seesaw"}, {0x38, "AHT20/FT6236"},
    {0x39, "APDS9960/TSL2561"}, {0x3C, "SSD1306/SH1106 OLED"}, {0x3D, "SSD1306 OLED"},
    {0x40, "INA219/PCA9685/SHT2x"}, {0x41, "INA219/INA226"}, {0x42, "u-blox GNSS (DDC)"},
    {0x43, "INA219"},           {0x44, "SHT3x/SHT4x"},   {0x45, "SHT3x"},
    {0x48, "ADS1115/TMP102"},   {0x49, "ADS1115/TSL2561"}, {0x4A, "ADS1115"},
    {0x4B, "ADS1115"},          {0x50, "AT24 EEPROM"},   {0x51, "RTC BM8563/PCF8563"},
    {0x52, "EEPROM/Nunchuk"},   {0x53, "ADXL345"},       {0x57, "MAX30102/EEPROM"},
    {0x58, "SGP30"},            {0x59, "SGP40"},         {0x5A, "MLX90614/CCS811"},
    {0x5B, "CCS811"},           {0x5C, "AM2320/BH1750"}, {0x5D, "GT911 touch"},
    {0x60, "Si5351/MCP4725/ATECC"}, {0x61, "SCD30"},     {0x62, "SCD4x/MCP4725"},
    {0x68, "BMI270 IMU / MPU6050 / DS3231"}, {0x69, "MPU6050/BMI270 alt"},
    {0x6A, "LSM6DS"},           {0x6B, "LSM6DS"},        {0x70, "TCA9548A mux/HT16K33"},
    {0x76, "BME280/BMP280/BMP388"}, {0x77, "BME280/BMP180/MS5611"},
};

static const char* lookup(uint8_t a)
{
    for (auto& k : KNOWN) if (k.addr == a) return k.name;
    return "unknown";
}

// On-board parts on the internal bus, so they are not mistaken for a module.
static bool onboard(uint8_t a)
{
    return a == 0x18 || a == 0x34 || a == 0x68 || a == 0x69;
}

void toolI2c()
{
    int bus = 0;  // 0 = internal/EXT G8/G9, 1 = Grove G2/G1
    int scroll = 0;
    bool found[128] = {false};
    uint32_t lastScan = 0;
    int freqIdx = 0;
    static const uint32_t FREQS[] = {100000, 400000, 10000};

    for (;;) {
        if (millis() - lastScan > 1000) {
            memset(found, 0, sizeof(found));
            if (bus == 0) {
                M5.In_I2C.scanID(found, FREQS[freqIdx]);
            } else {
                // Port 0 only: port 1 is the internal bus (keyboard, IMU).
                M5.Ex_I2C.begin(I2C_NUM_0, g.pins.groveA, g.pins.groveB);
                M5.Ex_I2C.scanID(found, FREQS[freqIdx]);
            }
            lastScan = millis();
        }

        std::vector<uint8_t> list;
        for (int a = 8; a < 0x78; a++) if (found[a]) list.push_back(a);

        frame("I2C scanner", "TAB bus  f speed  ;. scroll  ` back");
        textf(4, TOP, TEAL, "%s", bus == 0 ? "EXT/internal SDA G8 SCL G9" : "Grove SDA G2 SCL G1");
        textRight(W - 4, TOP, (String(FREQS[freqIdx] / 1000) + "k").c_str(), DIM);

        // address map: 7 rows of 16
        int gx = 4, gy = TOP + 11;
        for (int a = 0; a < 0x80; a++) {
            int cx = gx + (a % 16) * 14, cy = gy + (a / 16) * 6;
            uint16_t c = (a < 8 || a >= 0x78) ? BG : found[a] ? (bus == 0 && onboard(a) ? DIM : GOOD) : PANEL;
            canvas.fillRect(cx, cy, 12, 4, c);
        }
        int y = gy + 8 * 6 + 1;
        int visible = (BOTTOM - y) / LINE;
        scroll = constrain(scroll, 0, max(0, (int)list.size() - visible));
        if (list.empty()) text(4, y + 4, "no devices (pull-ups? power?)", WARN);
        for (int i = 0; i < visible && scroll + i < (int)list.size(); i++) {
            uint8_t a = list[scroll + i];
            bool ob   = bus == 0 && onboard(a);
            textf(4, y, ob ? DIM : GOOD, "0x%02X", a);
            textf(36, y, ob ? DIM : FG, "%s%s", lookup(a), ob ? " [onboard]" : "");
            y += LINE;
        }
        push();

        Key k = poll();
        if (k.type == K_BACK) break;
        if (k.type == K_TAB) { bus ^= 1; lastScan = 0; scroll = 0; }
        if (k.is('f')) { freqIdx = (freqIdx + 1) % 3; lastScan = 0; }
        if (k.type == K_DOWN) scroll++;
        if (k.type == K_UP) scroll = max(0, scroll - 1);
        delay(15);
    }
    M5.Ex_I2C.release();
}
