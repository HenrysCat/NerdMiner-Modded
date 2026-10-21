#include "displayDriver.h"

#ifdef V1_DISPLAY

#include <TFT_eSPI.h>
#include "media/images_240_135.h"
#include "media/myFonts.h"
#include "media/Free_Fonts.h"
#include "version.h"
#include "monitor.h"
#include "OpenFontRender.h"
#include "rotation.h"
#include "display.h"
#include <Preferences.h>
#if defined(M5STICK_C_PLUS2)
#include "mining.h"
#include "m5stickPlus2Hw.h"
#endif

#define WIDTH 240
#define HEIGHT 135

OpenFontRender render;
TFT_eSPI tft = TFT_eSPI();                  // Invoke library, pins defined in User_Setup.h
TFT_eSprite background = TFT_eSprite(&tft); // Invoke library sprite

static void loadSavedScreen();

void tDisplay_Init(void)
{
  tft.init();
  tft.setRotation(ROTATION_90);
  tft.setSwapBytes(true);                 // Swap the colour byte order when rendering
  background.createSprite(WIDTH, HEIGHT); // Background Sprite
  background.setSwapBytes(true);
  render.setDrawer(background);  // Link drawing object to background instance (so font will be rendered on background)
  render.setLineSpaceRatio(0.9); // Espaciado entre texto

  loadSavedScreen();

#if defined(M5STICK_C_PLUS2) && PLUS2_BACKLIGHT_PWM
  // Backlight on LEDC channel 0, like the CYD boards: the shared backlight
  // code in display.cpp (brightness, screen timeout, night dimming, all set
  // on the LAN settings page) drives that channel. 256 Hz as in M5GFX for
  // this board: at 5 kHz the Plus2 backlight stays dark.
  ledcSetup(0, PLUS2_BACKLIGHT_FREQ, 8);
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, backlightLevel());
#endif

  // Load the font and check it can be read OK
  // if (render.loadFont(NotoSans_Bold, sizeof(NotoSans_Bold))) {
  if (render.loadFont(DigitalNumbers, sizeof(DigitalNumbers)))
  {
    Serial.println("Initialise error");
    return;
  }
}

void tDisplay_AlternateScreenState(void)
{
  Serial.println("Switching display state");
#if defined(M5STICK_C_PLUS2) && PLUS2_BACKLIGHT_PWM
  // The backlight pin belongs to LEDC channel 0 (see tDisplay_Init)
  if (ledcRead(0) > 0)
    ledcWrite(0, 0);
  else
    ledcWrite(0, backlightLevel()); // day, night or in between
#else
  int screen_state = digitalRead(TFT_BL);
  digitalWrite(TFT_BL, !screen_state);
#endif
}

void tDisplay_AlternateRotation(void)
{
  tft.setRotation( flipRotation(tft.getRotation()) );
}

// ---------------------------------------------------------------------------
// Shared helpers for the 240x135 screens
// ---------------------------------------------------------------------------

// Rows actually stored in a background array. MinerScreen, initScreen and
// priceScreen only hold 240x128 pixels (MinerHeight still says 135), the
// others are declared larger than their 240x135 content.
#define BG_ROWS(img, w) ((int)min((size_t)HEIGHT, sizeof(img) / sizeof(img[0]) / (w)))

// Draw a background and repeat its last row down to line 135. Pushing a
// 128-row image as 135 rows read past the end of the array (7 lines of
// garbage at the bottom of the miner screen); pushing it as 128 rows left
// the previous screen visible in those lines (price screen).
static void pushBackground(const unsigned short *img, int w, int rows)
{
  background.pushImage(0, 0, w, rows, img);
  for (int y = rows; y < HEIGHT; y++)
    background.pushImage(0, y, w, 1, img + (rows - 1) * w);
}

