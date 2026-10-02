// "PULSE" theme for the ESP32-2432S028R ("CYD", 320x240 landscape).
//
// A dark, glassy, gauge-driven look: no bitmaps at all, everything is drawn
// from primitives so the palette can be re-tinted at runtime (tap the
// NERDMINER wordmark, top-left, to cycle accent colours).
//
//   MINE    segmented hashrate ring, stat tiles, pool strip
//   MARKET  BTC price, block height, fees, difficulty, halving progress
//   CLOCK   big desk clock, key numbers, hashrate history bars
//
// The original theme is kept in esp23_2432s028r_classic.cpp (build the
// ESP32-2432S028R-Classic env, or define CYD_THEME_CLASSIC).
//
// Dev aid: -D CYD_UI_SCREENSHOT=1 renders every screen once at boot into an
// offscreen 8-bit sprite and dumps it over serial (see SHOT_BEGIN/SHOT_END).
#include "displayDriver.h"

#if (defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)) && !defined(CYD_THEME_CLASSIC)

#include <TFT_eSPI.h>
#include <TFT_eTouch.h>
#include <WiFi.h>
#include <Preferences.h>
#include "media/myFonts.h"
#include "version.h"
#include "monitor.h"
#include "OpenFontRender.h"
#include <SPI.h>
#include "rotation.h"
#include "drivers/storage/nvMemory.h"
#include "drivers/storage/storage.h"
#include "currency.h"

#define WIDTH 130
#define HEIGHT 170

extern nvMemory nvMem;
extern monitor_data mMonitor;
extern pool_data pData;
extern DisplayDriver *currentDisplayDriver;
extern bool invertColors;
extern TSettings Settings;
extern unsigned long mPoolUpdate;
extern String readCustomAPName(); // wManager.cpp

OpenFontRender render;
TFT_eSPI tft = TFT_eSPI(); // pins defined in platformio.ini
SPIClass hSPI(HSPI);
TFT_eTouch<TFT_eSPI> touch(tft, ETOUCH_CS, 0xFF, hSPI);

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
static const uint16_t C_BAD = RGB565(255, 92, 112);
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

#define SCR_W 320
#define SCR_H 240
#define HDR_H 26 // header band; hairline sits on the row below

#define SCR_MINE 0
#define SCR_MARKET 1
#define SCR_CLOCK 2

// Hashrate ring
#define G_CX 92
#define G_CY 110
#define G_R 78
#define G_RI 67
#define G_SEGS 30
#define G_MAX_KH 800.0f // full-scale of the ring (classic ESP32 tops out ~670)

// Bottom strip (pool cells / hashrate history)
#define STRIP_Y 198
#define STRIP_H 38

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
// Everything draws either to the panel or (dev builds) to an offscreen shot.

static TFT_eSprite wgt = TFT_eSprite(&tft); // scratch sprite reused by every widget

#ifdef CYD_UI_SCREENSHOT
static TFT_eSprite shot = TFT_eSprite(&tft);
static bool shotOn = false;
static TFT_eSPI &out() { return shotOn ? (TFT_eSPI &)shot : tft; }
#else
static TFT_eSPI &out() { return tft; }
#endif

static bool wBegin(int w, int h, uint16_t bg)
{
  wgt.setColorDepth(16);
  wgt.createSprite(w, h);
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
#ifdef CYD_UI_SCREENSHOT
  if (shotOn)
  {
    wgt.pushToSprite(&shot, x, y);
    wgt.deleteSprite();
    return;
  }
#endif
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
  int x0 = cx - (wv + 4 + wu) / 2;
  text(d, val, x0, y, size, cv, bg);
  text(d, unit, x0 + wv + 4, y + (size - usz), usz, cu, bg);
}

// rounded card: 1px edge, AA corners
static void panel(TFT_eSPI &d, int x, int y, int w, int h, int r, uint16_t fill, uint16_t edge, uint16_t behind)
{
  d.fillSmoothRoundRect(x, y, w, h, r, edge, behind);
  d.fillSmoothRoundRect(x + 1, y + 1, w - 2, h - 2, r - 1, fill, edge);
}

