#pragma once

void toolRig();
void toolDetect();
void toolGps();
void toolLora();
void toolCC1101();
void toolNrf24();
void toolI2c();
void toolUart();
void toolGpio();
void toolWifi();
void toolSystem();
void toolSettings();
void toolVmeter();

// M5Stack Unit VMeter helpers (tool_vmeter.cpp). Begin finds the unit on the
// Grove or EXT bus; `where` and `calibrated` are optional outputs.
bool vmeterBegin(const char** where, bool* calibrated);
bool vmeterRead(float& volts);
void vmeterEnd();
// Describes a reading as a supply rail; *ok is true for a healthy 3.3 V or 5 V.
const char* vmeterRail(float volts, bool* ok);

#include <stdint.h>
// Listens for checksummed NMEA on rx and returns the baud rate found, or 0.
uint32_t gpsDetectBaud(int rx, int tx, bool quick);

#define BENCHMATE_VERSION "1.1.0"
