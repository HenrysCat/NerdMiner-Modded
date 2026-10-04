// "PULSE" theme for the LCDWiki ES3C35P (ESP32-S3, 3.5" 480x320 landscape).
//
// Same look and screens as the CYD theme in esp23_2432s028r.cpp, laid out for
// the larger panel:
//
//   MINE    segmented hashrate ring, stat tiles, pool strip
//   MARKET  BTC price, block height, fees, difficulty, halving progress
//   CLOCK   big desk clock, key numbers, hashrate history bars
//
// The panel is an ST77922 on quad SPI, which TFT_eSPI cannot drive. TFT_eSPI
// is used here only as a drawing engine: everything is drawn into a 480x320
// sprite in PSRAM (the frame buffer) and the changed rectangle is then sent
// to the panel by the small quad SPI driver below. tft.init() is never called.
//
// Dev aid: -D ES3C35P_UI_SCREENSHOT=1 renders every screen once at boot and
// dumps the frame buffer over serial (see SHOT_BEGIN/SHOT_END).
#include "displayDriver.h"

#ifdef ES3C35P_DISPLAY

#include <TFT_eSPI.h>
#include <WiFi.h>
#include <Wire.h>
#include <Preferences.h>
#include <driver/spi_master.h>
#include <driver/periph_ctrl.h>
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
static TFT_eSPI tft = TFT_eSPI();          // drawing engine only, never initialised
static TFT_eSprite fb = TFT_eSprite(&tft); // the frame buffer
static TFT_eSprite wgt = TFT_eSprite(&tft); // scratch sprite reused by every widget

#define SCR_W 480
#define SCR_H 320

// ================================================================== panel ==

#define LCD_NATIVE_W 320 // the panel is portrait; landscape is done in panelFlush
#define LCD_NATIVE_H 480
#define LCD_SPI_HZ 40000000 // the controller's write clock limit is 62.5 MHz
#define LCD_TX_BYTES 30720  // 48 full native rows per transfer

