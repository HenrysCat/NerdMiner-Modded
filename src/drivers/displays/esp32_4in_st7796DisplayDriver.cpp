// "PULSE" theme for the 4.0" 320x480 ST7796 "CYD" (classic ESP32, 480x320
// landscape).
//
// Same look, screens and layout as the ES3C35P theme in
// es3c35pDisplayDriver.cpp, which has the same resolution:
//
//   MINE    segmented hashrate ring, stat tiles, pool strip
//   MARKET  BTC price, block height, fees, difficulty, halving progress
//   CLOCK   big desk clock, key numbers, hashrate history bars
//
// This board has no PSRAM, so there is no frame buffer: as on the 2.8" CYD
// (esp23_2432s028r.cpp) the chrome is drawn straight to the panel and every
// widget goes through a small scratch sprite.
#include "displayDriver.h"

#ifdef ESP32_4IN_ST7796_DISPLAY

#include <TFT_eSPI.h>
#include <WiFi.h>
#include <SPI.h>
#include <Preferences.h>
#include "media/myFonts.h"
#include "version.h"
#include "monitor.h"
#include "OpenFontRender.h"
#include "drivers/storage/nvMemory.h"
#include "drivers/storage/storage.h"
#include "currency.h"

extern nvMemory nvMem;
extern monitor_data mMonitor;
extern pool_data pData;
extern DisplayDriver *currentDisplayDriver;
extern TSettings Settings;
extern unsigned long mPoolUpdate;
extern String readCustomAPName(); // wManager.cpp

static OpenFontRender render;
static TFT_eSPI tft = TFT_eSPI();           // pins defined in platformio.ini
static TFT_eSprite wgt = TFT_eSprite(&tft); // scratch sprite reused by every widget

#define SCR_W 480
#define SCR_H 320

// Landscape with the USB socket on the left, or (flipped) on the right
#define ROT_NORMAL 3
#define ROT_FLIPPED 1

// ================================================================== touch ==
// XPT2046 on the panel's SPI bus. Reads go through the SPI object TFT_eSPI
// uses: its transaction lock keeps them apart from the panel writes made on
// the monitor task.

#define TOUCH_SPI_HZ 2500000
#define TOUCH_Z_MIN 300 // a pressure reading below this is not a touch

// Raw readings at the edges of the panel, measured on hardware with the USB
// socket on the left: the controller's Y channel runs left to right along the
// long side, its X channel top to bottom along the short side.
#define TOUCH_LONG_MIN 350
#define TOUCH_LONG_MAX 3870
#define TOUCH_SHORT_MIN 260
#define TOUCH_SHORT_MAX 3740

static int touchRawL = 0, touchRawS = 0; // last point as the controller reported it

static int touchSample(SPIClass &spi, uint8_t cmd)
{
  spi.transfer(cmd);
  return spi.transfer16(0) >> 3;
}

