#pragma once
#include <RadioLib.h>
#include <functional>

// One HAL instance on the shared EXT SPI bus. The bus itself is started by
// radioSpiBegin(), so RadioLib never calls SPI.begin()/end() on its own.
RadioLibHal* radioHal();

// Fresh Module bound to the configured EXT pins. `rstPin` and `auxPin` let
// callers re-map roles (nRF24 uses AUX as CE in the reset slot).
Module* radioModule(uint32_t irq, uint32_t rst, uint32_t aux);

const char* radioErr(int16_t code);

struct PacketScreenOps {
    const char* title;
    PhysicalLayer* phy;
    bool hasRssi = true;
    bool hasSnr  = false;
    std::function<float()> liveRssi;        // optional channel RSSI while listening
    std::function<void()> restartRx;        // optional; defaults to phy->startReceive()
    std::function<int16_t(uint8_t*, size_t)> transmit;  // optional; defaults to phy->transmit()
};

void packetRxScreen(const PacketScreenOps& ops);
void packetTxScreen(const PacketScreenOps& ops);