// Init sequence of this panel, as {command, parameter count, parameters...}.
// Decoded from the board's factory firmware; LCDWiki publish the same table
// in their Arduino examples.
static const uint8_t LCD_INIT[] = {
    0xF1, 1, 0x00,
    0x60, 3, 0x00, 0x00, 0x00,
    0x65, 1, 0x80,
    0x79, 1, 0x06,
    0x7B, 3, 0x00, 0x08, 0x08,
    0x80, 11, 0x55, 0x62, 0x2F, 0x17, 0xF0, 0x52, 0x70, 0xD2, 0x52, 0x62, 0xEA,
    0x81, 4, 0x26, 0x52, 0x72, 0x27,
    0x84, 2, 0x92, 0x25,
    0x87, 6, 0x10, 0x10, 0x58, 0x00, 0x02, 0x3A,
    0x88, 15, 0x00, 0x00, 0x2C, 0x10, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x06,
    0x89, 3, 0x00, 0x00, 0x00,
    0x8A, 11, 0x13, 0x00, 0x2C, 0x00, 0x00, 0x2C, 0x10, 0x10, 0x00, 0x3E, 0x19,
    0x8B, 9, 0x15, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x97, 0x8E,
    0x8C, 13, 0x1D, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x50, 0x0F, 0x01, 0xC5, 0x12, 0x09,
    0x8D, 1, 0x0C,
    0x8E, 6, 0x33, 0x01, 0x0C, 0x13, 0x01, 0x01,
    0xB3, 2, 0x00, 0x30,
    0xF1, 1, 0x00,
    0x71, 1, 0xD0,
    0x66, 2, 0x02, 0x3F,
    0xBE, 3, 0x26, 0x00, 0x9D,
    0x70, 12, 0x01, 0xA6, 0x11, 0x40, 0xE0, 0x00, 0x11, 0x60, 0x11, 0x00, 0x00, 0x1A,
    0x90, 9, 0x04, 0x04, 0x55, 0x74, 0x00, 0x40, 0x43, 0x2D, 0x2D,
    0x91, 9, 0x04, 0x04, 0x55, 0x75, 0x00, 0x40, 0x42, 0x2D, 0x2D,
    0x92, 10, 0x04, 0x44, 0x55, 0xC0, 0x06, 0x00, 0x07, 0x05, 0x90, 0x2D,
    0x93, 10, 0x04, 0x43, 0x11, 0x00, 0x00, 0x00, 0x00, 0x05, 0x90, 0x2D,
    0x94, 6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x95, 5, 0x96, 0x16, 0x00, 0x00, 0xFF,
    0x96, 12, 0x44, 0x53, 0x03, 0x12, 0x23, 0x24, 0x06, 0x05, 0x9A, 0x2D, 0x00, 0x44,
    0x97, 12, 0x44, 0x53, 0x47, 0x56, 0x20, 0x20, 0x02, 0x01, 0x9A, 0x2D, 0x00, 0x44,
    0xBA, 5, 0x55, 0x9A, 0x2D, 0x9A, 0x2D,
    0x9A, 7, 0x40, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
    0x9B, 7, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
    0x9C, 13, 0x5C, 0x12, 0x00, 0x00, 0x10, 0x12, 0x00, 0x00, 0x10, 0x02, 0x00, 0x00, 0x00,
    0x9D, 8, 0x8A, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01,
    0x9E, 7, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01,
    0xB4, 12, 0x1D, 0x1C, 0x1E, 0x0B, 0x14, 0x02, 0x13, 0x09, 0x1E, 0x00, 0x1E, 0x10,
    0xB5, 12, 0x1D, 0x1C, 0x1E, 0x0A, 0x15, 0x03, 0x11, 0x08, 0x1E, 0x01, 0x1E, 0x12,
    0xB6, 7, 0x77, 0x77, 0x00, 0x0A, 0xFF, 0x0A, 0xFF,
    0x86, 14, 0xC6, 0x04, 0xB1, 0x02, 0x58, 0x12, 0x58, 0x0C, 0x13, 0x01, 0xA5, 0x00, 0xA5, 0xA5,
    0xB7, 16, 0x07, 0x0A, 0x0E, 0x06, 0x05, 0x03, 0x2B, 0x03, 0x03, 0x42, 0x07, 0x10, 0x10, 0x2E, 0x3F, 0x0D,
    0xB8, 16, 0x07, 0x0A, 0x0D, 0x05, 0x05, 0x02, 0x2B, 0x02, 0x03, 0x42, 0x06, 0x10, 0x0F, 0x2E, 0x3F, 0x0D,
    0xB9, 2, 0x23, 0x23,
    0xBF, 6, 0x10, 0x14, 0x14, 0x0B, 0x0B, 0x0B,
    0xF2, 1, 0x00,
    0x73, 5, 0x04, 0xDA, 0x12, 0x54, 0x47,
    0x77, 5, 0x6B, 0x5B, 0xFD, 0xC3, 0xC5,
    0x7A, 2, 0x15, 0x27,
    0x7B, 2, 0x04, 0x57,
    0x7E, 2, 0x01, 0x0E,
    0xBF, 1, 0x36,
    0xE3, 2, 0x40, 0x40,
    0xF0, 1, 0x00,
    0xD0, 1, 0x00,
    0x2A, 4, 0x00, 0x00, 0x01, 0x3F,
    0x2B, 4, 0x00, 0x00, 0x01, 0xDF,
    0x21, 0,       // inversion on (this panel needs it for true colours)
    0x11, 0,       // sleep out, then wait 120 ms
    0x29, 0,       // display on
    0x2C, 0,
    0x3A, 1, 0x01, // RGB565
    0x36, 1, 0x00, // native portrait, RGB order
    0x35, 1, 0x01, // tearing effect line on, then wait 20 ms
};

static spi_device_handle_t lcdSpi = NULL;
static uint8_t *lcdTx = NULL; // DMA-capable transfer buffer
static bool lcdFlipped = false;

// Register write: opcode 0x02, the command in a 24-bit address, single line.
static void lcdCmd(uint8_t cmd, const uint8_t *data, size_t len)
{
  spi_transaction_t t = {};
  t.cmd = 0x02;
  t.addr = (uint32_t)cmd << 8;
  if (len)
  {
    memcpy(lcdTx, data, len);
    t.tx_buffer = lcdTx;
    t.length = len * 8;
  }
  spi_device_polling_transmit(lcdSpi, &t);
}