// horizontal gradient bar (alt -> accent) with softly rounded ends
static void gradBar(TFT_eSPI &d, int x, int y, int w, int h, uint16_t bg)
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
  (void)bg;
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
  const int X0 = 24, W = SCR_W - X0;
  if (!wBegin(W, HDR_H, C_BG))
    return;

  // wordmark
  int wn = textW("NERD", 14);
  text(wgt, "NERD", 2, 5, 14, C_TEXT, C_BG);
  text(wgt, "MINER", 2 + wn + 1, 5, 14, cAcc, C_BG);
  int wm = 2 + wn + 1 + textW("MINER", 14);

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
  int cw = textW(label, 9) + 14;
  int cx = wm + 10;
  wgt.fillSmoothRoundRect(cx, 6, cw, 15, 7, mix565(C_BG, col, 46), C_BG);
  text(wgt, label, cx + cw / 2, 8, 9, col, mix565(C_BG, col, 46), Align::TopCenter);

  // time (right)
  text(wgt, h.time.c_str(), W - 8, 5, 14, C_TEXT, C_BG, Align::TopRight);

  // wifi bars
  for (int i = 0; i < 4; i++)
  {
    int bh = 4 + i * 3;
    wgt.fillRect(220 + i * 5, 20 - bh, 3, bh, i < h.bars ? cAcc : C_TRACK);
  }

  // local IP, right-aligned before the wifi bars; shrinks to fit beside the state chip
  if (h.ip.length())
  {
    const int right = 212, room = right - (cx + cw) - 8;
    unsigned sz = 12;
    while (sz > 8 && textW(h.ip.c_str(), sz) > room)
      sz--;
    text(wgt, h.ip.c_str(), right, 14 - (int)sz / 2 - 1, sz, C_DIM, C_BG, Align::TopRight);
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
  tft.fillSmoothCircle(13, 13, 4, mix565(C_TRACK, base, 70 + lvl * 18), C_BG);
}

// -------------------------------------------------------------- pool strip --

struct StripV
{
  String l[4];
  String v[4];
};

static void chromeStrip(TFT_eSPI &d)
{
  panel(d, 8, STRIP_Y, 304, STRIP_H, 8, C_PANEL, C_EDGE, C_BG);
}

static void drawStrip(const StripV &s)
{
  for (int i = 0; i < 4; i++)
  {
    String k = s.l[i] + "|" + s.v[i] + "|" + accentIdx;
    if (!need(W_P0 + i, k))
      continue;
    if (!wBegin(70, 32, C_PANEL))
      continue;
    text(wgt, s.l[i].c_str(), 6, 3, 9, C_DIM, C_PANEL);
    text(wgt, s.v[i].c_str(), 6, 15, 14, C_TEXT, C_PANEL);
    wEnd(14 + 74 * i, STRIP_Y + 3);
    done(W_P0 + i, k);
  }
}

