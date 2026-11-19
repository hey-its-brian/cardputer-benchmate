#include "ui.h"
#include <stdarg.h>

namespace ui {

// Use M5.Display, not M5Cardputer.Display: the latter is a reference member that
// is still null if this file's globals are constructed before M5Cardputer.
M5Canvas canvas(&M5.Display);

static Key s_held;
static uint32_t s_heldSince = 0;
static uint32_t s_lastRepeat = 0;
static int s_battery = -1;
static uint32_t s_batteryAt = 0;

void begin()
{
    canvas.setColorDepth(16);
    canvas.createSprite(W, H);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
}

static Key decode(bool textMode)
{
    Key k;
    auto& ks = M5Cardputer.Keyboard.keysState();
    k.fn   = ks.fn;
    k.ctrl = ks.ctrl;
    if (ks.enter) { k.type = K_ENTER; return k; }
    if (ks.del)   { k.type = K_DEL;   return k; }
    if (ks.tab)   { k.type = K_TAB;   return k; }
    char c = 0;
    if (!ks.word.empty()) c = ks.word[0];
    else if (ks.space) c = ' ';
    if (!c) return k;
    if (c == '`' || c == '~') { k.type = K_BACK; return k; }
    if (!textMode || ks.fn) {
        switch (c) {
            case ';': case ':': k.type = K_UP;    return k;
            case '.': case '>': k.type = K_DOWN;  return k;
            case ',': case '<': k.type = K_LEFT;  return k;
            case '/': case '?': k.type = K_RIGHT; return k;
        }
    }
    k.type = K_CHAR;
    k.c    = c;
    return k;
}

// USB serial remote, for screenshots and driving the UI from a computer.
// Ctrl-P (0x10) dumps the current frame as "BMSHOT <w> <h>" plus raw RGB565;
// any other byte is treated as that key being pressed.
static Key serialKey(bool textMode)
{
    Key k;
    if (!Serial.available()) return k;
    int c = Serial.read();
    if (c == 0x10) {
        Serial.printf("\nBMSHOT %d %d\n", W, H);
        Serial.write((const uint8_t*)canvas.getBuffer(), W * H * 2);
        Serial.flush();
        return k;
    }
    if (c == '\r' || c == '\n') { k.type = K_ENTER; return k; }
    if (c == 0x7F || c == 0x08) { k.type = K_DEL; return k; }
    if (c == '\t') { k.type = K_TAB; return k; }
    if (c == '`') { k.type = K_BACK; return k; }
    if (!textMode) {
        switch (c) {
            case ';': k.type = K_UP;    return k;
            case '.': k.type = K_DOWN;  return k;
            case ',': k.type = K_LEFT;  return k;
            case '/': k.type = K_RIGHT; return k;
        }
    }
    if (c >= 32 && c < 127) { k.type = K_CHAR; k.c = c; }
    return k;
}

Key poll(bool textMode)
{
    M5Cardputer.update();
    uint32_t now = millis();
    if (Key sk = serialKey(textMode)) return sk;
    if (M5Cardputer.BtnA.wasPressed()) {
        Key k;
        k.type = K_BACK;
        return k;
    }
    auto& kb = M5Cardputer.Keyboard;
    if (kb.isChange()) {
        if (kb.isPressed()) {
            s_held       = decode(textMode);
            s_heldSince  = now;
            s_lastRepeat = now;
            return s_held;
        }
        s_held = Key();
        return Key();
    }
    // auto-repeat for navigation, adjust and delete keys
    bool repeatable = s_held.type == K_UP || s_held.type == K_DOWN || s_held.type == K_LEFT ||
                      s_held.type == K_RIGHT || s_held.type == K_DEL;
    if (repeatable && kb.isPressed() && now - s_heldSince > 400 && now - s_lastRepeat > 70) {
        s_lastRepeat = now;
        return s_held;
    }
    return Key();
}

void push()
{
    canvas.pushSprite(0, 0);
}

void text(int x, int y, const char* s, uint16_t color)
{
    canvas.setTextColor(color);
    canvas.setCursor(x, y);
    canvas.print(s);
}

void textf(int x, int y, uint16_t color, const char* fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    text(x, y, buf, color);
}

void textRight(int xRight, int y, const char* s, uint16_t color)
{
    text(xRight - canvas.textWidth(s), y, s, color);
}

void bar(int x, int y, int w, int h, float frac, uint16_t color, uint16_t back)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    canvas.fillRect(x, y, w, h, back);
    int fw = (int)(w * frac + 0.5f);
    if (fw > 0) canvas.fillRect(x, y, fw, h, color);
}