static bool panelInit()
{
  spi_bus_config_t bus = {};
  bus.mosi_io_num = LCD_PIN_D0;
  bus.miso_io_num = LCD_PIN_D1;
  bus.quadwp_io_num = LCD_PIN_D2;
  bus.quadhd_io_num = LCD_PIN_D3;
  bus.sclk_io_num = LCD_PIN_SCK;
  bus.max_transfer_sz = LCD_TX_BYTES;
  bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK)
    return false;

  spi_device_interface_config_t dev = {};
  dev.command_bits = 8;
  dev.address_bits = 24;
  dev.mode = 0;
  dev.clock_speed_hz = LCD_SPI_HZ;
  dev.spics_io_num = LCD_PIN_CS;
  dev.flags = SPI_DEVICE_HALFDUPLEX;
  dev.queue_size = 1;
  if (spi_bus_add_device(SPI2_HOST, &dev, &lcdSpi) != ESP_OK)
    return false;

  lcdTx = (uint8_t *)heap_caps_malloc(LCD_TX_BYTES, MALLOC_CAP_DMA);
  if (!lcdTx)
    return false;

  for (size_t i = 0; i < sizeof(LCD_INIT);)
  {
    uint8_t cmd = LCD_INIT[i], len = LCD_INIT[i + 1];
    lcdCmd(cmd, &LCD_INIT[i + 2], len);
    if (cmd == 0x11)
      delay(120);
    else if (cmd == 0x35)
      delay(20);
    i += 2 + len;
  }
  // The table leaves the panel in its normal state (inversion on). The
  // "Invert display colors" setting turns it off, i.e. shows the negative.
  if (Settings.invertColors)
    lcdCmd(0x20, NULL, 0);
  return true;
}

// Send a rectangle of the (landscape) frame buffer to the (portrait) panel.
// Not flipped: the panel's long edge runs right-to-left across the frame
// buffer; flipped is the same turned by 180 degrees.
static void panelFlush(int x, int y, int w, int h)
{
  if (!lcdSpi || !fb.created())
    return;
  if (x < 0)
  {
    w += x;
    x = 0;
  }
  if (y < 0)
  {
    h += y;
    y = 0;
  }
  if (x + w > SCR_W)
    w = SCR_W - x;
  if (y + h > SCR_H)
    h = SCR_H - y;
  if (w <= 0 || h <= 0)
    return;

  int px0, px1, py0, py1; // native rectangle, end exclusive
  if (!lcdFlipped)
  {
    px0 = y;
    px1 = y + h;
    py0 = LCD_NATIVE_H - (x + w);
    py1 = LCD_NATIVE_H - x;
  }
  else
  {
    px0 = LCD_NATIVE_W - (y + h);
    px1 = LCD_NATIVE_W - y;
    py0 = x;
    py1 = x + w;
  }
  // The controller drives its gates in pairs: windows must be 4-pixel aligned.
  px0 &= ~3;
  py0 &= ~3;
  px1 = (px1 + 3) & ~3;
  py1 = (py1 + 3) & ~3;
  const int nw = px1 - px0, nh = py1 - py0;

  spi_device_acquire_bus(lcdSpi, portMAX_DELAY);
  const uint8_t ca[4] = {(uint8_t)(px0 >> 8), (uint8_t)px0, (uint8_t)((px1 - 1) >> 8), (uint8_t)(px1 - 1)};
  const uint8_t ra[4] = {(uint8_t)(py0 >> 8), (uint8_t)py0, (uint8_t)((py1 - 1) >> 8), (uint8_t)(py1 - 1)};
  lcdCmd(0x2A, ca, 4);
  lcdCmd(0x2B, ra, 4);

  const uint16_t *src = (const uint16_t *)fb.getPointer(); // already in wire byte order
  uint16_t *dst = (uint16_t *)lcdTx;
  const int rowsPerTx = (LCD_TX_BYTES / 2) / nw;
  for (int r = 0; r < nh; r += rowsPerTx)
  {
    const int n = (nh - r < rowsPerTx) ? nh - r : rowsPerTx;
    // Walk the frame buffer along its rows (it lives in PSRAM) and scatter
    // into the transfer buffer.
    for (int i = 0; i < nw; i++)
    {
      const int px = px0 + i;
      if (!lcdFlipped)
      {
        const uint16_t *s = src + px * SCR_W + (SCR_W - 1 - (py0 + r));
        for (int j = 0; j < n; j++)
          dst[j * nw + i] = s[-j];
      }
      else
      {
        const uint16_t *s = src + (SCR_H - 1 - px) * SCR_W + (py0 + r);
        for (int j = 0; j < n; j++)
          dst[j * nw + i] = s[j];
      }
    }

    // Pixels: opcode 0x32 and RAMWR in the address, data on four lines. CS
    // stays low across the transfers of one window.
    spi_transaction_ext_t t = {};
    t.base.flags = SPI_TRANS_MODE_QIO;
    if (r == 0)
    {
      t.base.cmd = 0x32;
      t.base.addr = 0x002C00;
    }
    else
    {
      t.base.flags |= SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR;
    }
    if (r + n < nh)
      t.base.flags |= SPI_TRANS_CS_KEEP_ACTIVE;
    t.base.tx_buffer = lcdTx;
    t.base.length = (size_t)n * nw * 16;
    spi_device_polling_transmit(lcdSpi, &t.base);
  }
  spi_device_release_bus(lcdSpi);
}

