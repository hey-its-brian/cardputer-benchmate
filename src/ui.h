#pragma once
#include <M5Cardputer.h>

namespace ui {

// Screen layout (240x135)
constexpr int W        = 240;
constexpr int H        = 135;
constexpr int HEAD_H   = 14;
constexpr int FOOT_Y   = 125;
constexpr int TOP      = HEAD_H + 2;       // first usable content row
constexpr int BOTTOM   = FOOT_Y - 2;       // last usable content row
constexpr int LINE     = 10;               // Font0 line pitch
constexpr int ROWS     = (BOTTOM - TOP) / LINE;

// Palette (RGB565)
constexpr uint16_t BG      = 0x0841;
constexpr uint16_t PANEL   = 0x18E3;
constexpr uint16_t FG      = 0xE73C;
constexpr uint16_t DIM     = 0x7BEF;
constexpr uint16_t ACCENT  = 0xFD20;  // amber
constexpr uint16_t TEAL    = 0x5E7F;
constexpr uint16_t GOOD    = 0x47E8;
constexpr uint16_t WARN    = 0xFEA0;
constexpr uint16_t BAD     = 0xF9A6;
constexpr uint16_t SEL_BG  = 0x3186;

extern M5Canvas canvas;

enum KeyType : uint8_t { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_BACK, K_DEL, K_TAB, K_CHAR };

struct Key {
    KeyType type = K_NONE;
    char c       = 0;
    bool fn      = false;
    bool ctrl    = false;
    operator bool() const { return type != K_NONE; }
    bool is(char ch) const { return type == K_CHAR && (c == ch || c == (ch ^ 0x20)); }
};

void begin();
// Reads the keyboard. In nav mode ;.,/ act as arrows; in text mode they only
// do so with Fn held. The ` key (and the G0 button) always means "back".
Key poll(bool textMode = false);

// Drawing
void frame(const char* title, const char* hint = nullptr);  // clear + header + footer
void push();
void text(int x, int y, const char* s, uint16_t color = FG);
void textf(int x, int y, uint16_t color, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
void textRight(int xRight, int y, const char* s, uint16_t color = FG);
void bar(int x, int y, int w, int h, float frac, uint16_t color, uint16_t back = PANEL);
uint16_t rssiColor(float dbm);

// Blocking widgets
int  menu(const char* title, const char* const* items, int n, int sel = 0, const char* hint = nullptr);
bool editNumber(const char* title, float& value, int decimals);
bool editText(const char* title, String& value, size_t maxLen);
void message(const char* title, const char* msg, uint16_t color = FG);
void busy(const char* title, const char* msg);  // draws immediately, no wait

// Settings-style form: rows of label/value with inline editing.
struct Field {
    enum Kind : uint8_t { CHOICE, NUM, ACTION } kind;
    const char* label;
    // CHOICE
    int* idx                 = nullptr;
    const char* const* opts  = nullptr;
    int nopts                = 0;
    // NUM
    float* val = nullptr;
    float step = 1, mn = 0, mx = 0;
    int dec    = 0;

    static Field choice(const char* l, int* i, const char* const* o, int n)
    {
        Field f{CHOICE, l};
        f.idx = i; f.opts = o; f.nopts = n;
        return f;
    }
    static Field num(const char* l, float* v, float step, float mn, float mx, int dec)
    {
        Field f{NUM, l};
        f.val = v; f.step = step; f.mn = mn; f.mx = mx; f.dec = dec;
        return f;
    }
    static Field action(const char* l) { return Field{ACTION, l}; }
};

// Returns the index of the ACTION row chosen, or -1 on back. `sel` keeps the
// cursor position between calls.
int form(const char* title, Field* fields, int n, int& sel);

}  // namespace ui