uint16_t rssiColor(float dbm)
{
    if (dbm > -70) return GOOD;
    if (dbm > -95) return WARN;
    return BAD;
}

void frame(const char* title, const char* hint)
{
    canvas.fillScreen(BG);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.fillRect(0, 0, W, HEAD_H, PANEL);
    canvas.fillRect(0, HEAD_H - 1, W, 1, ACCENT);
    text(4, 3, title, ACCENT);

    uint32_t now = millis();
    if (s_battery < 0 || now - s_batteryAt > 5000) {
        s_battery   = M5Cardputer.Power.getBatteryLevel();
        s_batteryAt = now;
    }
    char b[8];
    snprintf(b, sizeof(b), "%d%%", s_battery);
    uint16_t bc = s_battery > 40 ? GOOD : s_battery > 15 ? WARN : BAD;
    int bx = W - 4 - canvas.textWidth(b);
    text(bx, 3, b, bc);

    if (hint) {
        canvas.fillRect(0, FOOT_Y, W, H - FOOT_Y, PANEL);
        text(3, FOOT_Y + 1, hint, DIM);
    }
}

void message(const char* title, const char* msg, uint16_t color)
{
    frame(title, "any key: continue");
    // simple word wrap at 38 columns
    int y = TOP + 4;
    String s(msg);
    while (s.length() && y < BOTTOM) {
        int nl  = s.indexOf('\n');
        int cut = nl >= 0 && nl <= 38 ? nl : (int)min<size_t>(38, s.length());
        if (nl < 0 || nl > 38) {
            if ((int)s.length() > 38) {
                int sp = s.lastIndexOf(' ', 38);
                if (sp > 10) cut = sp;
            }
        }
        text(6, y, s.substring(0, cut).c_str(), color);
        s = s.substring(cut);
        if (s.startsWith("\n") || s.startsWith(" ")) s = s.substring(1);
        y += LINE + 1;
    }
    push();
    while (!poll()) delay(10);
}

void busy(const char* title, const char* msg)
{
    frame(title);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(FG);
    canvas.drawString(msg, W / 2, (TOP + BOTTOM) / 2);
    canvas.setTextDatum(top_left);
    push();
}

int menu(const char* title, const char* const* items, int n, int sel, const char* hint)
{
    constexpr int ROW_H = 18;
    constexpr int VISIBLE = (BOTTOM - TOP) / ROW_H;
    int top = 0;
    if (!hint) hint = ";/. move  ENT select  ` back";
    for (;;) {
        if (sel < top) top = sel;
        if (sel >= top + VISIBLE) top = sel - VISIBLE + 1;
        frame(title, hint);
        canvas.setFont(&fonts::Font2);
        for (int i = 0; i < VISIBLE && top + i < n; i++) {
            int idx = top + i;
            int y   = TOP + i * ROW_H;
            if (idx == sel) {
                canvas.fillRoundRect(2, y, W - 10, ROW_H - 1, 3, SEL_BG);
                canvas.fillRect(2, y + 3, 3, ROW_H - 7, ACCENT);
            }
            canvas.setTextColor(idx == sel ? ACCENT : DIM);
            canvas.setCursor(10, y + 1);
            canvas.printf("%c", idx < 9 ? '1' + idx : ' ');
            canvas.setTextColor(idx == sel ? FG : 0xBDF7);
            canvas.setCursor(24, y + 1);
            canvas.print(items[idx]);
        }
        canvas.setFont(&fonts::Font0);
        if (n > VISIBLE) {  // scrollbar
            int th = (BOTTOM - TOP) * VISIBLE / n;
            int ty = TOP + (BOTTOM - TOP - th) * top / (n - VISIBLE);
            canvas.fillRect(W - 4, TOP, 2, BOTTOM - TOP, PANEL);
            canvas.fillRect(W - 4, ty, 2, th, DIM);
        }
        push();

        Key k;
        while (!(k = poll())) delay(10);
        switch (k.type) {
            case K_UP:    sel = (sel + n - 1) % n; break;
            case K_DOWN:  sel = (sel + 1) % n; break;
            case K_ENTER: return sel;
            case K_BACK:  return -1;
            case K_CHAR:
                if (k.c >= '1' && k.c <= '9' && k.c - '1' < n) return k.c - '1';
                break;
            default: break;
        }
    }
}

