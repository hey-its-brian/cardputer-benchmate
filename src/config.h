#pragma once
#include <Arduino.h>

// Default wiring follows the Cardputer ADV EXT 2.54-14P header, which is also
// what the M5 Cap LoRa-1262 (SX1262 + GPS) uses.
//
//   EXT pin   signal   GPIO      EXT pin   signal    GPIO
//   1         RESET    G3        2         5VIN      -
//   3         INT      G4        4         GND       -
//   5         BUSY     G6        6         5VOUT     -
//   7         SCK      G40       8         I2C_SDA   G8
//   9         MOSI     G14       10        I2C_SCL   G9
//   11        MISO     G39       12        UART_TX   G13
//   13        CS       G5        14        UART_RX   G15
//
// Grove (HY2.0-4P): yellow G2, white G1.

struct PinConfig {
    int8_t sck    = 40;
    int8_t miso   = 39;
    int8_t mosi   = 14;
    int8_t cs     = 5;
    int8_t irq    = 4;   // SX126x DIO1 / SX127x DIO0 / CC1101 GDO0 / nRF24 IRQ
    int8_t rst    = 3;   // radio reset
    int8_t aux    = 6;   // SX126x BUSY / SX127x DIO1 / CC1101 GDO2 / nRF24 CE
    int8_t uartTx = 13;  // EXT UART, ESP32 TX
    int8_t uartRx = 15;  // EXT UART, ESP32 RX
    int8_t groveA = 2;   // yellow
    int8_t groveB = 1;   // white
    int8_t sdCs   = 12;  // held high so the SD card stays off the shared bus
};

struct Settings {
    uint16_t version = 2;
    PinConfig pins;
    float loraFreq      = 915.0f;
    float subGhzFreq    = 433.92f;
    uint8_t brightness  = 140;
};

extern Settings g;

void settingsLoad();
void settingsSave();
void settingsReset();

// Bring up the shared EXT SPI bus with the configured pins (safe to call repeatedly).
void radioSpiBegin();
// Put every radio control pin back to a harmless input state.
void radioPinsRelease();

enum UartPort : uint8_t { PORT_EXT = 0, PORT_GROVE = 1 };
// Returns the configured RX/TX pins for a port, optionally swapped.
void uartPins(UartPort port, bool swap, int& rx, int& tx);