static void panelFlushAll() { panelFlush(0, 0, SCR_W, SCR_H); }

// ================================================================== touch ==
// The touch half of the ST77922: 16-bit register addresses, MSB first.

#define TP_REG_STATUS 0x0001
#define TP_REG_MAX_X 0x0005
#define TP_REG_MAX_TOUCHES 0x0009
#define TP_REG_REPORT 0x0010 // 4 bytes of report header, then 7 bytes per point
#define TP_POINT_BYTES 7
#define TP_MAX_POINTS 10

static bool touchReady = false;
static uint8_t touchPoints = TP_MAX_POINTS;

static bool touchRead(uint16_t reg, uint8_t *buf, size_t n)
{
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)reg);
  if (Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, n) != n)
    return false;
  for (size_t i = 0; i < n; i++)
    buf[i] = Wire.read();
  return true;
}

static void touchInit()
{
  pinMode(TOUCH_PIN_RST, OUTPUT);
  digitalWrite(TOUCH_PIN_RST, HIGH);
  delay(5);
  digitalWrite(TOUCH_PIN_RST, LOW);
  delay(5);
  digitalWrite(TOUCH_PIN_RST, HIGH);
  delay(40);
  Wire.begin(TOUCH_PIN_SDA, TOUCH_PIN_SCL, 400000);

  uint8_t status = 0xFF, res[4] = {0};
  for (int i = 0; i < 20; i++) // low nibble 1 = controller still starting
  {
    if (touchRead(TP_REG_STATUS, &status, 1) && (status & 0x0F) != 1)
    {
      touchReady = true;
      break;
    }
    delay(10);
  }
  if (touchReady)
  {
    uint8_t n = 0;
    touchRead(TP_REG_MAX_X, res, 4);
    if (touchRead(TP_REG_MAX_TOUCHES, &n, 1) && n >= 1 && n <= TP_MAX_POINTS)
      touchPoints = n;
  }
  Serial.printf("[touch] %s, status 0x%02X, range %u x %u, %u points\n", touchReady ? "ready" : "not responding",
                status, ((res[0] & 0x3F) << 8) | res[1], ((res[2] & 0x3F) << 8) | res[3], touchPoints);
}

static int touchRawX = 0, touchRawY = 0; // last point as the controller reported it