static bool editLine(const char* title, String& value, size_t maxLen, bool numeric)
{
    String s = value;
    for (;;) {
        frame(title, "ENT ok  DEL erase  ` cancel");
        canvas.fillRoundRect(6, 50, W - 12, 22, 3, PANEL);
        canvas.drawRoundRect(6, 50, W - 12, 22, 3, ACCENT);
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(FG);
        String shown = s;
        while (canvas.textWidth(shown + "_") > W - 24) shown = shown.substring(1);
        canvas.setCursor(12, 53);
        canvas.print(shown);
        if ((millis() / 400) % 2 == 0) canvas.print("_");
        canvas.setFont(&fonts::Font0);
        push();

        Key k = poll(true);
        if (!k) { delay(15); continue; }
        if (k.type == K_ENTER) { value = s; return true; }
        if (k.type == K_BACK) return false;
        if (k.type == K_DEL && s.length()) s.remove(s.length() - 1);
        if (k.type == K_CHAR && s.length() < maxLen) {
            if (!numeric || isdigit(k.c) || k.c == '.' || k.c == '-') s += k.c;
        }
    }
}

bool editNumber(const char* title, float& value, int decimals)
{
    String s = String(value, decimals);
    if (!editLine(title, s, 12, true)) return false;
    if (!s.length()) return false;
    value = s.toFloat();
    return true;
}

bool editText(const char* title, String& value, size_t maxLen)
{
    return editLine(title, value, maxLen, false);
}

static String fieldValue(const Field& f)
{
    switch (f.kind) {
        case Field::CHOICE: return f.opts[*f.idx];
        case Field::NUM:    return String(*f.val, f.dec);
        default:            return "";
    }
}

int form(const char* title, Field* fields, int n, int& sel)
{
    constexpr int ROW_H = 13;
    constexpr int VISIBLE = (BOTTOM - TOP) / ROW_H;
    int top = 0;
    if (sel < 0 || sel >= n) sel = 0;
    for (;;) {
        if (sel < top) top = sel;
        if (sel >= top + VISIBLE) top = sel - VISIBLE + 1;
        Field& cur = fields[sel];
        frame(title, cur.kind == Field::ACTION ? ";/. move  ENT run  ` back"
                                               : ";/. move  ,// change  ENT edit");
        for (int i = 0; i < VISIBLE && top + i < n; i++) {
            int idx  = top + i;
            Field& f = fields[idx];
            int y    = TOP + i * ROW_H;
            bool on  = idx == sel;
            if (on) canvas.fillRect(0, y - 2, W, ROW_H, SEL_BG);
            if (f.kind == Field::ACTION) {
                text(8, y, on ? "> " : "  ", ACCENT);
                text(20, y, f.label, on ? ACCENT : TEAL);
            } else {
                text(8, y, f.label, on ? FG : DIM);
                String v = fieldValue(f);
                if (on) v = "< " + v + " >";
                textRight(W - 8, y, v.c_str(), on ? ACCENT : FG);
            }
        }
        push();

        Key k;
        while (!(k = poll())) delay(10);
        int dir = 0;
        switch (k.type) {
            case K_UP:    sel = (sel + n - 1) % n; break;
            case K_DOWN:  sel = (sel + 1) % n; break;
            case K_LEFT:  dir = -1; break;
            case K_RIGHT: dir = 1; break;
            case K_BACK:  return -1;
            case K_ENTER:
                if (cur.kind == Field::ACTION) return sel;
                if (cur.kind == Field::CHOICE) dir = 1;
                if (cur.kind == Field::NUM) {
                    float v = *cur.val;
                    if (editNumber(cur.label, v, cur.dec)) *cur.val = constrain(v, cur.mn, cur.mx);
                }
                break;
            case K_CHAR:
                if (cur.kind == Field::NUM && (isdigit(k.c) || k.c == '-')) {
                    String s(k.c);
                    if (editLine(cur.label, s, 12, true) && s.length())
                        *cur.val = constrain(s.toFloat(), cur.mn, cur.mx);
                }
                break;
            default: break;
        }
        if (dir) {
            if (cur.kind == Field::CHOICE) *cur.idx = (*cur.idx + dir + cur.nopts) % cur.nopts;
            if (cur.kind == Field::NUM) *cur.val = constrain(*cur.val + dir * cur.step, cur.mn, cur.mx);
        }
    }
}

}  // namespace ui
