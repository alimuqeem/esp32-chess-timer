// Chess clock for the Waveshare ESP32-S3-Touch-AMOLED-1.8 (V2: CO5300 panel).
//
// Stand-alone: no Mac, no Wi-Fi. The two players sit at the two ends of the screen; the top half is
// drawn upside down. Tap your own half after your move: your clock stops, your increment is added,
// your opponent's clock starts. The game logic lives in clock.h (unit-tested on the Mac).
//
// Serial debug commands (115200, newline-terminated):  STATE | TOUCH x y | LEFT player secs | CAL | DUMP
#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <esp_timer.h>

#include "Arduino_GFX_Library.h"
#include "clock.h"
#include "fonts/FreeSans9pt7b.h"
#include "fonts/FreeSansBold12pt7b.h"

// Pins and geometry from Waveshare's V2 pin_config.h.
static const int LCD_SDIO0 = 4, LCD_SDIO1 = 5, LCD_SDIO2 = 6, LCD_SDIO3 = 7;
static const int LCD_SCLK = 11, LCD_CS = 12;
static const int W = 368, H = 448;
static const int COL_OFFSET = 16;  // CO5300 on this board starts 16 columns in
static const int IIC_SDA = 15, IIC_SCL = 14;
static const uint8_t TOUCH_ADDR = 0x15, EXPANDER_ADDR = 0x20;

// Screen layout: top player's zone, a middle band, bottom player's zone.
static const int ZONE_H = 196, BAND_Y = 196, BAND_H = 56, BOT_Y = 252;

static const uint32_t DIM_AFTER_MS = 60000;   // idle (not mid-game): dim
static const uint32_t OFF_AFTER_MS = 300000;  // idle: blank the AMOLED
static const uint8_t BRIGHT_ON = 120, BRIGHT_DIM = 20;
static const uint32_t TAP_GAP_MS = 150;  // ignore touch events closer together than this
// The CST820's raw range covers only a window of the picture, stretched over 0..367 x 0..447 and
// clamped at the ends (so the outer 30-50 px of each edge all read as the extreme). Fitted from two
// runs of the CAL screen (18 taps, residual ~9 px rms): screen = raw / scale + offset.
static const float TOUCH_SCALE_X = 1.24f, TOUCH_SCALE_Y = 1.22f;
static const int TOUCH_OFF_X = 33, TOUCH_OFF_Y = 31;

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
static const uint16_t C_BG = 0x0000, C_TEXT = RGB(236, 236, 242), C_DIM = RGB(104, 104, 116);
static const uint16_t C_GHOST = RGB(20, 20, 26), C_LINE = RGB(48, 48, 58);
static const uint16_t C_GREEN = RGB(0, 214, 120), C_AMBER = RGB(255, 176, 0), C_RED = RGB(255, 84, 84);
static const uint16_t C_BLUE = RGB(90, 150, 255), C_FLAG_BG = RGB(150, 20, 24);

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(bus, GFX_NOT_DEFINED, 0, W, H, COL_OFFSET, 0, 0, 0);
Arduino_Canvas *canvas = new Arduino_Canvas(W, H, gfx);  // full frame lives in PSRAM

static ChessClock game;
static Preferences prefs;
static bool touchOk = false, dirty = true, asleep = false, dimmed = false;
static uint32_t lastActivity = 0;

static int64_t nowUs() { return esp_timer_get_time(); }

// ------------------------------------------------------------------- touch ----