// The BTC price in at most `room` pixels of the current free font:
// "EUR 93120" -> "93120" -> "93.1k" -> "93k". The currency prefix goes first
// (it is the one picked in the settings). "--" until a price is known.
static String fitPrice(const String &price, unsigned int value, int room)
{
  if (value == 0) return String("--");

  int i = price.length();
  while (i > 0 && isDigit(price[i - 1])) i--;

  char c1[16], c2[16];
  if (value >= 1000000UL) {
    snprintf(c1, sizeof(c1), "%.1fM", value / 1000000.0);
    snprintf(c2, sizeof(c2), "%luM", (unsigned long)(value / 1000000UL));
  } else {
    snprintf(c1, sizeof(c1), "%.1fk", value / 1000.0);
    snprintf(c2, sizeof(c2), "%luk", (unsigned long)(value / 1000UL));
  }

  const String candidates[] = {price, price.substring(i), String(c1), String(c2)};
  for (const String &c : candidates)
    if (background.textWidth(c) <= room) return c;
  return String(c2);
}

// ---------------------------------------------------------------------------
// Last screen shown, kept across restarts (NVS namespace "v1_ui", same idea
// as the CYD's "cyd_ui"). The monitor task resets to the first screen when
// it starts, so the saved one is put back on the first draw.
// ---------------------------------------------------------------------------
#define V1_UI_PREFS_NS "v1_ui"
#define V1_SCREEN_SAVE_DELAY_ms 5000 // wait until the user stops clicking

static int s_savedScreen = 0;
static bool s_screenRestored = false;
static int s_lastSeenScreen = -1;
static uint32_t s_screenChangedAt = 0;

static void loadSavedScreen()
{
  Preferences p;
  // read-write: read-only logged "nvs_open failed: NOT_FOUND" at every boot
  // until a screen had been saved once (the namespace did not exist yet)
  if (p.begin(V1_UI_PREFS_NS, false)) {
    s_savedScreen = p.getUChar("screen", 0);
    p.end();
  }
}

static void saveCurrentScreen()
{
  if (!s_screenRestored)
    return; // not before the saved screen has been put back
  const int cur = currentDisplayDriver->current_cyclic_screen;
  if (cur != s_lastSeenScreen) {
    s_lastSeenScreen = cur;
    s_screenChangedAt = millis();
    return;
  }
  if (cur == s_savedScreen || millis() - s_screenChangedAt < V1_SCREEN_SAVE_DELAY_ms)
    return;
  Preferences p;
  if (p.begin(V1_UI_PREFS_NS, false)) {
    p.putUChar("screen", cur);
    p.end();
  }
  s_savedScreen = cur;
}

// Called first thing by every cyclic screen: on the very first draw, switch
// to the saved screen and draw that one instead. True when it did.
static bool restoreSavedScreen(unsigned long mElapsed)
{
  if (s_screenRestored)
    return false;
  s_screenRestored = true;
  const int n = currentDisplayDriver->num_cyclic_screens;
  if (s_savedScreen <= 0 || s_savedScreen >= n || s_savedScreen == currentDisplayDriver->current_cyclic_screen)
    return false;
  currentDisplayDriver->current_cyclic_screen = s_savedScreen;
  currentDisplayDriver->cyclic_screens[s_savedScreen](mElapsed);
  return true;
}

#if defined(M5STICK_C_PLUS2) && defined(PIN_BATTERY_ADC)
// ---------------------------------------------------------------------------
// Battery (M5StickC Plus2): reading and percentage in m5stickPlus2Hw.cpp
// ---------------------------------------------------------------------------
static int batteryMillivolts() { return plus2BatteryMillivolts(); }
static int batteryPercent(int mv) { return plus2BatteryPercent(mv); }

// Battery icon + percentage on the black area left of the grey banner
// (where the chip temperature was: the classic ESP32's internal sensor is
// unreliable, many chips read an almost constant value).
static void drawBattery()
{
  const int pct = batteryPercent(batteryMillivolts());
  const uint16_t fill = pct > 50 ? 0x07E0 : pct > 20 ? 0xFD20 : 0xF800; // green / orange / red
  const String label = String(pct) + "%";

  // Text right-aligned just before the banner (it starts at x~160), icon
  // right before the text, whatever the number of digits.
  background.setFreeFont(FSS9);
  background.setTextSize(1);
  background.setTextColor(0xDEDB);
  background.setTextDatum(TR_DATUM);
  const int textRight = 156;
  background.drawString(label, textRight, 0, GFXFF);
  background.setTextDatum(TL_DATUM);

  const int w = 16, h = 9, y = 2;
  const int x = textRight - background.textWidth(label) - 4 - (w + 2);
  background.drawRect(x, y, w, h, 0xDEDB);
  background.fillRect(x + w, y + 3, 2, 3, 0xDEDB); // terminal
  const int fw = (w - 4) * pct / 100;
  if (fw > 0) background.fillRect(x + 2, y + 2, fw, h - 4, fill);
}
#endif