// First touch point in screen (landscape) coordinates.
static bool touchGetXY(int &x, int &y)
{
  // The whole report is read every time: the controller holds a frame until
  // it has been read out, so reading only the first point leaves it stuck on
  // the first touch.
  uint8_t d[4 + TP_MAX_POINTS * TP_POINT_BYTES];
  if (!touchReady || !touchRead(TP_REG_REPORT, d, 4 + touchPoints * TP_POINT_BYTES))
    return false;
  const uint8_t *p = d + 4;
  if (!(p[0] & 0x80))
    return false;
  int px = ((p[0] & 0x3F) << 8) | p[1];
  int py = ((p[2] & 0x3F) << 8) | p[3];
  touchRawX = px;
  touchRawY = py;
  if (!lcdFlipped)
  {
    x = LCD_NATIVE_H - 1 - py;
    y = px;
  }
  else
  {
    x = py;
    y = LCD_NATIVE_W - 1 - px;
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
#define G_MAX_KH 480.0f // full-scale of the ring: 16 KH/s per segment (this board runs ~370)

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
// Widgets are drawn into a scratch sprite, copied into the frame buffer and
// that rectangle is sent to the panel.

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
  wgt.pushToSprite(&fb, x, y);
  panelFlush(x, y, wgt.width(), wgt.height());
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

// Draws the segments whose level changed; flushes them unless the caller
// repaints the whole screen anyway.
static void gaugeDraw(TFT_eSPI &d, float v, bool flush)
{
  float pos = v * G_SEGS;
  int x0 = SCR_W, y0 = SCR_H, x1 = 0, y1 = 0;
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

    // bounding box of the segment (drawArc angles: 0 at 6 o'clock, clockwise)
    for (int k = 0; k < 4; k++)
    {
      float a = (a0 + (k & 1 ? 7 : 0)) * DEG_TO_RAD;
      int rr = (k & 2) ? G_R : G_RI;
      int sx = G_CX - (int)(rr * sinf(a)), sy = G_CY + (int)(rr * cosf(a));
      x0 = min(x0, sx);
      x1 = max(x1, sx);
      y0 = min(y0, sy);
      y1 = max(y1, sy);
    }
  }
  if (flush && x1 >= x0)
    panelFlush(x0 - 3, y0 - 3, x1 - x0 + 7, y1 - y0 + 7);
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
  fb.fillSmoothCircle(17, 17, 5, mix565(C_TRACK, base, 70 + lvl * 18), C_BG);
  panelFlush(8, 8, 20, 20);
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
  gaugeDraw(d, gaugeShown, false);
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
  paintChrome(fb, scr);
  panelFlushAll();
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

void es3c35p_MinerScreen(unsigned long mElapsed)
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

void es3c35p_MarketScreen(unsigned long mElapsed)
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

void es3c35p_ClockScreen(unsigned long mElapsed)
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
#ifdef ES3C35P_UI_SCREENSHOT
static void dumpScreens(void);
#endif

void es3c35p_Init(void)
{
  // Speaker amplifier off, backlight off until the first frame is on the panel
  pinMode(AMP_PIN_SHUTDOWN, OUTPUT);
  digitalWrite(AMP_PIN_SHUTDOWN, HIGH);
  ledcSetup(0, 5000, 8); // LEDC channel 0, 5kHz, 8 bit (0-255 duty)
  ledcAttachPin(LCD_PIN_BL, 0);
  ledcWrite(0, 0);

  if (nvMem.loadConfig(&Settings)) // also the invert setting, read by panelInit
    lcdFlipped = Settings.flipDisplay;

  // TFT_eSPI's text path touches its SPI port's registers even when the
  // target is a sprite. That port is SPI3 here (USE_HSPI_PORT), which nothing
  // else uses: keep it clocked so those writes are harmless.
  periph_module_enable(PERIPH_SPI3_MODULE);

  fb.setColorDepth(16);
  fb.createSprite(SCR_W, SCR_H);
  if (!fb.created())
    Serial.printf("[ui] frame buffer alloc failed (PSRAM %u bytes free)\n", ESP.getFreePsram());
  else
    fb.fillSprite(C_BG);

  if (!panelInit())
    Serial.println("[ui] panel init failed");
  panelFlushAll();
  ledcWrite(0, Settings.Brightness);

  touchInit();
  xTaskCreate(touchTask, "touch", 3072, NULL, 2, NULL);

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

#ifdef ES3C35P_UI_SCREENSHOT
  dumpScreens();
#endif
}

void es3c35p_AlternateScreenState(void)
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
  lcdFlipped = !lcdFlipped;
  persistFlip(lcdFlipped);
  panelFlushAll();
}

// The button is polled from the Arduino loop task, drawing happens on the
// monitor task: hand the request over, except while the boot / setup pages
// are up (the monitor task is not drawing then).
static volatile bool pendingFlip = false;

void es3c35p_AlternateRotation(void)
{
  if (uiLive)
    pendingFlip = true;
  else
    applyFlip();
}

void es3c35p_LoadingScreen(void)
{
  uiLive = false;
  uiScreen = -1;
  paintLoading(fb);
  panelFlushAll();
}

void es3c35p_SetupScreen(void)
{
  uiLive = false;
  uiScreen = -1;
  paintSetup(fb);
  panelFlushAll();
}

void es3c35p_AnimateCurrentScreen(unsigned long frame)
{
  if (!uiLive || uiScreen < 0)
    return;
  drawPulse(frame);
  if (uiScreen == SCR_MINE && fabsf(gaugeTarget - gaugeShown) > 0.002f)
  {
    gaugeShown += (gaugeTarget - gaugeShown) * 0.3f; // ease toward the measured rate
    gaugeDraw(fb, gaugeShown, true);
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
// only HDR_H tall, too thin a target on a capacitive panel).
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
        Serial.printf("[touch] x=%d y=%d (raw %d,%d)\n", t_x, t_y, touchRawX, touchRawY);
      }
    }
    else
    {
      if (wasDown)
        Serial.println("[touch] released");
      downCount = 0;
      wasDown = false;
    }
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

void es3c35p_DoLedStuff(unsigned long frame)
{
  int action = pendingTouch;
  pendingTouch = TA_NONE;
  DisplayDriver *drv = currentDisplayDriver;
  switch (action)
  {
  case TA_BACKLIGHT:
    es3c35p_AlternateScreenState();
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

// ----------------------------------------------------- dev: screenshot dump --

#ifdef ES3C35P_UI_SCREENSHOT
// Raw frame buffer: SCR_W x SCR_H pixels, 2 bytes each, high byte first.
static void dumpShot(const char *name)
{
  panelFlushAll();
  Serial.printf("\nSHOT_BEGIN %s %d %d\n", name, SCR_W, SCR_H);
  const uint8_t *p = (const uint8_t *)fb.getPointer();
  size_t left = (size_t)SCR_W * SCR_H * 2;
  while (left)
  {
    size_t n = Serial.write(p, left > 512 ? 512 : left);
    p += n;
    left -= n;
    Serial.flush(); // the USB serial drops data when its buffer overruns
    if (!n)
      delay(2);
  }
  Serial.printf("\nSHOT_END\n");
  Serial.flush();
}

static void dumpScreens(void)
{
  // Wait for the capture script to say it is listening.
  Serial.println("SHOT_READY (send any byte)");
  for (int i = 0; i < 600 && !Serial.available(); i++)
    delay(50);
  while (Serial.available())
    Serial.read();

  const int bootAccent = accentIdx;
  HdrV h;
  h.time = "21:47";
  h.ip = "192.168.100.100";
  h.bars = 3;
  h.status = NM_hashing;

  for (int i = 0; i < HIST_N; i++)
    hist[i] = 310 + 30 * sinf(i * 0.45f) + (i * 7 % 23);
  histCount = HIST_N;
  histEpoch = 1;

  for (int acc = 0; acc < 2; acc++)
  {
    // accent 0 shows every screen; accent 2 (amber/rose) shows the mining screen again
    setAccent(acc == 0 ? 0 : 2);

    // loading + setup
    paintLoading(fb);
    dumpShot(acc == 0 ? "loading" : "loading_alt");
    if (acc == 0)
    {
      paintSetup(fb);
      dumpShot("setup");
    }

    // mine
    uiLive = true;
    gaugeShown = gaugeTarget = 0.84f;
    uiScreen = -1;
    ensureChrome(SCR_MINE);
    MineV m;
    m.hdr = h;
    m.hash = "372.4";
    m.shares = "1,284";
    m.best = "8.73M";
    m.tmpl = "412";
    m.blocks = "0";
    m.total = "58213";
    m.kh = 372.4f;
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
    k.remaining = "111655";
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
    c.hash = "372.4";
    paintClock(c);
    dumpShot("clock");
  }

  uiLive = false;
  uiScreen = -1;
  setAccent(bootAccent);
  resetWidgets();
  histCount = 0;
}
#endif

CyclicScreenFunction es3c35pCyclicScreens[] = {es3c35p_MinerScreen, es3c35p_MarketScreen, es3c35p_ClockScreen};

DisplayDriver es3c35pDriver = {
    es3c35p_Init,
    es3c35p_AlternateScreenState,
    es3c35p_AlternateRotation,
    es3c35p_LoadingScreen,
    es3c35p_SetupScreen,
    es3c35pCyclicScreens,
    es3c35p_AnimateCurrentScreen,
    es3c35p_DoLedStuff,
    SCREENS_ARRAY_SIZE(es3c35pCyclicScreens),
    0,
    SCR_W,
    SCR_H};
#endif
