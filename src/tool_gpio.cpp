#include "config.h"
#include "tools.h"
#include "ui.h"

using namespace ui;

enum PinMode : uint8_t { M_IN, M_IN_PU, M_IN_PD, M_OUT_L, M_OUT_H, M_PWM, M_ADC, M_FREQ, M_COUNT };
static const char* const MODE_NAMES[] = {"IN", "IN-PU", "IN-PD", "OUT-L", "OUT-H", "PWM", "ADC", "FREQ"};
static const uint32_t PWM_FREQS[] = {50, 1000, 10000, 100000};

struct GpioRow {
    uint8_t pin;
    const char* where;
    PinMode mode;
    uint8_t pwmFreqIdx;
    uint8_t duty;  // percent
    volatile uint32_t edges;
    uint32_t lastEdges;
    float hz;
};

static void ARDUINO_ISR_ATTR countEdge(void* arg)
{
    (*(volatile uint32_t*)arg)++;
}

static void leaveMode(GpioRow& r)
{
    if (r.mode == M_PWM) ledcDetach(r.pin);
    if (r.mode == M_FREQ) detachInterrupt(r.pin);
}

static void enterMode(GpioRow& r)
{
    switch (r.mode) {
        case M_IN:    pinMode(r.pin, INPUT); break;
        case M_IN_PU: pinMode(r.pin, INPUT_PULLUP); break;
        case M_IN_PD: pinMode(r.pin, INPUT_PULLDOWN); break;
        case M_OUT_L: pinMode(r.pin, OUTPUT); digitalWrite(r.pin, LOW); break;
        case M_OUT_H: pinMode(r.pin, OUTPUT); digitalWrite(r.pin, HIGH); break;
        case M_PWM:
            ledcAttach(r.pin, PWM_FREQS[r.pwmFreqIdx], 10);
            ledcWrite(r.pin, r.duty * 1023 / 100);
            break;
        case M_ADC:   pinMode(r.pin, INPUT); analogSetPinAttenuation(r.pin, ADC_11db); break;
        case M_FREQ:
            pinMode(r.pin, INPUT);
            r.edges = r.lastEdges = 0;
            r.hz = 0;
            attachInterruptArg(r.pin, countEdge, (void*)&r.edges, RISING);
            break;
        default: break;
    }
}

static String fmtHz(float hz)
{
    if (hz >= 1e6f) return String(hz / 1e6f, 3) + " MHz";
    if (hz >= 1e3f) return String(hz / 1e3f, 2) + " kHz";
    return String(hz, 0) + " Hz";
}

void toolGpio()
{
    M5.Ex_I2C.release();
    GpioRow rows[] = {
        {(uint8_t)g.pins.groveA, "Grove Y", M_IN, 1, 50},
        {(uint8_t)g.pins.groveB, "Grove W", M_IN, 1, 50},
        {3, "EXT RST", M_IN, 1, 50},
        {4, "EXT INT", M_IN, 1, 50},
        {5, "EXT CS", M_IN, 1, 50},
        {6, "EXT BUSY", M_IN, 1, 50},
        {13, "EXT TX", M_IN, 1, 50},
        {15, "EXT RX", M_IN, 1, 50},
    };
    constexpr int N = sizeof(rows) / sizeof(rows[0]);
    for (auto& r : rows) enterMode(r);
    int sel = 0;
    uint32_t lastFreq = millis();

    for (;;) {
        uint32_t now = millis();
        if (now - lastFreq >= 500) {
            float dt = (now - lastFreq) / 1000.0f;
            for (auto& r : rows) {
                if (r.mode != M_FREQ) continue;
                uint32_t e  = r.edges;
                r.hz        = (e - r.lastEdges) / dt;
                r.lastEdges = e;
            }
            lastFreq = now;
        }

        GpioRow& cur = rows[sel];
        const char* hint = "ENT mode  ,/ adjust  ` back";
        if (cur.mode == M_PWM) hint = "ENT mode  ,/ duty  f freq  ` back";
        frame("GPIO / logic probe", hint);
        for (int i = 0; i < N; i++) {
            GpioRow& r = rows[i];
            int y = TOP + i * 13;
            bool on = i == sel;
            if (on) canvas.fillRect(0, y - 2, W, 13, SEL_BG);
            textf(4, y, on ? ACCENT : FG, "G%d", r.pin);
            text(28, y, r.where, DIM);
            text(84, y, MODE_NAMES[r.mode], on ? FG : TEAL);
            switch (r.mode) {
                case M_IN: case M_IN_PU: case M_IN_PD: {
                    bool v = digitalRead(r.pin);
                    canvas.fillRoundRect(130, y - 1, 26, 9, 2, v ? GOOD : PANEL);
                    text(133, y, v ? "HIGH" : "LOW", v ? BG : DIM);
                    break;
                }
                case M_OUT_L: case M_OUT_H:
                    text(130, y, r.mode == M_OUT_H ? "driving 3V3" : "driving 0V", r.mode == M_OUT_H ? WARN : DIM);
                    break;
                case M_PWM:
                    textf(130, y, WARN, "%s %d%%", fmtHz(PWM_FREQS[r.pwmFreqIdx]).c_str(), r.duty);
                    break;
                case M_ADC: {
                    uint32_t mv = analogReadMilliVolts(r.pin);
                    textf(130, y, FG, "%4lu mV", (unsigned long)mv);
                    bar(184, y, 52, 7, mv / 3100.0f, TEAL);
                    break;
                }
                case M_FREQ:
                    text(130, y, fmtHz(r.hz).c_str(), r.hz > 0 ? GOOD : DIM);
                    break;
                default: break;
            }
        }
        push();

        Key k = poll();
        if (k.type == K_BACK) break;
        if (k.type == K_UP) sel = (sel + N - 1) % N;
        if (k.type == K_DOWN) sel = (sel + 1) % N;
        if (k.type == K_ENTER || k.type == K_TAB) {
            leaveMode(cur);
            cur.mode = (PinMode)((cur.mode + 1) % M_COUNT);
            enterMode(cur);
        }
        if (k.type == K_LEFT || k.type == K_RIGHT || (k.type == K_CHAR && k.c == ' ')) {
            int d = k.type == K_LEFT ? -10 : 10;
            if (cur.mode == M_OUT_L || cur.mode == M_OUT_H) {
                cur.mode = cur.mode == M_OUT_L ? M_OUT_H : M_OUT_L;
                enterMode(cur);
            } else if (cur.mode == M_PWM) {
                cur.duty = constrain((int)cur.duty + d, 0, 100);
                ledcWrite(cur.pin, cur.duty * 1023 / 100);
            }
        }
        if (k.is('f') && cur.mode == M_PWM) {
            leaveMode(cur);
            cur.pwmFreqIdx = (cur.pwmFreqIdx + 1) % 4;
            enterMode(cur);
        }
        delay(20);
    }
    for (auto& r : rows) {
        leaveMode(r);
        pinMode(r.pin, INPUT);
    }
    radioPinsRelease();
}