#if defined(M5STICK_C_PLUS2)
extern volatile uint32_t shares; // 32-bit shares accepted (mining.cpp)

// Up to 6 digits as they are (they fit left of the VALID BLOCKS box), then
// in millions: "9.88M".
static String shareCount(uint32_t n)
{
  if (n < 1000000UL)
    return String(n);
  char buf[12];
  snprintf(buf, sizeof(buf), "%.2fM", n / 1000000.0);
  return String(buf);
}
#endif

void tDisplay_MinerScreen(unsigned long mElapsed)
{
  if (restoreSavedScreen(mElapsed)) return;

#if defined(M5STICK_C_PLUS2)
  // Pool stats: only its best share is used here (merged into BEST DIFF by
  // getPoolData). Throttled inside to one HTTPS call per UPDATE_POOL_min.
  getPoolData();
#endif

  mining_data data = getMiningData(mElapsed);

  // Print background screen (240x128 image, see pushBackground)
  pushBackground(MinerScreen, MinerWidth, BG_ROWS(MinerScreen, MinerWidth));

  Serial.printf(">>> Completed %s share(s), %s Khashes, avg. hashrate %s KH/s\n",
                data.completedShares.c_str(), data.totalKHashes.c_str(), data.currentHashRate.c_str());

  // Hashrate: size 26 so "970.0" fits left of "KH/s" (30 cut the first digit)
  render.setFontSize(26);
  render.setFontColor(TFT_BLACK);
  render.rdrawString(data.currentHashRate.c_str(), 96, 93, TFT_BLACK);
  // Total hashes
  render.setFontSize(13);
  render.rdrawString(data.totalMHashes.c_str(), 200, 106, TFT_BLACK);
#if defined(M5STICK_C_PLUS2)
  // Four rows, same small font for every label and every value:
  // TEMPLATES / BEST DIFF / ACCEPT / REJECT, as a two-column table (labels
  // left-aligned in one column, values left-aligned in the next one).
  // The labels of the image ("BLOCK TEMPLATES", "BEST DIFFICULTY",
  // "32BITs SHARES") are blacked out.
  // ACCEPT / REJECT are 32-bit shares (difficulty >= 1, about one an hour at
  // ~1 MH/s), accepted (green) and rejected (red), both saved across
  // restarts -- not the pool's low-difficulty shares (several a second).
  {
    // Layout, measured on the MinerScreen image: the NERD MINER logo ends at
    // x = 72, the yellow band starts at y = 74, the VALID BLOCKS box at x ~200.
    const int labelX = 80;                  // label column (left-aligned)
    const int valueX = 142;                 // value column (left-aligned)
    const int rowY[4] = {16, 31, 46, 61};   // 15 px apart (font is 8 px high)

    background.fillRect(74, 14, valueX - 2 - 74, 74 - 14, TFT_BLACK);
    // setTextFont(1) also clears the free font left selected by the other
    // drawings (drawBattery's FreeSans 9): with a free font set, TFT_eSPI
    // draws "font 1" in that free font, which made these labels far too big.
    background.setTextFont(1);
    background.setTextSize(1);
    background.setTextColor(0x8C71); // grey of the other labels
    background.setTextDatum(TL_DATUM);
    background.drawString("TEMPLATES", labelX, rowY[0], 1);
    background.drawString("BEST DIFF", labelX, rowY[1], 1);
    background.drawString("ACCEPT",    labelX, rowY[2], 1);
    background.drawString("REJECT",    labelX, rowY[3], 1);
    // Each value on the line of its label (the render font sits 1 px higher)
    render.setFontSize(7);
    render.drawString(data.templates.c_str(),               valueX, rowY[0] - 1, 0xDEDB);
    render.drawString(data.bestDiff.c_str(),                valueX, rowY[1] - 1, 0xDEDB);
    render.drawString(shareCount(shares).c_str(),           valueX, rowY[2] - 1, 0x07E0);
    render.drawString(shareCount(shares32Rejected).c_str(), valueX, rowY[3] - 1, 0xF800);
  }
#else
  // Block templates
  render.drawString(data.templates.c_str(), 140, 15, 0xDEDB);
  // Best diff
  render.drawString(data.bestDiff.c_str(), 140, 38, 0xDEDB);
  // 32Bit shares
  render.drawString(data.completedShares.c_str(), 140, 60, 0xDEDB);
#endif
  // Hores
  render.setFontSize(9);
  render.rdrawString(data.timeMining.c_str(), 226, 85, 0xDEDB);

  // Valid Blocks
  render.setFontSize(19);
  render.drawString(data.valids.c_str(), 210, 45, 0xDEDB);

#if defined(M5STICK_C_PLUS2) && defined(PIN_BATTERY_ADC)
  // Battery level, on the black area left of the grey banner
  drawBattery();
#else
  // Chip temperature, on the black area left of the grey banner (it used to
  // run into the time inside the banner)
  render.setFontSize(10);
  render.rdrawString(data.temp.c_str(), 148, 1, 0xDEDB);
  render.setFontSize(4);
  render.drawString("0", 149, 1, 0xDEDB); // degree sign
#endif

  // Time, inside the banner and clear of the pickaxe/WiFi icons (x >= 212)
  if (isTimeSynced()) {
    render.setFontSize(10);
    render.rdrawString(data.currentTime.c_str(), 208, 1, TFT_BLACK);
  }

  // Push prepared background to screen
  background.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Clock screen (240x135: T-Display V1, M5StickC Plus2, S3-GEEK, Cardputer Adv)
//
// Layout, measured on the minerClockScreen background:
//   grey banner  : BTC logo ends at x~147, pickaxe/WiFi icons start at x~213
//                  -> price fits in x 150..210, right aligned
//   black area   : "CLOCK MINER" logo in x 14..72 / y 18..45; free space to
//                  its right down to y~92 -> time + date centred on x=158
//   yellow band  : hashrate, right aligned on x=70 (left of "KH/s")
//   blue band    : block height, right aligned on x=190 (left of "CURRENT BLOCK")
// ---------------------------------------------------------------------------
#define CLOCK_PRICE_LEFT   150
#define CLOCK_PRICE_RIGHT  210
#define CLOCK_TIME_CENTER  158

void tDisplay_ClockScreen(unsigned long mElapsed)
{
  if (restoreSavedScreen(mElapsed)) return;

  clock_data data = getClockData(mElapsed);

  // Print background screen
  pushBackground(minerClockScreen, minerClockWidth, BG_ROWS(minerClockScreen, minerClockWidth));

  Serial.printf(">>> Completed %s share(s), %s Khashes, avg. hashrate %s KH/s\n",
                data.completedShares.c_str(), data.totalKHashes.c_str(), data.currentHashRate.c_str());

  // Hashrate (yellow band)
  render.setFontSize(20);
  render.setFontColor(TFT_BLACK);
  render.rdrawString(data.currentHashRate.c_str(), 70, 103, TFT_BLACK);

  // BTC price (grey banner), never over the icons
  background.setFreeFont(FSSB9);
  background.setTextSize(1);
  background.setTextColor(TFT_BLACK);
  background.setTextDatum(TR_DATUM);
  background.drawString(fitPrice(data.btcPrice, getBTCpriceValue(), CLOCK_PRICE_RIGHT - CLOCK_PRICE_LEFT), CLOCK_PRICE_RIGHT, 1, GFXFF);

  // Block height (blue band). The digital font has no '-' glyph, so the
  // placeholder is drawn with the free font.
  if (isBlockHeightKnown()) {
    render.setFontSize(14);
    render.rdrawString(data.blockHeight.c_str(), 190, 110, TFT_BLACK);
  } else {
    background.setTextDatum(TR_DATUM);
    background.drawString("--", 190, 110, GFXFF);
  }

  // Time + date (black area, right of the logo). Transparent text: the
  // sprite is redrawn from the background image on every refresh, so no
  // background box is needed -- the old one cut into the logo.
  const bool synced = isTimeSynced();
  background.setTextColor(0xDEDB);
  background.setTextDatum(TC_DATUM);
  background.setFreeFont(&FreeSansBold24pt7b);
  background.drawString(synced ? data.currentTime : String("--:--"), CLOCK_TIME_CENTER, 26, GFXFF);
  background.setFreeFont(&FreeSans9pt7b);
  background.drawString(synced ? data.currentDate : String("--/--/----"), CLOCK_TIME_CENTER, 70, GFXFF);

  background.setTextDatum(TL_DATUM); // the other screens expect the default

  // Push prepared background to screen
  background.pushSprite(0, 0);
}

void tDisplay_GlobalHashScreen(unsigned long mElapsed)
{
  if (restoreSavedScreen(mElapsed)) return;

  coin_data data = getCoinData(mElapsed);

  // Print background screen
  pushBackground(globalHashScreen, globalHashWidth, BG_ROWS(globalHashScreen, globalHashWidth));

  Serial.printf(">>> Completed %s share(s), %s Khashes, avg. hashrate %s KH/s\n",
                data.completedShares.c_str(), data.totalKHashes.c_str(), data.currentHashRate.c_str());

  // Grey banner: time on the right, price between the BTC logo (ends x~147)
  // and the time. Both used to be drawn over each other.
  if (isTimeSynced()) {
    render.setFontSize(10);
    render.rdrawString(data.currentTime.c_str(), 236, 1, TFT_BLACK);
  }

  background.setFreeFont(FSSB9);
  background.setTextSize(1);
  background.setTextDatum(TL_DATUM);
  background.setTextColor(TFT_BLACK);
  background.drawString(fitPrice(data.btcPrice, getBTCpriceValue(), 193 - 150), 150, 1, GFXFF);

  // Print Last Pool Block
  background.setFreeFont(FSS9);
  background.setTextDatum(TR_DATUM);
  background.setTextColor(0x9C92);
  background.drawString(data.halfHourFee.c_str(), 230, 40, GFXFF);

  // Print Difficulty
  background.setFreeFont(FSS9);
  background.setTextDatum(TR_DATUM);
  background.setTextColor(0x9C92);
  background.drawString(data.netwrokDifficulty.c_str(), 230, 68, GFXFF);

  // Print Global Hashrate
  render.setFontSize(12);
  render.rdrawString(data.globalHashRate.c_str(), 205, 115, TFT_BLACK);

  // Print BlockHeight: size 22, right edge 119, so all six digits fit inside
  // the red box (size 23 at x=105 cut the first digit)
  const bool blockKnown = isBlockHeightKnown();
  if (blockKnown) {
    render.setFontSize(22);
    render.rdrawString(data.blockHeight.c_str(), 119, 80, 0xDEDB);
  } else {
    background.setFreeFont(FSSB12);
    background.setTextDatum(TC_DATUM);
    background.setTextColor(0xDEDB);
    background.drawString("--", 66, 84, GFXFF);
  }

  // Halving progress bar, inside the pink box (x 1..106, y 117..132). It was
  // drawn at y=149, below the bottom of this 135-line screen.
  if (blockKnown) {
    float pct = data.progressPercent;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    const int barW = (int)(106 * pct / 100.0f);
    if (barW > 0) background.fillRect(1, 117, barW, 16, 0xDEDB);
  }

  // Print Remaining BLocks
  background.setTextFont(FONT2);
  background.setTextSize(1);
  background.setTextDatum(MC_DATUM);
  background.setTextColor(TFT_BLACK);
  background.drawString(blockKnown ? data.remainingBlocks.c_str() : "--", 54, 125, FONT2);

  background.setTextDatum(TL_DATUM);

  // Push prepared background to screen
  background.pushSprite(0, 0);
}

void tDisplay_BTCprice(unsigned long mElapsed)
{
  if (restoreSavedScreen(mElapsed)) return;

  clock_data data = getClockData(mElapsed);
  
  // Print background screen (240x128 image, see pushBackground)
  pushBackground(priceScreen, priceScreenWidth, BG_ROWS(priceScreen, priceScreenWidth));

  Serial.printf(">>> Completed %s share(s), %s Khashes, avg. hashrate %s KH/s\n",
                data.completedShares.c_str(), data.totalKHashes.c_str(), data.currentHashRate.c_str());

  // Hashrate: size 20 so it fits left of "KH/s" (25 cut the first digit)
  render.setFontSize(20);
  render.setFontColor(TFT_BLACK);
  render.rdrawString(data.currentHashRate.c_str(), 70, 103, TFT_BLACK);

  // Print BlockHeight, inside the red band
  if (isBlockHeightKnown()) {
    render.setFontSize(16);
    render.rdrawString(data.blockHeight.c_str(), 190, 108, TFT_WHITE);
  } else {
    background.setFreeFont(FSSB9);
    background.setTextDatum(TR_DATUM);
    background.setTextColor(TFT_WHITE);
    background.drawString("--", 190, 108, GFXFF);
  }

  // Print Hour (grey banner, left of the icons)
  background.setFreeFont(FSSB9);
  background.setTextSize(1);
  background.setTextDatum(TL_DATUM);
  background.setTextColor(TFT_BLACK);
  background.drawString(isTimeSynced() ? data.currentTime : String("--:--"), 148, 1, GFXFF);

  // Print BTC Price, centred in the black area right of the logo (x 76..238).
  // Big font when it fits, otherwise a smaller one, then a shorter text; no
  // background box (the sprite is redrawn every time).
  const int priceRoom = 238 - 76;
  const int priceCenter = 157;
  background.setTextDatum(TC_DATUM);
  background.setTextColor(0xDEDB);
  background.setFreeFont(FF23);
  if (background.textWidth(data.btcPrice) <= priceRoom && getBTCpriceValue() != 0) {
    background.drawString(data.btcPrice, priceCenter, 44, GFXFF);
  } else {
    background.setFreeFont(FSSB12);
    background.drawString(fitPrice(data.btcPrice, getBTCpriceValue(), priceRoom), priceCenter, 50, GFXFF);
  }

  background.setTextDatum(TL_DATUM);

  // Push prepared background to screen
  background.pushSprite(0, 0);
}

void tDisplay_LoadingScreen(void)
{
  tft.fillScreen(TFT_BLACK);
  tft.pushImage(0, 0, initWidth, initHeight, initScreen);
  // Version in the black strip under the 240x128 image (it was drawn at
  // y=147, below the bottom of the 135-line screen)
  tft.setTextFont(1); // the 6x8 font, not a free font that might be selected
  tft.setTextColor(0xDEDB, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(CURRENT_VERSION, 4, initHeight, 1);
}

void tDisplay_SetupScreen(void)
{
  tft.pushImage(0, 0, setupModeWidth, setupModeHeight, setupModeScreen);
}

#if defined(M5STICK_C_PLUS2) && defined(PIN_BATTERY_ADC) && PLUS2_LOW_BATTERY_OFF
// ---------------------------------------------------------------------------
// Low battery: below PLUS2_BATTERY_OFF_mV (under mining load) for 30 s in a
// row -> message, then the power latch is released. On USB the stick keeps
// running (the latch is taken again) and checking goes on. Readings under
// 2.5 V are treated as a measuring problem, not as an empty cell.
// ---------------------------------------------------------------------------
#define BATTERY_CHECK_ms 5000
#define BATTERY_LOW_FOR_ms 30000

static void batteryGuardTick()
{
  static uint32_t lastCheck = 0, lowSince = 0;
  if (lastCheck != 0 && millis() - lastCheck < BATTERY_CHECK_ms)
    return;
  lastCheck = millis();

  const int mv = batteryMillivolts();
  if (mv >= PLUS2_BATTERY_OFF_mV || mv < 2500) {
    lowSince = 0;
    return;
  }
  if (lowSince == 0) {
    lowSince = millis();
    Serial.printf("Battery low: %d mV\n", mv);
    return;
  }
  if (millis() - lowSince < BATTERY_LOW_FOR_ms)
    return;

  Serial.printf("Battery empty (%d mV): switching off\n", mv);
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.drawString("Batterie faible", WIDTH / 2, HEIGHT / 2 - 14, 4);
  tft.setTextColor(0xDEDB, TFT_BLACK);
  tft.drawString("Arret... rechargez le stick", WIDTH / 2, HEIGHT / 2 + 16, 2);
  tft.setTextDatum(TL_DATUM);
  saveStat(); // before the power goes
  delay(3000);
  plus2CutPower(); // returns only on USB power
  Serial.println("Still powered (USB): carrying on");
  lowSince = 0;
}
#endif

void tDisplay_AnimateCurrentScreen(unsigned long frame)
{
  saveCurrentScreen(); // every ~100 ms on the monitor task
#if defined(M5STICK_C_PLUS2) && defined(PIN_BATTERY_ADC)
  plus2BatteryMillivolts(); // keeps the reading fresh (every 5 s) for the web monitor too
#endif
#if defined(M5STICK_C_PLUS2) && defined(PIN_BATTERY_ADC) && PLUS2_LOW_BATTERY_OFF
  batteryGuardTick();
#endif
}

#if defined(M5STICK_C_PLUS2)
// ---------------------------------------------------------------------------
// Share / block signal (M5StickC Plus2): red LED (GPIO19, active high) and
// passive buzzer (GPIO2, LEDC channel 4 = timer 2, away from the backlight's
// channel 0 / timer 0). Driven from the monitor loop (~100 ms per call), no
// blocking delays. 32-bit share (about once an hour at ~1 MH/s): one 100 ms
// flash + short beep, at most every 10 s. Block: 3 s of flashing with a
// rising beep pattern.
// ---------------------------------------------------------------------------
#define BUZZER_CHANNEL 4
static uint32_t s_seenShares = 0, s_seenBlocks = 0;
static bool s_signalInit = false;
static int s_signalSteps = 0;   // ~100 ms steps left in the current pattern
static bool s_blockPattern = false;
static uint32_t s_lastShareSignal = 0;
#define SHARE_SIGNAL_MIN_GAP_ms 10000

static void buzzer(uint32_t freq)
{
#if SHARE_BEEP
  if (freq) ledcWriteTone(BUZZER_CHANNEL, freq);
  else ledcWrite(BUZZER_CHANNEL, 0);
#endif
}

void tDisplay_DoLedStuff(unsigned long frame)
{
  if (!s_signalInit) {
    s_signalInit = true;
    s_seenShares = g_shareFoundEvents; // nothing to celebrate from before boot
    s_seenBlocks = g_blockFoundEvents;
#if SHARE_BEEP
    ledcSetup(BUZZER_CHANNEL, 4000, 8);
    ledcAttachPin(PIN_BUZZER, BUZZER_CHANNEL);
    ledcWrite(BUZZER_CHANNEL, 0);
#endif
  }

  if (g_blockFoundEvents != s_seenBlocks) {
    s_seenBlocks = g_blockFoundEvents;
    s_seenShares = g_shareFoundEvents;
    s_blockPattern = true;
    s_signalSteps = 30;
  } else if (g_shareFoundEvents != s_seenShares) {
    s_seenShares = g_shareFoundEvents;
    // At most one share signal every 10 s, whatever the counter does
    if (s_signalSteps == 0 && (s_lastShareSignal == 0 || millis() - s_lastShareSignal >= SHARE_SIGNAL_MIN_GAP_ms)) {
      s_lastShareSignal = millis();
      s_blockPattern = false;
      s_signalSteps = 2;
    }
  }

  if (s_signalSteps <= 0)
    return;
  s_signalSteps--;

  bool led;
  uint32_t tone = 0;
  if (s_blockPattern) {
    led = (s_signalSteps % 2) == 0;
    if (s_signalSteps >= 24) tone = 2000 + (30 - s_signalSteps) * 400; // rising chirp
  } else {
    led = s_signalSteps == 1;   // first step on, second off
    if (s_signalSteps == 1) tone = 4000;
  }
  if (s_signalSteps == 0) { led = false; tone = 0; }

#if SHARE_LED
  digitalWrite(LED_PIN, led ? HIGH : LOW);
#else
  (void)led;
#endif
  buzzer(tone);
}
#else
void tDisplay_DoLedStuff(unsigned long frame)
{
}
#endif

CyclicScreenFunction tDisplayCyclicScreens[] = {tDisplay_MinerScreen, tDisplay_ClockScreen, tDisplay_GlobalHashScreen, tDisplay_BTCprice};

DisplayDriver tDisplayV1Driver = {
    tDisplay_Init,
    tDisplay_AlternateScreenState,
    tDisplay_AlternateRotation,
    tDisplay_LoadingScreen,
    tDisplay_SetupScreen,
    tDisplayCyclicScreens,
    tDisplay_AnimateCurrentScreen,
    tDisplay_DoLedStuff,
    SCREENS_ARRAY_SIZE(tDisplayCyclicScreens),
    0,
    WIDTH,
    HEIGHT};
#endif