// Touch point in screen (landscape) coordinates.
static bool touchGetXY(int &x, int &y)
{
  if (digitalRead(TOUCH_PIN_IRQ) == HIGH)
    return false;

  SPIClass &spi = TFT_eSPI::getSPIinstance();
  spi.beginTransaction(SPISettings(TOUCH_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_PIN_CS, LOW);
  const int z = 4095 + touchSample(spi, 0xB0) - touchSample(spi, 0xC0);
  int l = 0, s = 0;
  if (z >= TOUCH_Z_MIN)
  {
    touchSample(spi, 0xD0); // the first conversion after the pressure ones has not settled
    for (int i = 0; i < 4; i++)
    {
      s += touchSample(spi, 0xD0);
      l += touchSample(spi, 0x90);
    }
  }
  digitalWrite(TOUCH_PIN_CS, HIGH);
  spi.endTransaction();
  if (z < TOUCH_Z_MIN)
    return false;

  touchRawL = l / 4;
  touchRawS = s / 4;
  x = constrain(map(touchRawL, TOUCH_LONG_MIN, TOUCH_LONG_MAX, 0, SCR_W - 1), 0, SCR_W - 1);
  y = constrain(map(touchRawS, TOUCH_SHORT_MIN, TOUCH_SHORT_MAX, 0, SCR_H - 1), 0, SCR_H - 1);
  if (tft.getRotation() == ROT_FLIPPED)
  {
    x = SCR_W - 1 - x;
    y = SCR_H - 1 - y;
  }
  return true;
}

// ---------------------------------------------------------------- palette --

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

static const uint16_t C_BG = RGB565(5, 7, 13);      // near-black ink
static const uint16_t C_PANEL = RGB565(13, 18, 30); // raised surface
static const uint16_t C_EDGE = RGB565(30, 41, 62);  // hairlines
static const uint16_t C_TRACK = RGB565(22, 30, 46); // unlit gauge / bar tracks
static const uint16_t C_TEXT = RGB565(232, 238, 248);
static const uint16_t C_DIM = RGB565(112, 128, 156);
static const uint16_t C_GOOD = RGB565(61, 255, 176);
static const uint16_t C_WARN = RGB565(255, 176, 40);
static const uint16_t C_GOLD = RGB565(255, 206, 64);

struct Accent
{
  uint16_t main; // hot end of gradients, wordmark, dots
  uint16_t alt;  // cool end of gradients
};

static const Accent ACCENTS[] = {
    {RGB565(34, 225, 255), RGB565(140, 92, 255)}, // ice / violet
    {RGB565(61, 255, 176), RGB565(38, 148, 255)}, // mint / azure
    {RGB565(255, 176, 40), RGB565(255, 72, 112)}, // amber / rose
    {RGB565(255, 84, 190), RGB565(122, 92, 255)}, // magenta / indigo
    {RGB565(190, 255, 60), RGB565(30, 208, 160)}, // lime / teal
};
#define ACCENT_COUNT (sizeof(ACCENTS) / sizeof(ACCENTS[0]))

static int accentIdx = 0;
static uint16_t cAcc = ACCENTS[0].main;
static uint16_t cAlt = ACCENTS[0].alt;

// t = 0..255 blends a -> b
static uint16_t mix565(uint16_t a, uint16_t b, uint8_t t)
{
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + ((br - ar) * t) / 255;
  int g = ag + ((bg - ag) * t) / 255;
  int bl = ab + ((bb - ab) * t) / 255;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// --------------------------------------------------------------- geometry --

#define HDR_H 34 // header band; hairline sits on the row below

#define SCR_MINE 0
#define SCR_MARKET 1
#define SCR_CLOCK 2

// Hashrate ring
#define G_CX 140
#define G_CY 148
#define G_R 104
#define G_RI 90
#define G_SEGS 30
#define G_MAX_KH 1200.0f // full-scale of the ring: 40 KH/s per segment (this board runs ~950)

// Bottom strip (pool cells / hashrate history)
#define STRIP_Y 264
#define STRIP_H 48

// --------------------------------------------------------------- UI state --

static int uiScreen = -1;    // screen whose chrome is currently on the panel
static bool uiLive = false;  // false while the boot / setup screens own the panel
static float gaugeTarget = 0;
static float gaugeShown = 0;
static uint8_t segLvl[G_SEGS];
static unsigned long shareFlashUntil = 0;
static uint32_t lastShares = 0;

// history of hashrate for the CLOCK screen
#define HIST_N 48
#define HIST_EVERY_MS 10000
static float hist[HIST_N];
static uint8_t histCount = 0;
static uint32_t histEpoch = 0;
static unsigned long histLastMs = 0;

// Every widget remembers the "key" it last drew so it only repaints on change.
enum WidgetId
{
  W_HDR,
  W_HERO,
  W_TOTAL,
  W_T0, // 4 tiles
  W_T1,
  W_T2,
  W_T3,
  W_P0, // 4 strip cells
  W_P1,
  W_P2,
  W_P3,
  W_PRICE,
  W_BLOCK,
  W_M0, // 3 market tiles
  W_M1,
  W_M2,
  W_HALV,
  W_HH,
  W_COLON,
  W_MM,
  W_DATE,
  W_C0, // 3 clock tiles
  W_C1,
  W_C2,
  W_CHART,
  W_COUNT
};
static String wkey[W_COUNT];

static bool need(int id, const String &k) { return wkey[id] != k; }
static void done(int id, const String &k) { wkey[id] = k; }
static void resetWidgets()
{
  for (int i = 0; i < W_COUNT; i++)
    wkey[i] = "\x01";
}

// ---------------------------------------------------------- output target --
// Widgets are drawn into a scratch sprite and pushed to the panel.

static bool wBegin(int w, int h, uint16_t bg)
{
  // The largest widgets are ~35KB at 16 bit, a big block for a heap shared
  // with WiFi and TLS: when there is none, draw the widget at 8 bit (half the
  // memory, slightly coarser colours) rather than not at all.
  wgt.setColorDepth(16);
  wgt.createSprite(w, h);
  if (!wgt.created())
  {
    wgt.setColorDepth(8);
    wgt.createSprite(w, h);
  }
  if (!wgt.created())
  {
    Serial.printf("[ui] sprite %dx%d alloc failed (free %u, max block %u)\n", w, h,
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return false;
  }
  wgt.fillSprite(bg);
  return true;
}

static void wEnd(int x, int y)
{
  wgt.pushSprite(x, y);
  wgt.deleteSprite();
}

// ------------------------------------------------------------ draw helpers --

// fillScreen() is not virtual in TFT_eSPI, so it would ignore sprite targets
static void clearScreen(TFT_eSPI &d) { d.fillRect(0, 0, SCR_W, SCR_H, C_BG); }

static void text(TFT_eSPI &d, const char *s, int x, int y, unsigned size, uint16_t fg, uint16_t bg,
                 Align a = Align::TopLeft)
{
  render.setDrawer(d);
  render.setFontSize(size);
  render.setAlignment(a);
  render.drawString(s, x, y, fg, bg);
}

static int textW(const char *s, unsigned size)
{
  render.setFontSize(size);
  return render.getTextWidth("%s", s);
}

// value + smaller unit, centred on cx
static void textPair(TFT_eSPI &d, int cx, int y, const char *val, const char *unit, unsigned size,
                     uint16_t cv, uint16_t cu, uint16_t bg)
{
  unsigned usz = size > 12 ? size - 3 : size;
  int wv = textW(val, size);
  int wu = textW(unit, usz);
  int x0 = cx - (wv + 5 + wu) / 2;
  text(d, val, x0, y, size, cv, bg);
  text(d, unit, x0 + wv + 5, y + (size - usz), usz, cu, bg);
}

// rounded card: 1px edge, AA corners
static void panel(TFT_eSPI &d, int x, int y, int w, int h, int r, uint16_t fill, uint16_t edge, uint16_t behind)
{
  d.fillSmoothRoundRect(x, y, w, h, r, edge, behind);
  d.fillSmoothRoundRect(x + 1, y + 1, w - 2, h - 2, r - 1, fill, edge);
}

// horizontal gradient bar (alt -> accent) with softly rounded ends
static void gradBar(TFT_eSPI &d, int x, int y, int w, int h)
{
  for (int i = 0; i < w; i++)
  {
    int inset = 0;
    int e = i < w - 1 - i ? i : w - 1 - i; // distance to nearest end
    if (h >= 6)
      inset = e < 1 ? 2 : (e < 3 ? 1 : 0);
    uint16_t c = mix565(cAlt, cAcc, (uint8_t)(w > 1 ? i * 255 / (w - 1) : 255));
    d.drawFastVLine(x + i, y + inset, h - 2 * inset, c);
  }
}

static String commas(const String &s)
{
  int n = s.length();
  for (int i = 0; i < n; i++)
    if (!isDigit(s[i]))
      return s;
  String o;
  for (int i = 0; i < n; i++)
  {
    o += s[i];
    int rem = n - 1 - i;
    if (rem > 0 && rem % 3 == 0)
      o += ',';
  }
  return o;
}

static String fmtPrice(const String &s)
{
  // btcPrice is "<ascii prefix><digits>"; swap the prefix for the real symbol
  // (the embedded Pulse font carries the glyphs for all of them)
  int i = 0;
  while (i < (int)s.length() && !isdigit((unsigned char)s[i]))
    i++;
  String digits = s.substring(i);
  if (digits.toInt() <= 0)
    return "--";
  return String(currencyFor(Settings.Currency).symbol) + commas(digits);
}

static String btcPairLabel()
{
  String c = currencyFor(Settings.Currency).code;
  c.toUpperCase();
  return "BTC / " + c;
}

static String fmtUptime(const String &s)
{
  int d = 0, h = 0, m = 0, sec = 0;
  sscanf(s.c_str(), "%d %d:%d:%d", &d, &h, &m, &sec);
  char b[16];
  if (d > 0)
    snprintf(b, sizeof(b), "%dd %02dh", d, h);
  else
    snprintf(b, sizeof(b), "%02d:%02d:%02d", h, m, sec);
  return String(b);
}

static String fmtDate(const String &s)
{
  static const char *mon[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  static const char *wd[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int d = 0, m = 0, y = 0;
  if (sscanf(s.c_str(), "%d/%d/%d", &d, &m, &y) != 3 || m < 1 || m > 12)
    return "";
  int yy = y - (m < 3 ? 1 : 0);
  int dow = (yy + yy / 4 - yy / 100 + yy / 400 + t[m - 1] + d) % 7;
  char b[32];
  snprintf(b, sizeof(b), "%s  %d %s %d", wd[dow], d, mon[m - 1], y);
  return String(b);
}

static int wifiBars()
{
  if (WiFi.status() != WL_CONNECTED)
    return 0;
  int r = WiFi.RSSI();
  return r > -55 ? 4 : r > -65 ? 3
                   : r > -75   ? 2
                   : r > -85   ? 1
                               : 0;
}

static float lastKh = 0;

// The firmware never sets NM_hashing itself (status stays NM_Connecting once
// WiFi is up), so derive what the header shows from what is actually happening.
static int uiStatus()
{
  if (mMonitor.NerdStatus == NM_waitingConfig)
    return NM_waitingConfig;
  return (WiFi.status() == WL_CONNECTED && lastKh > 0) ? NM_hashing : NM_Connecting;
}

static void noteHashrate(float kh)
{
  lastKh = kh;
  unsigned long now = millis();
  if (histCount != 0 && now - histLastMs < HIST_EVERY_MS)
    return;
  histLastMs = now;
  if (histCount < HIST_N)
    hist[histCount++] = kh;
  else
  {
    memmove(hist, hist + 1, (HIST_N - 1) * sizeof(float));
    hist[HIST_N - 1] = kh;
  }
  histEpoch++;
}

// -------------------------------------------------------------- gauge ring --

static void gaugeDraw(TFT_eSPI &d, float v)
{
  float pos = v * G_SEGS;
  for (int i = 0; i < G_SEGS; i++)
  {
    float f = pos - i;
    uint8_t lvl = f >= 1.0f ? 4 : (f <= 0.05f ? 0 : 1 + (int)(f * 3.0f)); // 0, 1..3 (partial), 4
    if (lvl == segLvl[i])
      continue;
    segLvl[i] = lvl;
    uint16_t full = mix565(cAlt, cAcc, (uint8_t)(i * 255 / (G_SEGS - 1)));
    uint16_t col = lvl == 0 ? C_TRACK : (lvl == 4 ? full : mix565(C_TRACK, full, lvl * 64));
    int a0 = 45 + i * 9; // 270 degree sweep, gap at 6 o'clock
    d.drawArc(G_CX, G_CY, G_R, G_RI, a0, a0 + 7, col, C_BG, true);
  }
}

// ------------------------------------------------------------------ header --

struct HdrV
{
  String time;
  String ip;
  int bars;
  int status;
};

static void drawHeader(const HdrV &h)
{
  char k[64];
  snprintf(k, sizeof(k), "%s|%s|%d|%d|%d", h.time.c_str(), h.ip.c_str(), h.bars, h.status, accentIdx);
  if (!need(W_HDR, k))
    return;
  const int X0 = 32, W = SCR_W - X0;
  if (!wBegin(W, HDR_H, C_BG))
    return;

  // wordmark
  int wn = textW("NERD", 19);
  text(wgt, "NERD", 2, 6, 19, C_TEXT, C_BG);
  text(wgt, "MINER", 2 + wn + 1, 6, 19, cAcc, C_BG);
  int wm = 2 + wn + 1 + textW("MINER", 19);

  // state chip
  const char *label = "SETUP";
  uint16_t col = C_DIM;
  if (h.status == NM_hashing)
  {
    label = "HASHING";
    col = C_GOOD;
  }
  else if (h.status == NM_Connecting)
  {
    label = "LINKING";
    col = C_WARN;
  }
  int cw = textW(label, 12) + 18;
  int cx = wm + 12;
  wgt.fillSmoothRoundRect(cx, 7, cw, 20, 9, mix565(C_BG, col, 46), C_BG);
  text(wgt, label, cx + cw / 2, 10, 12, col, mix565(C_BG, col, 46), Align::TopCenter);

  // time (right)
  text(wgt, h.time.c_str(), W - 10, 6, 19, C_TEXT, C_BG, Align::TopRight);

  // wifi bars
  for (int i = 0; i < 4; i++)
  {
    int bh = 5 + i * 4;
    wgt.fillRect(356 + i * 7, 26 - bh, 4, bh, i < h.bars ? cAcc : C_TRACK);
  }

  // local IP, right-aligned before the wifi bars; shrinks to fit beside the state chip
  if (h.ip.length())
  {
    const int right = 344, room = right - (cx + cw) - 10;
    unsigned sz = 16;
    while (sz > 10 && textW(h.ip.c_str(), sz) > room)
      sz--;
    text(wgt, h.ip.c_str(), right, 17 - (int)sz / 2 - 1, sz, C_DIM, C_BG, Align::TopRight);
  }

  wEnd(X0, 0);
  done(W_HDR, k);
}

// Breathing status dot, left of the wordmark. Called every animation frame.
static void drawPulse(unsigned long frame)
{
  static int lastLvl = -1;
  int ph = frame % 20;
  int lvl = ph < 10 ? ph : 20 - ph; // 0..10..0
  int st = uiStatus();
  uint16_t base = st == NM_hashing ? cAcc : (st == NM_Connecting ? C_WARN : C_DIM);
  int key = lvl + 11 * st + 40 * accentIdx;
  if (key == lastLvl)
    return;
  lastLvl = key;
  tft.fillSmoothCircle(17, 17, 5, mix565(C_TRACK, base, 70 + lvl * 18), C_BG);
}

// -------------------------------------------------------------- pool strip --

struct StripV
{
  String l[4];
  String v[4];
};

static void chromeStrip(TFT_eSPI &d)
{
  panel(d, 10, STRIP_Y, 460, STRIP_H, 10, C_PANEL, C_EDGE, C_BG);
}

static void drawStrip(const StripV &s)
{
  for (int i = 0; i < 4; i++)
  {
    String k = s.l[i] + "|" + s.v[i] + "|" + accentIdx;
    if (!need(W_P0 + i, k))
      continue;
    if (!wBegin(104, 40, C_PANEL))
      continue;
    text(wgt, s.l[i].c_str(), 8, 3, 12, C_DIM, C_PANEL);
    text(wgt, s.v[i].c_str(), 8, 18, 19, C_TEXT, C_PANEL);
    wEnd(20 + 112 * i, STRIP_Y + 4);
    done(W_P0 + i, k);
  }
}

static void chromeStripCells(TFT_eSPI &d)
{
  chromeStrip(d);
  for (int i = 1; i < 4; i++)
    d.drawFastVLine(16 + 112 * i, STRIP_Y + 10, STRIP_H - 20, C_EDGE);
}

// generic stat tile (label over value, coloured stripe at the left)
static void drawTile(int id, int x, int y, int w, int h, const char *label, const char *value,
                     uint16_t stripe, uint16_t valCol, uint16_t edge)
{
  char k[64];
  snprintf(k, sizeof(k), "%s|%s|%u|%u|%u", label, value, stripe, valCol, edge);
  if (!need(id, k))
    return;
  if (!wBegin(w, h, C_BG))
    return;
  panel(wgt, 0, 0, w, h, 10, C_PANEL, edge, C_BG);
  int top = (h - 38) / 2;
  wgt.fillSmoothRoundRect(10, 10, 4, h - 20, 2, stripe, C_PANEL);
  text(wgt, label, 24, top, 12, C_DIM, C_PANEL);
  text(wgt, value, 24, top + 16, strlen(value) > 9 ? 16 : 20, valCol, C_PANEL);
  wEnd(x, y);
  done(id, k);
}

// ============================================================ MINE screen ==

struct MineV
{
  HdrV hdr;
  String hash, shares, best, tmpl, blocks, total;
  float kh;
  bool flash;
  StripV strip;
};

static const int TILE_Y[4] = {42, 96, 150, 204};

static void chromeMine(TFT_eSPI &d)
{
  memset(segLvl, 0xFF, sizeof(segLvl));
  gaugeDraw(d, gaugeShown);
  chromeStripCells(d);
}

static void paintMine(const MineV &m)
{
  drawHeader(m.hdr);

  // hero readout inside the ring
  String k = m.hash + "|" + accentIdx;
  if (need(W_HERO, k))
  {
    if (wBegin(140, 100, C_BG))
    {
      text(wgt, "HASHRATE", 70, 5, 12, C_DIM, C_BG, Align::TopCenter);
      unsigned sz = m.hash.length() <= 5 ? 48 : (m.hash.length() == 6 ? 40 : 34);
      text(wgt, m.hash.c_str(), 70, 20, sz, C_TEXT, C_BG, Align::TopCenter);
      text(wgt, "KH/s", 70, 75, 17, cAcc, C_BG, Align::TopCenter);
      wEnd(G_CX - 70, G_CY - 50);
      done(W_HERO, k);
    }
  }

  // lifetime hashes, in the gap at the bottom of the ring
  k = m.total + "|" + accentIdx;
  if (need(W_TOTAL, k))
  {
    if (wBegin(160, 22, C_BG))
    {
      textPair(wgt, 80, 2, commas(m.total).c_str(), "MH TOTAL", 15, C_TEXT, C_DIM, C_BG);
      wEnd(G_CX - 80, 232);
      done(W_TOTAL, k);
    }
  }

  const int tx = 262, tw = 208, th = 48;
  drawTile(W_T0, tx, TILE_Y[0], tw, th, "SHARES", m.shares.c_str(), cAcc, C_TEXT, m.flash ? cAcc : C_EDGE);
  drawTile(W_T1, tx, TILE_Y[1], tw, th, "BEST DIFF", m.best.c_str(), cAlt, C_TEXT, C_EDGE);
  drawTile(W_T2, tx, TILE_Y[2], tw, th, "TEMPLATES", m.tmpl.c_str(), C_DIM, C_TEXT, C_EDGE);
  bool found = m.blocks.toInt() > 0;
  drawTile(W_T3, tx, TILE_Y[3], tw, th, "BLOCKS FOUND", m.blocks.c_str(), found ? C_GOLD : C_GOOD,
           found ? C_GOLD : C_TEXT, found ? C_GOLD : C_EDGE);

  drawStrip(m.strip);
}

// ========================================================== MARKET screen ==

struct MarketV
{
  HdrV hdr;
  String price, block, fee, diff, net;
  int pct;
  String remaining;
  StripV strip;
};

static void chromeMarket(TFT_eSPI &d)
{
  panel(d, 10, 42, 460, 92, 12, C_PANEL, C_EDGE, C_BG);
  text(d, btcPairLabel().c_str(), 26, 51, 12, C_DIM, C_PANEL);
  text(d, "LATEST BLOCK", 454, 51, 12, C_DIM, C_PANEL, Align::TopRight);
  panel(d, 10, 206, 460, 50, 10, C_PANEL, C_EDGE, C_BG);
  chromeStripCells(d);
}

static void paintMarket(const MarketV &m)
{
  drawHeader(m.hdr);

  String k = m.price + "|" + accentIdx;
  if (need(W_PRICE, k) && wBegin(264, 62, C_PANEL))
  {
    const unsigned len = m.price.length();
    text(wgt, m.price.c_str(), 2, 0, len > 8 ? 50 * 8 / len : 50, C_TEXT, C_PANEL);
    wEnd(24, 68);
    done(W_PRICE, k);
  }

  k = m.block + "|" + accentIdx;
  if (need(W_BLOCK, k) && wBegin(164, 62, C_PANEL))
  {
    text(wgt, m.block.c_str(), 160, 22, 30, cAcc, C_PANEL, Align::TopRight);
    wEnd(292, 68);
    done(W_BLOCK, k);
  }

  const int y = 142, w = 148, h = 56;
  drawTile(W_M0, 10, y, w, h, "FEE / HALF HOUR", m.fee.c_str(), cAcc, C_TEXT, C_EDGE);
  drawTile(W_M1, 166, y, w, h, "DIFFICULTY", m.diff.c_str(), cAlt, C_TEXT, C_EDGE);
  drawTile(W_M2, 322, y, w, h, "NETWORK RATE", m.net.c_str(), C_DIM, C_TEXT, C_EDGE);

  char kb[48];
  snprintf(kb, sizeof(kb), "%d|%s|%d", m.pct, m.remaining.c_str(), accentIdx);
  if (need(W_HALV, kb) && wBegin(436, 40, C_PANEL))
  {
    text(wgt, "NEXT HALVING", 4, 2, 12, C_DIM, C_PANEL);
    char pc[8];
    snprintf(pc, sizeof(pc), "%d%%", m.pct);
    text(wgt, pc, 4 + textW("NEXT HALVING", 12) + 10, 0, 15, cAcc, C_PANEL);
    String rem = commas(m.remaining) + " blocks left";
    text(wgt, rem.c_str(), 432, 1, 13, C_TEXT, C_PANEL, Align::TopRight);
    wgt.fillSmoothRoundRect(4, 24, 428, 10, 5, C_TRACK, C_PANEL);
    int fw = (428 * m.pct) / 100;
    if (fw < 10 && m.pct > 0)
      fw = 10;
    if (fw > 0)
      gradBar(wgt, 4, 24, fw, 10);
    wEnd(22, 211);
    done(W_HALV, kb);
  }

  drawStrip(m.strip);
}

// ============================================================ CLOCK screen ==

struct ClockV
{
  HdrV hdr;
  String hh, mm;
  bool colon;
  String date, price, block, hash;
};

static void chromeClock(TFT_eSPI &d)
{
  chromeStrip(d);
}

static void drawChart()
{
  char k[24];
  snprintf(k, sizeof(k), "%lu|%u|%d", (unsigned long)histEpoch, histCount, accentIdx);
  if (!need(W_CHART, k))
    return;
  const int W = 432, H = 38;
  if (!wBegin(W, H, C_PANEL))
    return;
  float mx = 1;
  for (int i = 0; i < histCount; i++)
    if (hist[i] > mx)
      mx = hist[i];
  mx *= 1.08f;
  const int step = 9;
  for (int i = 0; i < HIST_N; i++)
  {
    int slot = i - (HIST_N - histCount); // right-align so the newest bar is on the right
    int x = i * step;
    if (slot < 0)
    {
      wgt.fillRect(x, H - 2, 7, 2, C_TRACK); // not sampled yet
      continue;
    }
    int bh = (int)(H * hist[slot] / mx);
    if (bh < 2)
      bh = 2;
    uint16_t c = mix565(cAlt, cAcc, (uint8_t)(i * 255 / (HIST_N - 1)));
    wgt.fillRect(x, H - bh, 7, bh, c);
  }
  wEnd(24, STRIP_Y + 5);
  done(W_CHART, k);
}

static void paintClock(const ClockV &c)
{
  drawHeader(c.hdr);

  // Glyph boxes start well above the digits (ascender space), so draw the
  // text above the widget: the digits then fill it.
  String k = c.hh + "|" + accentIdx;
  if (need(W_HH, k) && wBegin(168, 94, C_BG))
  {
    text(wgt, c.hh.c_str(), 166, -42, 124, C_TEXT, C_BG, Align::TopRight);
    wEnd(52, 40);
    done(W_HH, k);
  }
  k = c.mm + "|" + accentIdx;
  if (need(W_MM, k) && wBegin(168, 94, C_BG))
  {
    text(wgt, c.mm.c_str(), 2, -42, 124, cAcc, C_BG, Align::TopLeft);
    wEnd(260, 40);
    done(W_MM, k);
  }
  k = String(c.colon) + "|" + accentIdx;
  if (need(W_COLON, k) && wBegin(40, 94, C_BG))
  {
    uint16_t dot = c.colon ? C_DIM : C_BG;
    wgt.fillSmoothCircle(20, 30, 7, dot, C_BG);
    wgt.fillSmoothCircle(20, 63, 7, dot, C_BG);
    wEnd(220, 40);
    done(W_COLON, k);
  }

  k = c.date;
  if (need(W_DATE, k) && wBegin(332, 24, C_BG))
  {
    text(wgt, c.date.c_str(), 166, 2, 16, C_DIM, C_BG, Align::TopCenter);
    wEnd(74, 142);
    done(W_DATE, k);
  }

  const int y = 180, w = 148, h = 64;
  drawTile(W_C0, 10, y, w, h, btcPairLabel().c_str(), c.price.c_str(), cAcc, C_TEXT, C_EDGE);
  drawTile(W_C1, 166, y, w, h, "BLOCK", c.block.c_str(), cAlt, C_TEXT, C_EDGE);
  drawTile(W_C2, 322, y, w, h, "HASHRATE KH/s", c.hash.c_str(), C_GOOD, C_TEXT, C_EDGE);

  drawChart();
}

// ------------------------------------------------------ boot / setup pages --

static void paintLoading(TFT_eSPI &d)
{
  clearScreen(d);
  // full-circle segmented ring, gradient around it
  const int cx = 240, cy = 116, r = 68, ri = 56, n = 24;
  for (int i = 0; i < n; i++)
  {
    uint16_t c = mix565(cAlt, cAcc, (uint8_t)(i * 255 / (n - 1)));
    int a0 = i * 15;
    d.drawArc(cx, cy, r, ri, a0, a0 + 11, c, C_BG, true);
  }
  text(d, "#", cx, cy - 37, 74, C_TEXT, C_BG, Align::TopCenter);
  int wn = textW("NERD", 40), wm = textW("MINER", 40);
  int x0 = cx - (wn + wm + 3) / 2;
  text(d, "NERD", x0, 200, 40, C_TEXT, C_BG);
  text(d, "MINER", x0 + wn + 3, 200, 40, cAcc, C_BG);
  text(d, CURRENT_VERSION, cx, 258, 16, C_DIM, C_BG, Align::TopCenter);
  text(d, "STARTING UP", cx, 288, 12, C_DIM, C_BG, Align::TopCenter);
}

static void paintSetup(TFT_eSPI &d)
{
  clearScreen(d);
  text(d, "SETUP", 20, 10, 32, C_TEXT, C_BG);
  text(d, "MODE", 20 + textW("SETUP", 32) + 9, 10, 32, cAcc, C_BG);
  text(d, "Join the miner's WiFi to configure it", 20, 54, 15, C_DIM, C_BG);

  String ap = readCustomAPName();
  if (ap.length() == 0)
    ap = DEFAULT_SSID;
  const char *lab[3] = {"JOIN THIS WIFI NETWORK", "PASSWORD", "THEN OPEN IN A BROWSER"};
  const char *val[3] = {ap.c_str(), DEFAULT_WIFIPW, "192.168.4.1"};
  for (int i = 0; i < 3; i++)
  {
    int y = 86 + i * 66;
    panel(d, 10, y, 460, 58, 12, C_PANEL, C_EDGE, C_BG);
    d.fillSmoothCircle(44, y + 29, 17, mix565(C_PANEL, cAcc, 60), C_PANEL);
    char n[2] = {(char)('1' + i), 0};
    text(d, n, 44, y + 16, 22, cAcc, mix565(C_PANEL, cAcc, 60), Align::TopCenter);
    text(d, lab[i], 78, y + 8, 12, C_DIM, C_PANEL);
    text(d, val[i], 78, y + 24, 22, C_TEXT, C_PANEL);
  }
  text(d, "Save the form and the miner restarts by itself", 240, 292, 13, C_DIM, C_BG, Align::TopCenter);
}

// ---------------------------------------------------------------- chrome --

static void paintChrome(TFT_eSPI &d, int scr)
{
  clearScreen(d);
  d.drawFastHLine(0, HDR_H, SCR_W, C_EDGE);
  switch (scr)
  {
  case SCR_MINE:
    chromeMine(d);
    break;
  case SCR_MARKET:
    chromeMarket(d);
    break;
  default:
    chromeClock(d);
    break;
  }
}

static void ensureChrome(int scr)
{
  if (uiScreen == scr)
    return;
  uiScreen = scr;
  resetWidgets();
  paintChrome(tft, scr);
}

static void repaintCached(int scr);

static void setAccent(int idx)
{
  accentIdx = idx % ACCENT_COUNT;
  cAcc = ACCENTS[accentIdx].main;
  cAlt = ACCENTS[accentIdx].alt;
  if (uiLive)
  {
    int s = currentDisplayDriver->current_cyclic_screen;
    uiScreen = -1;
    ensureChrome(s);
    repaintCached(s);
  }
}

// Last view painted per screen, so a page change can repaint instantly
// instead of waiting for the next (possibly network-blocked) data tick.
static MineV lastMine;
static MarketV lastMarket;
static ClockV lastClock;
static bool haveMine = false, haveMarket = false, haveClock = false;

static void repaintCached(int scr)
{
  if (scr == SCR_MINE && haveMine)
    paintMine(lastMine);
  else if (scr == SCR_MARKET && haveMarket)
    paintMarket(lastMarket);
  else if (scr == SCR_CLOCK && haveClock)
    paintClock(lastClock);
}

// ------------------------------------------------------- saved UI choices --
// Accent colour and last-viewed page live in their own NVS namespace (the
// pool/wallet/brightness/invert/flip settings are in the main config file).

#define UI_PREFS_NS "pulse_ui"

static int savedAccent = 0;
static int savedPage = 0;
static int restorePage = 0; // applied on the first data tick (the monitor resets the page to 0 at start)

static void loadUiPrefs()
{
  Preferences p;
  if (!p.begin(UI_PREFS_NS, true))
    return; // namespace not created yet: defaults
  accentIdx = p.getUChar("accent", 0) % ACCENT_COUNT;
  int page = p.getUChar("page", 0);
  p.end();
  cAcc = ACCENTS[accentIdx].main;
  cAlt = ACCENTS[accentIdx].alt;
  savedAccent = accentIdx;
  savedPage = restorePage = page;
}

static void saveUiPrefs()
{
  int page = currentDisplayDriver->current_cyclic_screen;
  if (page == savedPage && accentIdx == savedAccent)
    return;
  Preferences p;
  if (p.begin(UI_PREFS_NS, false))
  {
    p.putUChar("accent", accentIdx);
    p.putUChar("page", page);
    p.end();
  }
  savedAccent = accentIdx;
  savedPage = page;
}

// -------------------------------------------------------------- live data --

static void refreshPool()
{
  if (Settings.PoolAddress != "tn.vkbit.com")
  {
    pData = getPoolData(); // throttled internally (UPDATE_POOL_min)
  }
  else
  {
    pData.bestDifficulty = "TESTNET";
    pData.workersHash = "TESTNET";
    pData.workersCount = 1;
    mPoolUpdate = millis();
  }
}

static HdrV liveHeader(const String &time)
{
  HdrV h;
  h.time = time;
  h.ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String();
  h.bars = wifiBars();
  h.status = uiStatus();
  return h;
}

static void logShare(const String &shares, const String &khashes, const String &rate)
{
  // Parsed by the bench scripts (see project notes) -- keep the format.
  Serial.printf(">>> Completed %s share(s), %s Khashes, avg. hashrate %s KH/s\n",
                shares.c_str(), khashes.c_str(), rate.c_str());
}

void esp32_4in_MinerScreen(unsigned long mElapsed)
{
  if (restorePage > 0 && restorePage < currentDisplayDriver->num_cyclic_screens)
  {
    // First tick after boot: go straight to the page that was showing before.
    int p = restorePage;
    restorePage = 0;
    currentDisplayDriver->current_cyclic_screen = p;
    currentDisplayDriver->cyclic_screens[p](mElapsed);
    return;
  }
  restorePage = 0;
  mining_data data = getMiningData(mElapsed);
  refreshPool();
  uiLive = true;
  ensureChrome(SCR_MINE);

  float kh = data.currentHashRate.toFloat();
  noteHashrate(kh);
  gaugeTarget = constrain(kh / G_MAX_KH, 0.0f, 1.0f);

  uint32_t sh = data.completedShares.toInt();
  if (sh > lastShares && lastShares != 0)
    shareFlashUntil = millis() + 1500;
  lastShares = sh;

  MineV m;
  m.hdr = liveHeader(data.currentTime);
  m.hash = data.currentHashRate;
  m.shares = commas(data.completedShares);
  m.best = data.bestDiff;
  m.tmpl = commas(data.templates);
  m.blocks = data.valids;
  m.total = data.totalMHashes;
  m.kh = kh;
  m.flash = millis() < shareFlashUntil;
  m.strip.l[0] = "WORKERS";
  m.strip.v[0] = String(pData.workersCount);
  m.strip.l[1] = "POOL RATE";
  m.strip.v[1] = pData.workersHash;
  m.strip.l[2] = "POOL BEST";
  m.strip.v[2] = pData.bestDifficulty;
  m.strip.l[3] = "UPTIME";
  m.strip.v[3] = fmtUptime(data.timeMining);
  lastMine = m;
  haveMine = true;
  paintMine(m);

  logShare(data.completedShares, data.totalKHashes, data.currentHashRate);
}

void esp32_4in_MarketScreen(unsigned long mElapsed)
{
  coin_data data = getCoinData(mElapsed);
  refreshPool();
  uiLive = true;
  ensureChrome(SCR_MARKET);
  noteHashrate(data.currentHashRate.toFloat());

  MarketV m;
  m.hdr = liveHeader(data.currentTime);
  m.price = fmtPrice(data.btcPrice);
  m.block = commas(data.blockHeight);
  m.fee = data.halfHourFee.startsWith("0 ") ? String("--") : data.halfHourFee;
  m.diff = data.netwrokDifficulty.length() ? data.netwrokDifficulty : String("--");
  m.net = data.globalHashRate.length() ? data.globalHashRate + " EH/s" : String("--");
  m.pct = (int)data.progressPercent;
  m.remaining = String(data.remainingBlocks.toInt());
  m.strip.l[0] = "WORKERS";
  m.strip.v[0] = String(pData.workersCount);
  m.strip.l[1] = "POOL RATE";
  m.strip.v[1] = pData.workersHash;
  m.strip.l[2] = "POOL BEST";
  m.strip.v[2] = pData.bestDifficulty;
  m.strip.l[3] = "SHARES";
  m.strip.v[3] = commas(data.completedShares);
  lastMarket = m;
  haveMarket = true;
  paintMarket(m);

  logShare(data.completedShares, data.totalKHashes, data.currentHashRate);
}

void esp32_4in_ClockScreen(unsigned long mElapsed)
{
  clock_data data = getClockData(mElapsed);
  uiLive = true;
  ensureChrome(SCR_CLOCK);
  noteHashrate(data.currentHashRate.toFloat());

  ClockV c;
  c.hdr = liveHeader(data.currentTime);
  int colon = data.currentTime.indexOf(':');
  c.hh = colon > 0 ? data.currentTime.substring(0, colon) : String("--");
  c.mm = colon > 0 ? data.currentTime.substring(colon + 1) : String("--");
  c.colon = (millis() / 1000) & 1;
  c.date = fmtDate(data.currentDate);
  c.price = fmtPrice(data.btcPrice);
  c.block = commas(data.blockHeight);
  c.hash = data.currentHashRate;
  lastClock = c;
  haveClock = true;
  paintClock(c);

  logShare(data.completedShares, data.totalKHashes, data.currentHashRate);
}

// --------------------------------------------------------- driver plumbing --

static void touchTask(void *);

// Boot / setup page on the panel, if any: it has to be drawn again when the
// display is flipped while it is showing.
enum StaticPage
{
  SP_NONE = 0,
  SP_LOADING,
  SP_SETUP
};
static int staticPage = SP_NONE;

void esp32_4in_Init(void)
{
  // Backlight off until the first frame is on the panel
  ledcSetup(0, 5000, 8); // LEDC channel 0, 5kHz, 8 bit (0-255 duty)
  ledcAttachPin(LCD_PIN_BL, 0);
  ledcWrite(0, 0);

  // The touch controller is on the panel's bus: deselect it before the panel
  // is set up
  pinMode(TOUCH_PIN_CS, OUTPUT);
  digitalWrite(TOUCH_PIN_CS, HIGH);
  pinMode(TOUCH_PIN_IRQ, INPUT);

  nvMem.loadConfig(&Settings);

  tft.init();
  tft.invertDisplay(Settings.invertColors);
  tft.setRotation(Settings.flipDisplay ? ROT_FLIPPED : ROT_NORMAL);
  tft.fillScreen(C_BG);
  ledcWrite(0, Settings.Brightness);

  xTaskCreatePinnedToCore(touchTask, "touch", 3072, NULL, 1, NULL, 0);

  loadUiPrefs();

  if (render.loadFont(NotoSans_Bold, sizeof(NotoSans_Bold)))
  {
    Serial.println("Initialise error");
    return;
  }

  pData.bestDifficulty = "0";
  pData.workersHash = "0";
  pData.workersCount = 0;

  resetWidgets();
  memset(segLvl, 0xFF, sizeof(segLvl));
}

void esp32_4in_AlternateScreenState(void)
{
  Serial.println("Switching display state");
  int screen_state_duty = ledcRead(0);
  // Switching the duty cycle for the ledc channel, where the backlight pin is attached.
  if (screen_state_duty > 0)
  {
    ledcWrite(0, 0);
  }
  else
  {
    ledcWrite(0, Settings.Brightness);
  }
}

// Save the orientation into the main config file, same field the LAN page
// uses. Load a fresh copy first so only this one value changes (the in-memory
// Settings can hold portal defaults that must not be saved).
static void persistFlip(bool flipped)
{
  Settings.flipDisplay = flipped;
  TSettings stored;
  if (nvMem.loadConfig(&stored))
  {
    stored.flipDisplay = flipped;
    nvMem.saveConfig(&stored);
  }
}

static void applyFlip()
{
  tft.setRotation(tft.getRotation() == ROT_FLIPPED ? ROT_NORMAL : ROT_FLIPPED);
  persistFlip(tft.getRotation() == ROT_FLIPPED);
  if (uiLive)
  {
    int s = currentDisplayDriver->current_cyclic_screen;
    uiScreen = -1;
    ensureChrome(s);
    repaintCached(s);
  }
  else if (staticPage == SP_LOADING)
    paintLoading(tft);
  else if (staticPage == SP_SETUP)
    paintSetup(tft);
}

// The button is polled from the Arduino loop task, drawing happens on the
// monitor task: hand the request over, except while the boot / setup pages
// are up (the monitor task is not drawing then).
static volatile bool pendingFlip = false;

void esp32_4in_AlternateRotation(void)
{
  if (uiLive)
    pendingFlip = true;
  else
    applyFlip();
}

void esp32_4in_LoadingScreen(void)
{
  uiLive = false;
  uiScreen = -1;
  staticPage = SP_LOADING;
  paintLoading(tft);
}

void esp32_4in_SetupScreen(void)
{
  uiLive = false;
  uiScreen = -1;
  staticPage = SP_SETUP;
  paintSetup(tft);
}

void esp32_4in_AnimateCurrentScreen(unsigned long frame)
{
  if (!uiLive || uiScreen < 0)
    return;
  drawPulse(frame);
  if (uiScreen == SCR_MINE && fabsf(gaugeTarget - gaugeShown) > 0.002f)
  {
    gaugeShown += (gaugeTarget - gaugeShown) * 0.3f; // ease toward the measured rate
    gaugeDraw(tft, gaugeShown);
  }
}

// Touch is sampled by its own small task at ~50Hz (the monitor loop is too
// slow and can block on network calls, which made taps get missed). It only
// records the gesture; all drawing stays on the monitor task.
enum TouchAction
{
  TA_NONE = 0,
  TA_NEXT,
  TA_PREV,
  TA_BACKLIGHT,
  TA_ACCENT
};
static volatile int pendingTouch = TA_NONE;

// Finger-sized corner zones at the top of the screen (the header itself is
// only HDR_H tall, too thin a target).
#define TOUCH_CORNER_W 160
#define TOUCH_CORNER_H 90

static void touchTask(void *)
{
  bool wasDown = false;
  int downCount = 0;
  for (;;)
  {
    int t_x, t_y;
    bool down = touchGetXY(t_x, t_y);
    if (down)
    {
      if (++downCount == 2 && !wasDown) // two consecutive samples: real press, act once per touch
      {
        wasDown = true;
        if (ledcRead(0) == 0)
          pendingTouch = TA_BACKLIGHT; // dark: any touch turns the backlight back on
        else if (t_y < TOUCH_CORNER_H && t_x > SCR_W - TOUCH_CORNER_W)
          pendingTouch = TA_BACKLIGHT; // top-right: backlight on/off
        else if (t_y < TOUCH_CORNER_H && t_x < TOUCH_CORNER_W)
          pendingTouch = TA_ACCENT; // wordmark: next accent colour
        else
          pendingTouch = t_x > SCR_W / 2 ? TA_NEXT : TA_PREV;
        Serial.printf("[touch] x=%d y=%d (raw %d,%d)\n", t_x, t_y, touchRawL, touchRawS);
      }
    }
    else
    {
      downCount = 0;
      wasDown = false;
    }
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

void esp32_4in_DoLedStuff(unsigned long frame)
{
  int action = pendingTouch;
  pendingTouch = TA_NONE;
  DisplayDriver *drv = currentDisplayDriver;
  switch (action)
  {
  case TA_BACKLIGHT:
    esp32_4in_AlternateScreenState();
    break;
  case TA_ACCENT:
    setAccent(accentIdx + 1);
    break;
  case TA_NEXT:
    drv->current_cyclic_screen = (drv->current_cyclic_screen + 1) % drv->num_cyclic_screens;
    break;
  case TA_PREV:
    drv->current_cyclic_screen = drv->current_cyclic_screen - 1;
    if (drv->current_cyclic_screen < 0)
      drv->current_cyclic_screen = drv->num_cyclic_screens - 1;
    break;
  }

  if (pendingFlip)
  {
    pendingFlip = false;
    applyFlip();
  }

  // Screen changed by touch or the boot button: draw the new page right away
  // from the last values it showed; the next data tick refreshes them.
  if (uiLive && currentDisplayDriver->current_cyclic_screen != uiScreen)
  {
    int s = currentDisplayDriver->current_cyclic_screen;
    ensureChrome(s);
    repaintCached(s);
  }
  if (uiLive)
    saveUiPrefs();
}

CyclicScreenFunction esp32_4inCyclicScreens[] ={esp32_4in_MinerScreen, esp32_4in_MarketScreen, esp32_4in_ClockScreen};

DisplayDriver esp32_4inDriver = {
    esp32_4in_Init,
    esp32_4in_AlternateScreenState,
    esp32_4in_AlternateRotation,
    esp32_4in_LoadingScreen,
    esp32_4in_SetupScreen,
    esp32_4inCyclicScreens,
    esp32_4in_AnimateCurrentScreen,
    esp32_4in_DoLedStuff,
    SCREENS_ARRAY_SIZE(esp32_4inCyclicScreens),
    0,
    SCR_W,
    SCR_H};
#endif