static void chromeStripCells(TFT_eSPI &d)
{
  chromeStrip(d);
  for (int i = 1; i < 4; i++)
    d.drawFastVLine(12 + 74 * i, STRIP_Y + 8, STRIP_H - 16, C_EDGE);
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
  panel(wgt, 0, 0, w, h, 8, C_PANEL, edge, C_BG);
  int top = (h - 28) / 2;
  wgt.fillSmoothRoundRect(8, 8, 3, h - 16, 1, stripe, C_PANEL);
  text(wgt, label, 18, top, 9, C_DIM, C_PANEL);
  text(wgt, value, 18, top + 12, strlen(value) > 8 ? 12 : 15, valCol, C_PANEL);
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

static const int TILE_Y[4] = {32, 72, 112, 152};

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
    if (wBegin(104, 74, C_BG))
    {
      text(wgt, "HASHRATE", 52, 4, 9, C_DIM, C_BG, Align::TopCenter);
      unsigned sz = m.hash.length() <= 5 ? 36 : (m.hash.length() == 6 ? 30 : 26);
      text(wgt, m.hash.c_str(), 52, 15, sz, C_TEXT, C_BG, Align::TopCenter);
      text(wgt, "KH/s", 52, 56, 13, cAcc, C_BG, Align::TopCenter);
      wEnd(G_CX - 52, G_CY - 38);
      done(W_HERO, k);
    }
  }

  // lifetime hashes, in the gap at the bottom of the ring
  k = m.total + "|" + accentIdx;
  if (need(W_TOTAL, k))
  {
    if (wBegin(120, 16, C_BG))
    {
      textPair(wgt, 60, 1, commas(m.total).c_str(), "MH TOTAL", 11, C_TEXT, C_DIM, C_BG);
      wEnd(G_CX - 60, 171);
      done(W_TOTAL, k);
    }
  }

  const int tx = 184, tw = 128, th = 36;
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
  panel(d, 8, 32, 304, 70, 10, C_PANEL, C_EDGE, C_BG);
  text(d, btcPairLabel().c_str(), 20, 39, 9, C_DIM, C_PANEL);
  text(d, "LATEST BLOCK", 300, 39, 9, C_DIM, C_PANEL, Align::TopRight);
  panel(d, 8, 156, 304, 36, 8, C_PANEL, C_EDGE, C_BG);
  chromeStripCells(d);
}