static bool i2cWrite8(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool i2cRead(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return false;  // full STOP, as the CST816 family expects
  if (Wire.requestFrom((int)addr, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

// Waveshare's V2 sequence: expander pins 0-2 low for 20 ms then high (resets display and touch).
static bool expanderReset() {
  uint8_t out, cfg;
  if (!i2cRead(EXPANDER_ADDR, 0x01, &out, 1) || !i2cRead(EXPANDER_ADDR, 0x03, &cfg, 1)) return false;
  const uint8_t mask = 0x07;
  i2cWrite8(EXPANDER_ADDR, 0x03, cfg & ~mask);
  i2cWrite8(EXPANDER_ADDR, 0x01, out & ~mask);
  delay(20);
  i2cWrite8(EXPANDER_ADDR, 0x01, out | mask);
  return true;
}

static void touchInit() {
  // The CST820 only answers after the reset pulse and a *write* as its first access.
  uint8_t id = 0;
  for (int i = 0; i < 20 && !touchOk; i++) {
    if (i2cWrite8(TOUCH_ADDR, 0xFA, 0x10)) touchOk = i2cRead(TOUCH_ADDR, 0xA7, &id, 1);
    if (!touchOk) delay(100);
  }
  if (touchOk) i2cWrite8(TOUCH_ADDR, 0xFE, 0x01);  // no auto-standby: keep the chip answering
  Serial.printf("touch %s id=0x%02X\n", touchOk ? "ok" : "NOT FOUND", id);
}

// One event per finger-down. Returns true with the position and the time of the first contact. The
// chip can report a stray first sample, so a touch counts once a second sample agrees with the first.
static bool pollTouchDown(int *x, int *y, int64_t *tUs) {
  static uint32_t lastPoll = 0, lastEvent = 0;
  static bool touching = false, armed = false;
  static int px, py;
  static int64_t pt;
  uint32_t now = millis();
  if (!touchOk || now - lastPoll < 5) return false;
  lastPoll = now;
  uint8_t b[5];
  if (!i2cRead(TOUCH_ADDR, 0x02, b, 5)) return false;
  if (b[0] == 0) {
    touching = armed = false;
    return false;
  }
  int cx = ((b[1] & 0x0F) << 8) | b[2], cy = ((b[3] & 0x0F) << 8) | b[4];
  if (touching) return false;
  if (!armed || abs(cx - px) > 60 || abs(cy - py) > 60) {  // first sample (or it jumped): remember it
    armed = true;
    px = cx;
    py = cy;
    pt = nowUs();
    return false;
  }
  touching = true;  // second agreeing sample
  if (now - lastEvent < TAP_GAP_MS) return false;
  lastEvent = now;
  *x = cx;
  *y = cy;
  *tUs = pt;
  return true;
}

// Touch is polled on its own task (core 0) so a tap is timestamped when the finger lands even while the
// main loop is busy for ~65 ms drawing a frame. Events queue up and the main loop handles them in order.
struct TouchEvent {
  int16_t x, y;
  int64_t t;
};
static QueueHandle_t touchQ;

static void touchTask(void *) {
  for (;;) {
    int x, y;
    int64_t t;
    if (pollTouchDown(&x, &y, &t)) {
      TouchEvent e = {(int16_t)x, (int16_t)y, t};
      xQueueSend(touchQ, &e, 0);
    }
    vTaskDelay(1);
  }
}

// ----------------------------------------------------------------- drawing ----

static const int ALIGN_L = -1, ALIGN_C = 0, ALIGN_R = 1;

static void textAt(int x, int baseline, const GFXfont *f, uint16_t colour, const char *s, int align = ALIGN_C) {
  canvas->setFont(f);
  canvas->setTextSize(1);
  canvas->setTextColor(colour);
  int16_t x1, y1;
  uint16_t w, h;
  canvas->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  int cx = align == ALIGN_L ? x : align == ALIGN_R ? x - (int)w - x1 : x - (int)w / 2 - x1;
  canvas->setCursor(cx, baseline);
  canvas->print(s);
}

static void button(int x, int y, int w, int h, const char *label, uint16_t colour, bool filled = false) {
  canvas->fillRoundRect(x, y, w, h, 12, filled ? colour : C_BG);
  canvas->drawRoundRect(x, y, w, h, 12, colour);
  textAt(x + w / 2, y + h / 2 + 6, &FreeSansBold12pt7b, filled ? C_BG : colour, label);
}

// Seven-segment digits drawn as bevelled bars, so they stay crisp at any size.
// Segment bits: 0 a(top) 1 b(top right) 2 c(bottom right) 3 d(bottom) 4 e(bottom left) 5 f(top left) 6 g(mid)
static const uint8_t SEG_MASK[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

static void hseg(int x0, int x1, int yc, int t, uint16_t c) {
  int hh = t / 2;
  canvas->fillRect(x0 + hh, yc - hh, x1 - x0 - 2 * hh + 1, t, c);
  canvas->fillTriangle(x0, yc, x0 + hh, yc - hh, x0 + hh, yc + hh, c);
  canvas->fillTriangle(x1, yc, x1 - hh, yc - hh, x1 - hh, yc + hh, c);
}
static void vseg(int xc, int y0, int y1, int t, uint16_t c) {
  int hh = t / 2;
  canvas->fillRect(xc - hh, y0 + hh, t, y1 - y0 - 2 * hh + 1, c);
  canvas->fillTriangle(xc, y0, xc - hh, y0 + hh, xc + hh, y0 + hh, c);
  canvas->fillTriangle(xc, y1, xc - hh, y1 - hh, xc + hh, y1 - hh, c);
}

static void drawDigit(int x, int y, int w, int h, int t, int d, uint16_t lit, uint16_t ghost) {
  int L = x + t / 2, R = x + w - t / 2, T = y + t / 2, B = y + h - t / 2, M = y + h / 2;
  int g = t / 6 + 1;
  for (int pass = 0; pass < 2; pass++) {
    uint8_t m = pass == 0 ? 0x7F : SEG_MASK[d];
    uint16_t c = pass == 0 ? ghost : lit;
    if (m & 1) hseg(L + g, R - g, T, t, c);
    if (m & 2) vseg(R, T + g, M - g, t, c);
    if (m & 4) vseg(R, M + g, B - g, t, c);
    if (m & 8) hseg(L + g, R - g, B, t, c);
    if (m & 16) vseg(L, M + g, B - g, t, c);
    if (m & 32) vseg(L, T + g, M - g, t, c);
    if (m & 64) hseg(L + g, R - g, M, t, c);
  }
}

// A time string ("05:00", "1:30:00", "9.4") centred on cx, as large as fits in maxW x maxH.
static void drawTime(int cx, int top, int maxW, int maxH, const char *s, uint16_t lit) {
  int nd = 0, ns = 0;  // digits, separators
  for (const char *p = s; *p; p++) (*p >= '0' && *p <= '9') ? nd++ : ns++;
  float units = nd * 0.67f + ns * 0.23f - 0.09f;  // width in units of digit height
  int h = (int)(maxW / units);
  if (h > maxH) h = maxH;
  if (h > 124) h = 124;  // keeps the tenths view ("9.4") from crowding the edge bar
  int w = h * 58 / 100, t = (h * 14 / 100) & ~1, gap = h * 9 / 100;
  int total = nd * (w + gap) + ns * (t + gap) - gap;
  int x = cx - total / 2;
  int y = top + (maxH - h) / 2;
  for (const char *p = s; *p; p++) {
    if (*p >= '0' && *p <= '9') {
      drawDigit(x, y, w, h, t, *p - '0', lit, C_GHOST);
      x += w + gap;
    } else {
      if (*p == ':') {
        canvas->fillRect(x, y + h * 30 / 100 - t / 2, t, t, lit);
        canvas->fillRect(x, y + h * 70 / 100 - t / 2, t, t, lit);
      } else {  // '.'
        canvas->fillRect(x, y + h - t, t, t, lit);
      }
      x += t + gap;
    }
  }
}

// Reverse the pixels of the top zone: a 180-degree turn, so the player across the table reads it.
static void rotateTopZone() {
  uint16_t *a = canvas->getFramebuffer(), *b = a + (size_t)W * ZONE_H - 1;
  while (a < b) {
    uint16_t t = *a;
    *a++ = *b;
    *b-- = t;
  }
}

static uint16_t lowTimeColour(int64_t left) {
  return left <= 10 * US ? C_RED : left <= 30 * US ? C_AMBER : C_TEXT;
}

// Draws one player's zone as if it were the bottom one; y0 is the zone's top edge.
static void drawZone(int p, int y0, int64_t now) {
  Phase ph = game.phase;
  int64_t left = game.remaining(p, now);
  bool running = ph == Phase::Running || ph == Phase::Paused;
  bool mine = running && game.active == p;
  bool loser = ph == Phase::Flagged && game.flagged == p;
  bool winner = ph == Phase::Flagged && game.flagged != p;
  if (loser) canvas->fillRect(0, y0, W, ZONE_H, C_FLAG_BG);

  uint16_t digits = ph == Phase::Ready ? C_TEXT : loser ? C_TEXT : winner ? C_GREEN : mine ? lowTimeColour(left) : C_DIM;
  char s[16];
  formatTime(left, s, sizeof s);
  drawTime(W / 2, y0 + 40, 340, 142, s, digits);  // height is capped inside (see drawTime)

  // Caption row (nearest the middle band).
  if (ph == Phase::Ready) {
    textAt(W / 2, y0 + 30, &FreeSansBold12pt7b, C_BLUE, "TAP TO START");
  } else if (loser) {
    textAt(W / 2, y0 + 30, &FreeSansBold12pt7b, C_TEXT, "OUT OF TIME");
  } else if (winner) {
    textAt(W / 2, y0 + 30, &FreeSansBold12pt7b, C_GREEN, "WINNER");
  } else {
    char m[16];
    snprintf(m, sizeof m, "MOVES %u", (unsigned)game.moves[p]);
    textAt(18, y0 + 28, &FreeSans9pt7b, C_DIM, m, ALIGN_L);
    if (game.incUs) {
      char inc[12];
      snprintf(inc, sizeof inc, "+%ds", (int)(game.incUs / US));
      textAt(W - 18, y0 + 28, &FreeSans9pt7b, C_DIM, inc, ALIGN_R);
    }
  }

  // Outer-edge bar: whose clock is running.
  if (mine) {
    uint16_t c = ph == Phase::Paused ? C_AMBER : left <= 10 * US ? C_RED : left <= 30 * US ? C_AMBER : C_GREEN;
    canvas->fillRoundRect(24, y0 + ZONE_H - 12, W - 48, 8, 4, c);
  }
}

static void drawBand() {
  canvas->fillRect(0, BAND_Y, W, BAND_H, C_BG);
  canvas->drawFastHLine(0, BAND_Y, W, C_LINE);
  canvas->drawFastHLine(0, BAND_Y + BAND_H - 1, W, C_LINE);
  switch (game.phase) {
    case Phase::Ready:
      button(124, 204, 120, 40, "MENU", C_DIM);
      break;
    case Phase::Running:  // pause symbol; the whole band is the button
      canvas->fillRoundRect(W / 2 - 15, BAND_Y + 14, 10, 28, 3, C_DIM);
      canvas->fillRoundRect(W / 2 + 5, BAND_Y + 14, 10, 28, 3, C_DIM);
      break;
    case Phase::Flagged:
      button(16, 204, 160, 40, "AGAIN", C_GREEN, true);
      button(192, 204, 160, 40, "MENU", C_TEXT);
      break;
    default:
      break;
  }
}

static const int PANEL_Y = 146, PANEL_H = 160, PB_Y = 212, PB_H = 70, PB_W = 108;
static const int PB_X[3] = {12, 130, 248};

static void drawPausedPanel() {
  canvas->fillRoundRect(4, PANEL_Y, W - 8, PANEL_H, 18, C_BG);
  canvas->drawRoundRect(4, PANEL_Y, W - 8, PANEL_H, 18, C_AMBER);
  textAt(W / 2, PANEL_Y + 52, &FreeSansBold12pt7b, C_AMBER, "PAUSED");
  const char *lab[3] = {"RESUME", "RESTART", "MENU"};
  const uint16_t col[3] = {C_GREEN, C_BLUE, C_DIM};
  for (int i = 0; i < 3; i++) {
    canvas->fillRoundRect(PB_X[i], PB_Y, PB_W, PB_H, 12, i == 0 ? col[i] : C_BG);
    canvas->drawRoundRect(PB_X[i], PB_Y, PB_W, PB_H, 12, col[i]);
    textAt(PB_X[i] + PB_W / 2, PB_Y + PB_H / 2 + 6, &FreeSans9pt7b, i == 0 ? C_BG : col[i], lab[i]);
  }
}

// Setup: 3 columns x 4 rows of tiles, one row per group.
static const int TILE_X0 = 12, TILE_W = 109, TILE_GAP = 8, ROW_Y0 = 56, ROW_PITCH = 96, TILE_DY = 20, TILE_H = 70;

static uint16_t groupColour(char g) {
  return g == 'B' ? C_RED : g == 'Z' ? C_AMBER : g == 'R' ? C_GREEN : C_BLUE;
}
static uint16_t scale565(uint16_t c, int pct) {  // dim an RGB565 colour to pct %
  int r = ((c >> 11) & 31) * pct / 100, g = ((c >> 5) & 63) * pct / 100, b = (c & 31) * pct / 100;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void drawSetup() {
  textAt(W / 2, 36, &FreeSansBold12pt7b, C_TEXT, "CHESS CLOCK");
  const char *names[4] = {"BULLET", "BLITZ", "RAPID", "CLASSICAL"};
  for (int r = 0; r < 4; r++) {
    int y0 = ROW_Y0 + r * ROW_PITCH;
    textAt(TILE_X0 + 4, y0 + 14, &FreeSans9pt7b, C_DIM, names[r], ALIGN_L);
    for (int c = 0; c < 3; c++) {
      int i = r * 3 + c, x = TILE_X0 + c * (TILE_W + TILE_GAP), y = y0 + TILE_DY;
      uint16_t col = groupColour(PRESETS[i].group);
      bool last = i == game.preset;
      canvas->fillRoundRect(x, y, TILE_W, TILE_H, 14, scale565(col, last ? 30 : 12));
      canvas->drawRoundRect(x, y, TILE_W, TILE_H, 14, last ? col : scale565(col, 55));
      if (last) canvas->drawRoundRect(x + 1, y + 1, TILE_W - 2, TILE_H - 2, 13, col);
      textAt(x + TILE_W / 2, y + TILE_H / 2 + 8, &FreeSansBold12pt7b, C_TEXT, PRESETS[i].label);
    }
  }
}

// Calibration screen (debug command CAL): five targets at known positions; the log shows where each
// real tap landed so the touch mapping can be checked.
static const int CAL_N = 9;
static const int CAL_X[CAL_N] = {60, 184, 308, 60, 184, 308, 60, 184, 308};
static const int CAL_Y[CAL_N] = {60, 60, 60, 224, 224, 224, 388, 388, 388};
static int calStep = -1;  // -1 = off

static void drawCal() {
  textAt(W / 2, 150, &FreeSansBold12pt7b, C_TEXT, "TAP EACH TARGET");
  textAt(W / 2, 182, &FreeSans9pt7b, C_DIM, "in order 1 to 9");
  for (int i = 0; i < CAL_N; i++) {
    uint16_t c = i < calStep ? C_DIM : i == calStep ? C_GREEN : C_BLUE;
    canvas->drawCircle(CAL_X[i], CAL_Y[i], 14, c);
    canvas->drawFastHLine(CAL_X[i] - 22, CAL_Y[i], 45, c);
    canvas->drawFastVLine(CAL_X[i], CAL_Y[i] - 22, 45, c);
    char n[2] = {(char)('1' + i), 0};
    textAt(CAL_X[i], CAL_Y[i] - 28, &FreeSansBold12pt7b, c, n);
  }
}

static char lastShown[2][16];

static uint32_t lastDrawMs = 0, maxDrawMs = 0;  // time to render + push one frame (STATE reports them)

static void draw(int64_t now) {
  uint32_t t0 = millis();
  canvas->fillScreen(C_BG);
  if (calStep >= 0) {
    drawCal();
  } else if (game.phase == Phase::Setup) {
    drawSetup();
  } else {
    drawZone(1, 0, now);
    rotateTopZone();
    drawBand();
    drawZone(0, BOT_Y, now);
    if (game.phase == Phase::Paused) drawPausedPanel();
  }
  canvas->flush();
  lastDrawMs = millis() - t0;
  if (lastDrawMs > maxDrawMs) maxDrawMs = lastDrawMs;
  for (int p = 0; p < 2; p++) formatTime(game.remaining(p, now), lastShown[p], sizeof lastShown[p]);
}

// The picture only changes when a displayed time string changes (1 Hz, or 10 Hz under 20 s).
static bool timesChanged(int64_t now) {
  if (game.phase == Phase::Setup) return false;
  for (int p = 0; p < 2; p++) {
    char s[16];
    formatTime(game.remaining(p, now), s, sizeof s);
    if (strcmp(s, lastShown[p])) return true;
  }
  return false;
}

// ------------------------------------------------------------------ input -----

static bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void setPreset(int i) {
  game.select(i);
  prefs.putUChar("preset", (uint8_t)i);
}

static void onTap(int x, int y, int64_t t) {
  // Whose side a tap is on: the middle band is split at its centre, so a tap that lands a little
  // short of the edge still counts for the player who made it. 1 = top player, 0 = bottom player.
  int side = y < (BAND_Y + BOT_Y) / 2 ? 1 : 0;
  switch (game.phase) {
    case Phase::Setup: {
      int c = (x - TILE_X0 + TILE_GAP / 2) / (TILE_W + TILE_GAP), r = (y - ROW_Y0) / ROW_PITCH;
      int ty = ROW_Y0 + r * ROW_PITCH + TILE_DY;
      if (x >= TILE_X0 - 4 && c >= 0 && c < 3 && r >= 0 && r < 4 && y >= ty - 6 && y < ty + TILE_H + 6) {
        setPreset(r * 3 + c);
        dirty = true;
      }
      break;
    }
    case Phase::Ready:
      if (inRect(x, y, 100, BAND_Y + 2, 168, BAND_H - 4)) game.menu();  // the MENU button
      else game.tap(side, t);
      dirty = true;
      break;
    case Phase::Running:
      if (inRect(x, y, 114, BAND_Y + 2, 140, BAND_H - 4)) game.pause(t);  // the pause symbol
      else game.tap(side, t);
      dirty = true;
      break;
    case Phase::Paused:
      for (int i = 0; i < 3; i++) {
        if (!inRect(x, y, PB_X[i] - 4, PB_Y - 4, PB_W + 8, PB_H + 8)) continue;
        if (i == 0) game.resume(t);
        else if (i == 1) game.rematch();
        else game.menu();
        dirty = true;
      }
      break;
    case Phase::Flagged:
      if (inRect(x, y, 8, 198, 176, 52)) game.rematch();
      else if (inRect(x, y, 184, 198, 176, 52)) game.menu();
      dirty = true;
      break;
  }
}

// Called for every finger-down. While idle-dimmed the first touch only wakes the screen.
static void handleTouch(int x, int y, int64_t t) {
  Serial.printf("TAP x=%d y=%d phase=%d\n", x, y, (int)game.phase);
  lastActivity = millis();
  if (calStep >= 0) {
    if (++calStep >= CAL_N) calStep = -1;
    dirty = true;
    return;
  }
  bool wasIdle = asleep || dimmed;
  if (wasIdle && game.phase != Phase::Running) {
    dirty = true;
    return;
  }
  onTap(x, y, t);
}

// A real finger-down, in the chip's raw coordinates: map to the screen, then handle.
static void onRawTouch(int rx, int ry, int64_t t) {
  long lateMs = (long)((nowUs() - t) / 1000);  // how long the tap waited in the queue
  int x = constrain((int)(rx / TOUCH_SCALE_X) + TOUCH_OFF_X, 0, W - 1);
  int y = constrain((int)(ry / TOUCH_SCALE_Y) + TOUCH_OFF_Y, 0, H - 1);
  if (calStep >= 0)
    Serial.printf("CAL %d target=%d,%d raw=%d,%d mapped=%d,%d\n", calStep + 1, CAL_X[calStep], CAL_Y[calStep], rx,
                  ry, x, y);
  else
    Serial.printf("RAW %d,%d late=%ldms\n", rx, ry, lateMs);
  handleTouch(x, y, t);
}

// ----------------------------------------------------------- debug commands ----

static void writeAll(const char *buf, size_t n) {
  uint32_t t0 = millis();
  while (n && millis() - t0 < 3000) {
    size_t w = Serial.write((const uint8_t *)buf, n);
    buf += w;
    n -= w;
    if (!w) delay(1);
  }
}

// The exact frame the panel is showing, as hex rows, so the Mac can render it to a PNG.
static void dumpFrame() {
  static char line[W * 4 + 1];
  static const char HEXDIGITS[] = "0123456789abcdef";
  Serial.setTxTimeoutMs(500);
  Serial.printf("FB %d %d\n", W, H);
  const uint16_t *fb = canvas->getFramebuffer();
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      uint16_t v = fb[y * W + x];
      line[x * 4] = HEXDIGITS[v >> 12];
      line[x * 4 + 1] = HEXDIGITS[(v >> 8) & 15];
      line[x * 4 + 2] = HEXDIGITS[(v >> 4) & 15];
      line[x * 4 + 3] = HEXDIGITS[v & 15];
    }
    line[W * 4] = '\n';
    writeAll(line, sizeof line);
  }
  Serial.print("FB END\n");
  Serial.setTxTimeoutMs(0);
}

static void handleCommand(char *cmd) {
  int x, y;
  float secsF;
  if (!strcmp(cmd, "STATE")) {
    int64_t t = nowUs();
    Serial.printf("STATE phase=%d active=%d l0=%lld l1=%lld m0=%u m1=%u preset=%s flagged=%d dim=%d off=%d draw_ms=%u max_draw_ms=%u us=%lld\n",
                  (int)game.phase, game.active, (long long)game.remaining(0, t), (long long)game.remaining(1, t),
                  (unsigned)game.moves[0], (unsigned)game.moves[1], PRESETS[game.preset].label, game.flagged,
                  (int)dimmed, (int)asleep, (unsigned)lastDrawMs, (unsigned)maxDrawMs, (long long)t);
  } else if (sscanf(cmd, "TOUCH %d %d", &x, &y) == 2) {
    handleTouch(x, y, nowUs());
  } else if (sscanf(cmd, "LEFT %d %f", &x, &secsF) == 2 && (x == 0 || x == 1)) {  // test aid: set a player's time
    int64_t t = nowUs();
    game.left[x] = (int64_t)(secsF * US);
    if (game.phase == Phase::Running && game.active == x) game.turnStart = t;
    dirty = true;
  } else if (!strcmp(cmd, "CAL")) {
    calStep = 0;
    dirty = true;
  } else if (!strcmp(cmd, "DUMP")) {
    draw(nowUs());  // make sure the canvas is current
    dumpFrame();
  }
}

static void pollSerial() {
  static char buf[48];
  static int n = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      buf[n] = 0;
      if (n) handleCommand(buf);
      n = 0;
    } else if (n < (int)sizeof buf - 1) {
      buf[n++] = c;
    }
  }
}

// ------------------------------------------------------------------- setup ----

void setup() {
  Serial.setRxBufferSize(1024);
  Serial.setTxBufferSize(1024);
  Serial.begin(115200);  // USB-Serial/JTAG
  Serial.setTxTimeoutMs(0);  // never block when no host is reading
  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);
  Serial.printf("expander %s\n", expanderReset() ? "ok" : "NOT FOUND");
  touchInit();
  touchQ = xQueueCreate(8, sizeof(TouchEvent));
  if (touchOk) xTaskCreatePinnedToCore(touchTask, "touch", 4096, nullptr, 2, nullptr, 0);
  if (!gfx->begin()) Serial.println("gfx->begin() failed");
  gfx->fillScreen(C_BG);
  gfx->setBrightness(BRIGHT_ON);
  if (!canvas->begin(GFX_SKIP_OUTPUT_BEGIN)) Serial.println("canvas->begin() failed (PSRAM?)");
  prefs.begin("chess", false);
  game.preset = prefs.getUChar("preset", 5) % N_PRESETS;
  lastActivity = millis();
  Serial.printf("chess clock ready; psram=%u free=%u\n", ESP.getPsramSize(), ESP.getFreePsram());
}

void loop() {
  pollSerial();
  TouchEvent e;
  while (xQueueReceive(touchQ, &e, 0) == pdTRUE) onRawTouch(e.x, e.y, e.t);  // before tick: a tap made in time counts
  int64_t now = nowUs();
  if (game.tick(now)) dirty = true;

  // Power policy for an always-on AMOLED: never dim or blank during a game (running or paused);
  // only the picker, the ready screen and the game-over screen idle down.
  uint32_t idle = millis() - lastActivity;
  bool inGame = game.phase == Phase::Running || game.phase == Phase::Paused;
  bool wantOff = !inGame && idle > OFF_AFTER_MS;
  bool wantDim = !inGame && idle > DIM_AFTER_MS;
  if (inGame) lastActivity = millis();  // so the idle timer starts when the game ends
  if (wantOff && !asleep) {
    gfx->displayOff();
    asleep = true;
  } else if (!wantOff && asleep) {
    gfx->displayOn();
    asleep = false;
    dirty = true;
  }
  if (wantDim != dimmed) {
    gfx->setBrightness(wantDim ? BRIGHT_DIM : BRIGHT_ON);
    dimmed = wantDim;
  }

  if (!asleep && (dirty || timesChanged(now))) {
    draw(now);
    dirty = false;
  }
  delay(2);
}