static void paintMarket(const MarketV &m)
{
  drawHeader(m.hdr);

  String k = m.price + "|" + accentIdx;
  if (need(W_PRICE, k) && wBegin(176, 46, C_PANEL))
  {
    const unsigned len = m.price.length();
    text(wgt, m.price.c_str(), 2, 0, len > 8 ? 38 * 8 / len : 38, C_TEXT, C_PANEL);
    wEnd(18, 52);
    done(W_PRICE, k);
  }

  k = m.block + "|" + accentIdx;
  if (need(W_BLOCK, k) && wBegin(110, 46, C_PANEL))
  {
    text(wgt, m.block.c_str(), 106, 17, 22, cAcc, C_PANEL, Align::TopRight);
    wEnd(190, 52);
    done(W_BLOCK, k);
  }

  const int y = 108, w = 98, h = 42;
  drawTile(W_M0, 8, y, w, h, "FEE / HALF HOUR", m.fee.c_str(), cAcc, C_TEXT, C_EDGE);
  drawTile(W_M1, 111, y, w, h, "DIFFICULTY", m.diff.c_str(), cAlt, C_TEXT, C_EDGE);
  drawTile(W_M2, 214, y, w, h, "NETWORK RATE", m.net.c_str(), C_DIM, C_TEXT, C_EDGE);

  char kb[48];
  snprintf(kb, sizeof(kb), "%d|%s|%d", m.pct, m.remaining.c_str(), accentIdx);
  if (need(W_HALV, kb) && wBegin(288, 30, C_PANEL))
  {
    text(wgt, "NEXT HALVING", 4, 1, 9, C_DIM, C_PANEL);
    char pc[8];
    snprintf(pc, sizeof(pc), "%d%%", m.pct);
    text(wgt, pc, 4 + textW("NEXT HALVING", 9) + 8, 0, 11, cAcc, C_PANEL);
    String rem = commas(m.remaining) + " blocks left";
    text(wgt, rem.c_str(), 284, 1, 10, C_TEXT, C_PANEL, Align::TopRight);
    wgt.fillSmoothRoundRect(4, 17, 280, 8, 4, C_TRACK, C_PANEL);
    int fw = (280 * m.pct) / 100;
    if (fw < 8 && m.pct > 0)
      fw = 8;
    if (fw > 0)
      gradBar(wgt, 4, 17, fw, 8, C_TRACK);
    wEnd(16, 159);
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
  const int W = 288, H = 30;
  if (!wBegin(W, H, C_PANEL))
    return;
  float mx = 1;
  for (int i = 0; i < histCount; i++)
    if (hist[i] > mx)
      mx = hist[i];
  mx *= 1.08f;
  const int step = 6;
  for (int i = 0; i < HIST_N; i++)
  {
    int slot = i - (HIST_N - histCount); // right-align so the newest bar is on the right
    int x = i * step;
    if (slot < 0)
    {
      wgt.fillRect(x, H - 2, 5, 2, C_TRACK); // not sampled yet
      continue;
    }
    int bh = (int)(H * hist[slot] / mx);
    if (bh < 2)
      bh = 2;
    uint16_t c = mix565(cAlt, cAcc, (uint8_t)(i * 255 / (HIST_N - 1)));
    wgt.fillRect(x, H - bh, 5, bh, c);
  }
  wEnd(16, STRIP_Y + 4);
  done(W_CHART, k);
}

static void paintClock(const ClockV &c)
{
  drawHeader(c.hdr);

  // Glyph boxes start well above the digits (ascender space), so draw the
  // text 31px above the widget: digits then span local y 2..67.
  String k = c.hh + "|" + accentIdx;
  if (need(W_HH, k) && wBegin(112, 70, C_BG))
  {
    text(wgt, c.hh.c_str(), 110, -31, 92, C_TEXT, C_BG, Align::TopRight);
    wEnd(34, 32);
    done(W_HH, k);
  }
  k = c.mm + "|" + accentIdx;
  if (need(W_MM, k) && wBegin(112, 70, C_BG))
  {
    text(wgt, c.mm.c_str(), 2, -31, 92, cAcc, C_BG, Align::TopLeft);
    wEnd(174, 32);
    done(W_MM, k);
  }
  k = String(c.colon) + "|" + accentIdx;
  if (need(W_COLON, k) && wBegin(28, 70, C_BG))
  {
    uint16_t dot = c.colon ? C_DIM : C_BG;
    wgt.fillSmoothCircle(14, 22, 5, dot, C_BG);
    wgt.fillSmoothCircle(14, 47, 5, dot, C_BG);
    wEnd(146, 32);
    done(W_COLON, k);
  }

  k = c.date;
  if (need(W_DATE, k) && wBegin(220, 18, C_BG))
  {
    text(wgt, c.date.c_str(), 110, 1, 12, C_DIM, C_BG, Align::TopCenter);
    wEnd(50, 108);
    done(W_DATE, k);
  }

  const int y = 140, w = 98, h = 48;
  drawTile(W_C0, 8, y, w, h, btcPairLabel().c_str(), c.price.c_str(), cAcc, C_TEXT, C_EDGE);
  drawTile(W_C1, 111, y, w, h, "BLOCK", c.block.c_str(), cAlt, C_TEXT, C_EDGE);
  drawTile(W_C2, 214, y, w, h, "HASHRATE KH/s", c.hash.c_str(), C_GOOD, C_TEXT, C_EDGE);

  drawChart();
}

// ------------------------------------------------------ boot / setup pages --

static void paintLoading(TFT_eSPI &d)
{
  clearScreen(d);
  // full-circle segmented ring, gradient around it
  const int cx = 160, cy = 92, r = 50, ri = 41, n = 24;
  for (int i = 0; i < n; i++)
  {
    uint16_t c = mix565(cAlt, cAcc, (uint8_t)(i * 255 / (n - 1)));
    int a0 = i * 15;
    d.drawArc(cx, cy, r, ri, a0, a0 + 11, c, C_BG, true);
  }
  text(d, "#", cx, cy - 27, 54, C_TEXT, C_BG, Align::TopCenter);
  int wn = textW("NERD", 30), wm = textW("MINER", 30);
  int x0 = cx - (wn + wm + 2) / 2;
  text(d, "NERD", x0, 152, 30, C_TEXT, C_BG);
  text(d, "MINER", x0 + wn + 2, 152, 30, cAcc, C_BG);
  text(d, CURRENT_VERSION, cx, 196, 12, C_DIM, C_BG, Align::TopCenter);
  text(d, "STARTING UP", cx, 216, 9, C_DIM, C_BG, Align::TopCenter);
}

static void paintSetup(TFT_eSPI &d)
{
  clearScreen(d);
  text(d, "SETUP", 16, 10, 24, C_TEXT, C_BG);
  text(d, "MODE", 16 + textW("SETUP", 24) + 7, 10, 24, cAcc, C_BG);
  text(d, "Join the miner's WiFi to configure it", 16, 42, 11, C_DIM, C_BG);

  String ap = readCustomAPName();
  if (ap.length() == 0)
    ap = DEFAULT_SSID;
  const char *lab[3] = {"JOIN THIS WIFI NETWORK", "PASSWORD", "THEN OPEN IN A BROWSER"};
  const char *val[3] = {ap.c_str(), DEFAULT_WIFIPW, "192.168.4.1"};
  for (int i = 0; i < 3; i++)
  {
    int y = 68 + i * 50;
    panel(d, 8, y, 304, 44, 10, C_PANEL, C_EDGE, C_BG);
    d.fillSmoothCircle(32, y + 22, 13, mix565(C_PANEL, cAcc, 60), C_PANEL);
    char n[2] = {(char)('1' + i), 0};
    text(d, n, 32, y + 12, 17, cAcc, mix565(C_PANEL, cAcc, 60), Align::TopCenter);
    text(d, lab[i], 58, y + 6, 9, C_DIM, C_PANEL);
    text(d, val[i], 58, y + 19, 17, C_TEXT, C_PANEL);
  }
  text(d, "Save the form and the miner restarts by itself", 160, 222, 10, C_DIM, C_BG, Align::TopCenter);
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
  paintChrome(out(), scr);
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

#define UI_PREFS_NS "cyd_ui"

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

void esp32_2432S028R_MinerScreen(unsigned long mElapsed)
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

void esp32_2432S028R_MarketScreen(unsigned long mElapsed)
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

void esp32_2432S028R_ClockScreen(unsigned long mElapsed)
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
void esp32_2432S028R_AlternateRotation(void);

void esp32_2432S028R_Init(void)
{
  tft.init();
  if (nvMem.loadConfig(&Settings))
  {
    invertColors = Settings.invertColors;
  }
  tft.invertDisplay(invertColors);
  tft.setRotation(Settings.flipDisplay ? 3 : 1); // 1 = USB on the right, 3 = USB on the left (touch follows automatically)
  tft.setSwapBytes(true);
  if (invertColors)
  {
    tft.writecommand(ILI9341_GAMMASET);
    tft.writedata(2);
    delay(120);
    tft.writecommand(ILI9341_GAMMASET); // Gamma curve selected
    tft.writedata(1);
  }
  hSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, ETOUCH_CS);
  touch.init();

  TFT_eTouchBase::Calibation calibation = {233, 3785, 3731, 120, 2};
  touch.setCalibration(calibation);
  xTaskCreatePinnedToCore(touchTask, "cydTouch", 3072, NULL, 1, NULL, 0);

  // Backlight: LEDC channel 0, 5kHz, 8 bit (0-255 duty)
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, Settings.Brightness);

  loadUiPrefs();

  if (render.loadFont(NotoSans_Bold, sizeof(NotoSans_Bold)))
  {
    Serial.println("Initialise error");
    return;
  }

  pinMode(LED_PIN, OUTPUT);
  pinMode(LED_PIN_B, OUTPUT);
  pinMode(LED_PIN_G, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(LED_PIN_B, HIGH);
  digitalWrite(LED_PIN_G, HIGH);
  pData.bestDifficulty = "0";
  pData.workersHash = "0";
  pData.workersCount = 0;

  resetWidgets();
  memset(segLvl, 0xFF, sizeof(segLvl));

#ifdef CYD_UI_SCREENSHOT
  extern void cydDumpScreens(void);
  cydDumpScreens();
#endif
}

void esp32_2432S028R_AlternateScreenState(void)
{
  Serial.println("Switching display state");
  int screen_state_duty = ledcRead(0);
  // Switching the duty cycle for the ledc channel, where the TFT_BL pin is attached.
  if (screen_state_duty > 0)
  {
    ledcWrite(0, 0);
  }
  else
  {
    ledcWrite(0, Settings.Brightness);
  }
}

// Save the orientation into the main config file, same field the portal and
// LAN page use. Load a fresh copy first so only this one value changes
// (the in-memory Settings can hold portal defaults that must not be saved).
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

void esp32_2432S028R_AlternateRotation(void)
{
  tft.setRotation(flipRotation(tft.getRotation()));
  persistFlip(tft.getRotation() == 3); // rotation 1 = USB right, 3 = USB left
  if (uiLive)
  {
    int s = currentDisplayDriver->current_cyclic_screen;
    uiScreen = -1;
    ensureChrome(s);
    repaintCached(s);
  }
}

void esp32_2432S028R_LoadingScreen(void)
{
  uiLive = false;
  uiScreen = -1;
  paintLoading(out());
}

void esp32_2432S028R_SetupScreen(void)
{
  uiLive = false;
  uiScreen = -1;
  paintSetup(out());
}

void esp32_2432S028R_AnimateCurrentScreen(unsigned long frame)
{
  if (!uiLive || uiScreen < 0)
    return;
  drawPulse(frame);
  if (uiScreen == SCR_MINE && fabsf(gaugeTarget - gaugeShown) > 0.002f)
  {
    gaugeShown += (gaugeTarget - gaugeShown) * 0.3f; // ease toward the measured rate
    gaugeDraw(out(), gaugeShown);
  }
}

unsigned long previousMillis = 0;

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

static void touchTask(void *)
{
  bool wasDown = false;
  int downCount = 0;
  for (;;)
  {
    int16_t t_x, t_y;
    bool down = touch.getXY(t_x, t_y);
    if (down)
    {
      if (++downCount == 2 && !wasDown) // two consecutive samples: real press, act once per touch
      {
        wasDown = true;
        if (t_y < HDR_H + 4 && t_x > 235)
          pendingTouch = TA_BACKLIGHT; // top-right: backlight on/off
        else if (t_y < HDR_H + 4 && t_x < 110)
          pendingTouch = TA_ACCENT; // wordmark: next accent colour
        else
          pendingTouch = t_x > 160 ? TA_NEXT : TA_PREV;
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

void esp32_2432S028R_DoLedStuff(unsigned long frame)
{
  unsigned long currentMillis = millis();
  int action = pendingTouch;
  pendingTouch = TA_NONE;
  DisplayDriver *drv = currentDisplayDriver;
  switch (action)
  {
  case TA_BACKLIGHT:
    esp32_2432S028R_AlternateScreenState();
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

  switch (mMonitor.NerdStatus)
  {
  case NM_waitingConfig:
    digitalWrite(LED_PIN, LOW); // steady
    break;

  case NM_Connecting:
    if (currentMillis - previousMillis >= 500)
    {
      previousMillis = currentMillis;
      digitalWrite(LED_PIN, HIGH);
      digitalWrite(LED_PIN_B, !digitalRead(LED_PIN));
    }
    break;

  case NM_hashing:
    if (currentMillis - previousMillis >= 500)
    {
      previousMillis = currentMillis;
      digitalWrite(LED_PIN_B, HIGH);
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }
    break;
  }
}

// ----------------------------------------------------- dev: screenshot dump --

#ifdef CYD_UI_SCREENSHOT
static void dumpShot(const char *name)
{
  Serial.printf("\nSHOT_BEGIN %s %d %d\n", name, SCR_W, SCR_H);
  Serial.write((const uint8_t *)shot.getPointer(), SCR_W * SCR_H);
  Serial.printf("\nSHOT_END\n");
  Serial.flush();
}

void cydDumpScreens(void)
{
  shot.setColorDepth(8);
  shot.createSprite(SCR_W, SCR_H);
  if (!shot.created())
  {
    Serial.println("[ui] shot sprite alloc failed");
    return;
  }
  shotOn = true;

  HdrV h;
  h.time = "21:47";
  h.ip = "192.168.100.100";
  h.bars = 3;
  h.status = NM_hashing;

  for (int i = 0; i < HIST_N; i++)
    hist[i] = 610 + 60 * sinf(i * 0.45f) + (i * 7 % 23);
  histCount = HIST_N;
  histEpoch = 1;

  for (int acc = 0; acc < 2; acc++)
  {
    // accent 0 shows every screen; accent 2 (amber/rose) shows the mining screen again
    setAccent(acc == 0 ? 0 : 2);

    // loading + setup
    paintLoading(shot);
    dumpShot(acc == 0 ? "loading" : "loading_alt");
    if (acc == 0)
    {
      paintSetup(shot);
      dumpShot("setup");
    }

    // mine
    uiLive = true;
    gaugeShown = gaugeTarget = 0.84f;
    uiScreen = -1;
    ensureChrome(SCR_MINE);
    MineV m;
    m.hdr = h;
    m.hash = "672.4";
    m.shares = "1,284";
    m.best = "8.73M";
    m.tmpl = "412";
    m.blocks = "0";
    m.total = "58213";
    m.kh = 672.4f;
    m.flash = acc == 1;
    m.strip.l[0] = "WORKERS";
    m.strip.v[0] = "3";
    m.strip.l[1] = "POOL RATE";
    m.strip.v[1] = "1.92M";
    m.strip.l[2] = "POOL BEST";
    m.strip.v[2] = "412.6M";
    m.strip.l[3] = "UPTIME";
    m.strip.v[3] = "3d 07h";
    paintMine(m);
    dumpShot(acc == 0 ? "mine" : "mine_alt");
    if (acc == 1)
      continue;

    // market
    uiScreen = -1;
    ensureChrome(SCR_MARKET);
    MarketV k;
    k.hdr = h;
    k.price = "$97,412";
    k.block = "912,345";
    k.fee = "12 sat/vB";
    k.diff = "146.72T";
    k.net = "912 EH/s";
    k.pct = 47;
    k.remaining = "111,655";
    k.strip = m.strip;
    k.strip.l[3] = "SHARES";
    k.strip.v[3] = "1,284";
    paintMarket(k);
    dumpShot("market");

    // clock
    uiScreen = -1;
    ensureChrome(SCR_CLOCK);
    ClockV c;
    c.hdr = h;
    c.hh = "21";
    c.mm = "47";
    c.colon = true;
    c.date = fmtDate("30/09/2026");
    c.price = "$97,412";
    c.block = "912,345";
    c.hash = "672.4";
    paintClock(c);
    dumpShot("clock");
  }

  shotOn = false;
  shot.deleteSprite();
  uiLive = false;
  uiScreen = -1;
  resetWidgets();
}
#endif

CyclicScreenFunction esp32_2432S028RCyclicScreens[] = {esp32_2432S028R_MinerScreen, esp32_2432S028R_MarketScreen, esp32_2432S028R_ClockScreen};

DisplayDriver esp32_2432S028RDriver = {
    esp32_2432S028R_Init,
    esp32_2432S028R_AlternateScreenState,
    esp32_2432S028R_AlternateRotation,
    esp32_2432S028R_LoadingScreen,
    esp32_2432S028R_SetupScreen,
    esp32_2432S028RCyclicScreens,
    esp32_2432S028R_AnimateCurrentScreen,
    esp32_2432S028R_DoLedStuff,
    SCREENS_ARRAY_SIZE(esp32_2432S028RCyclicScreens),
    0,
    WIDTH,
    HEIGHT};
#endif
