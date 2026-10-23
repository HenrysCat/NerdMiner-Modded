#include <Arduino.h>
#include <WiFi.h>
#include "mbedtls/md.h"
#include "HTTPClient.h"
#include <WiFiClientSecure.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <list>
#include <atomic>
#include "mining.h"
#include "utils.h"
#include "monitor.h"
#include "wManager.h"
#include "drivers/storage/storage.h"
#include "drivers/devices/device.h"
#include "currency.h"
#include "timeutil.h"
#ifdef M5STICK_C_PLUS2
#include "m5stickPlus2Hw.h"
#endif

extern uint32_t templates;
extern uint64_t totalKHashes;
extern uint32_t elapsedKHs;
extern uint64_t upTime;

extern volatile uint32_t shares; // increase if blockhash has 32 bits of zeroes
extern volatile uint32_t valids; // increased if blockhash <= targethalfshares

extern double best_diff; // track best diff

extern monitor_data mMonitor;

//from saved config
extern TSettings Settings; 
bool invertColors = false;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 3600, 60000);
unsigned int bitcoin_price=0;
String current_block = "793261";
global_data gData;
pool_data pData;
String poolAPIUrl;

// Different pools expose worker/difficulty stats through incompatible JSON
// schemas. getPoolAPIUrl() sets this alongside poolAPIUrl so getPoolData()
// knows how to parse whatever comes back.
enum PoolApiStyle {
  POOL_API_PUBLICPOOL = 0, // public-pool.io, sethforprivacy, solomining, hmpool.io (digi, bch)
  POOL_API_HMPOOL,         // btc.hmpool.io (HashedMax Unity Pool)
  // ckpool-solo family: heliospool.com/.eu/.asia AND pool.nerdminers.org
  // (confirmed from nerdminers.org's own fork source, golden-guy/ckpool-solo
  // @nerdminer_v2 -- its per-user JSON uses the same hashrate1hr/workers/
  // bestever field names as heliospool's schema below).
  POOL_API_CKPOOL,
};
int poolApiStyle = POOL_API_PUBLICPOOL;
// Both are worked out on the setup() task and again, later, on the monitor
// task that also reads them; working them out takes DNS lookups. The lock
// keeps a reader from seeing a half-finished answer (it used to see the
// public-pool default and ask the wrong pool for stats).
static std::mutex s_poolApiMutex;

// Changes from the LAN settings page that applyPendingSettings() applies on
// this task (see monitor.h).
static int s_appliedTimezone = DEFAULT_TIMEZONE;      // offset timeClient has
static const char *s_newCurrency = nullptr; // a kCurrencies code; g_settingsMutex
static std::atomic<bool> s_poolChanged{false};


void setup_monitor(void){
    /******** TIME ZONE SETTING *****/

    timeClient.begin();

    // Adjust offset depending on your zone
    // GMT +2 in seconds (zona horaria de Europa Central)
    s_appliedTimezone = Settings.Timezone;
    timeClient.setTimeOffset(3600 * s_appliedTimezone);

    Serial.println("TimeClient setup done");
#ifdef SCREEN_WORKERS_ENABLE
    Serial.println("poolAPIUrl: " + getPoolAPIUrl());
#endif
}

// Every HTTPS fetch below does its TLS handshake through mbedTLS's hardware
// SHA, which must not overlap the miner's use of the SHA engine -- the same
// reason getPoolData() takes g_hwShaMutex (see mining.h). Without it these
// requests all fail with -1 (TLS connect) while mining runs, leaving price,
// block height, fees, difficulty and network hashrate empty.
unsigned long mGlobalUpdate =0;

// Every fetch below pauses the hardware miner for its whole duration (TLS
// handshake under g_hwShaMutex, see above). They used to remember only the
// last SUCCESS, so a failing one (CoinGecko "429 Too Many Requests", internet
// down with WiFi still up...) was retried on every screen refresh, once a
// second, each attempt holding the miner for up to the 10 s timeout. Now a
// failed fetch waits FETCH_RETRY_ms before the next try, and the timeout is
// shorter.
#define FETCH_RETRY_ms   60000
#define FETCH_TIMEOUT_ms 5000
static bool fetchDue(unsigned long lastOk, unsigned long &lastTry, unsigned long periodMs)
{
    if (WiFi.status() != WL_CONNECTED) return false;
    const unsigned long now = millis();
    if (lastOk != 0 && now - lastOk <= periodMs) return false;
    if (lastTry != 0 && now - lastTry < FETCH_RETRY_ms) return false;
    lastTry = now;
    return true;
}
static unsigned long mGlobalTry = 0;
static unsigned long mHeightTry = 0;
static unsigned long mBTCTry = 0;

void updateGlobalData(void){
    
    if(fetchDue(mGlobalUpdate, mGlobalTry, UPDATE_Global_min * 60 * 1000)){
            
        //Make first API call to get global hash and current difficulty
        HTTPClient http;
        http.setTimeout(FETCH_TIMEOUT_ms);
        try {
        http.begin(getGlobalHash);
        int httpCode;
        { HW_SHA_REQUEST(); std::lock_guard<std::mutex> shaLock(g_hwShaMutex); httpCode = http.GET(); }

        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            
            StaticJsonDocument<1024> doc;
            deserializeJson(doc, payload);
            String temp = "";
            if (doc.containsKey("currentHashrate")) temp = String(doc["currentHashrate"].as<float>());
            if(temp.length()>18 + 3) //Exahashes more than 18 digits + 3 digits decimals
              gData.globalHash = temp.substring(0,temp.length()-18 - 3);
            if (doc.containsKey("currentDifficulty")) temp = String(doc["currentDifficulty"].as<float>());
            if(temp.length()>10 + 3){ //Terahash more than 10 digits + 3 digit decimals
              temp = temp.substring(0,temp.length()-10 - 3);
              gData.difficulty = temp.substring(0,temp.length()-2) + "." + temp.substring(temp.length()-2,temp.length()) + "T";
            }
            doc.clear();

            mGlobalUpdate = millis();
        }
        http.end();

      
        //Make third API call to get fees
        http.begin(getFees);
        { HW_SHA_REQUEST(); std::lock_guard<std::mutex> shaLock(g_hwShaMutex); httpCode = http.GET(); }

        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            
            StaticJsonDocument<1024> doc;
            deserializeJson(doc, payload);
            String temp = "";
            if (doc.containsKey("halfHourFee")) gData.halfHourFee = doc["halfHourFee"].as<int>();
#ifdef SCREEN_FEES_ENABLE
            if (doc.containsKey("fastestFee"))  gData.fastestFee = doc["fastestFee"].as<int>();
            if (doc.containsKey("hourFee"))     gData.hourFee = doc["hourFee"].as<int>();
            if (doc.containsKey("economyFee"))  gData.economyFee = doc["economyFee"].as<int>();
            if (doc.containsKey("minimumFee"))  gData.minimumFee = doc["minimumFee"].as<int>();
#endif
            doc.clear();

            mGlobalUpdate = millis();
        }
        
        http.end();
        } catch(...) {
          Serial.println("Global data HTTP error caught");
          http.end();
        }
    }
}

unsigned long mHeightUpdate = 0;
static volatile uint32_t s_blockHeight = 0; // numeric copy, safe to read from any task

String getBlockHeight(void){
    
    if(fetchDue(mHeightUpdate, mHeightTry, UPDATE_Height_min * 60 * 1000)){
            
        HTTPClient http;
        http.setTimeout(FETCH_TIMEOUT_ms);
        try {
        http.begin(getHeightAPI);
        int httpCode;
        { HW_SHA_REQUEST(); std::lock_guard<std::mutex> shaLock(g_hwShaMutex); httpCode = http.GET(); }

        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            payload.trim();

            current_block = payload;
            s_blockHeight = (uint32_t)payload.toInt();

            mHeightUpdate = millis();
        }        
        http.end();
        } catch(...) {
          Serial.println("Height HTTP error caught");
          http.end();
        }
    }
  
  return current_block;
}

unsigned long mBTCUpdate = 0;

String getBTCprice(void){
    const CurrencyInfo &cur = currencyFor(Settings.Currency);
    static char price_buffer[24];

    if(fetchDue(mBTCUpdate, mBTCTry, UPDATE_BTC_min * 60 * 1000)){

        HTTPClient http;
        http.setTimeout(FETCH_TIMEOUT_ms);

        try {
        http.begin(String(getBTCAPI) + cur.code);
        int httpCode;
        { HW_SHA_REQUEST(); std::lock_guard<std::mutex> shaLock(g_hwShaMutex); httpCode = http.GET(); }

        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();

            StaticJsonDocument<1024> doc;
            deserializeJson(doc, payload);
          
            if (doc.containsKey("bitcoin") && doc["bitcoin"].containsKey(cur.code)) {
                bitcoin_price = doc["bitcoin"][cur.code];
            }

            doc.clear();

            mBTCUpdate = millis();
        }
        
        http.end();
        } catch(...) {
          Serial.println("BTC price HTTP error caught");
          http.end();
        }
    }  
  
  snprintf(price_buffer, sizeof(price_buffer), "%s%u", cur.prefix, bitcoin_price);
  return String(price_buffer);
}

unsigned long mTriggerUpdate = 0;
unsigned long initialMillis = millis();
unsigned long initialTime = 0;
unsigned long mPoolUpdate = 0;

// ---------------------------------------------------------------------------
// Clock
//
// initialTime is local STANDARD time (UTC + Settings.Timezone hours, as the
// NTP client returns it) taken at millis() == mTriggerUpdate.
//  - AUTO_DST_EU: Settings.Timezone is the winter offset (1 for France) and
//    the EU summer-time hour is added on top, so the clock changes by itself
//    on the last Sundays of March and October.
//  - M5StickC Plus2: until the first NTP answer the time comes from the
//    on-board BM8563 RTC (kept in UTC, written after every NTP sync), so the
//    clock is right as soon as the stick is switched on, even without WiFi.
// ---------------------------------------------------------------------------
#ifdef M5STICK_C_PLUS2
// Read once, by the monitor task only. Other tasks (web server) only look at
// the result once s_rtcOk is set, after both values are fully written: a
// 64-bit value read while the other core writes it could come out half old,
// half new.
static bool s_rtcTried = false;
static volatile bool s_rtcOk = false;
static int64_t s_rtcUtc = 0;            // RTC time read at boot...
static unsigned long s_rtcMillis = 0;   // ...at this millis()

static void rtcSeedOnce()
{
  if (s_rtcTried) return;
  s_rtcTried = true;
  int64_t utc;
  if (plus2RtcRead(utc)) {
    s_rtcUtc = utc;
    s_rtcMillis = millis();
    __sync_synchronize(); // both stored before the flag is seen by the other core
    s_rtcOk = true;
    Serial.println("Clock set from the on-board RTC");
  } else {
    Serial.println("On-board RTC not set yet (waiting for NTP)");
  }
}
#endif

// UTC now, from NTP if synced, otherwise from the RTC (Plus2). False if neither.
// seedRtc: only the monitor task may read the RTC chip (see rtcSeedOnce).
static bool utcNow(int64_t &utc, bool seedRtc = true)
{
  if (mTriggerUpdate != 0) {
    utc = (int64_t)(initialTime + (millis() - mTriggerUpdate) / 1000) - 3600L * s_appliedTimezone;
    return true;
  }
#ifdef M5STICK_C_PLUS2
  if (seedRtc) rtcSeedOnce();
  if (s_rtcOk) {
    __sync_synchronize();
    utc = s_rtcUtc + (int64_t)((millis() - s_rtcMillis) / 1000);
    return true;
  }
#endif
  return false;
}

// Summer-time offset in seconds for a UTC time (0 without AUTO_DST_EU)
static long dstOffsetAt(int64_t utc)
{
#ifdef AUTO_DST_EU
  return tu_euSummerTime(utc) ? 3600 : 0;
#else
  (void)utc;
  return 0;
#endif
}

long currentDstOffset(void)
{
  int64_t utc;
  return utcNow(utc, false) ? dstOffsetAt(utc) : 0; // also called from the web server task
}

// Local wall-clock time in seconds since 1970 (what the screens show).
static int64_t localNow()
{
  int64_t utc;
  if (!utcNow(utc)) // not set yet: elapsed time since boot (screens show "--")
    return (int64_t)(initialTime + (millis() - mTriggerUpdate) / 1000);
  return utc + 3600L * s_appliedTimezone + dstOffsetAt(utc);
}

// Display helpers: lets a screen show "--" instead of a made-up value
// (00:00 before the clock is set, the hard-coded default block height,
// a price of 0) until real data has arrived.
bool isTimeSynced(void)
{
  int64_t utc;
  return utcNow(utc);
}
bool isBlockHeightKnown(void) { return mHeightUpdate != 0; }
uint32_t getBlockHeightValue(void) { return mHeightUpdate != 0 ? s_blockHeight : 0; }

// Local date and time WITHOUT ever starting an NTP request (NTPClient is not
// thread safe and is driven by the monitor task): for the web server task.
bool getLocalClock(int &hour, int &minute, int &second, int &day, int &month, int &year)
{
  int64_t utc;
  if (!utcNow(utc, false)) // web server task: never touches the RTC chip itself
    return false;
  const int64_t now = utc + 3600L * s_appliedTimezone + dstOffsetAt(utc);
  const int64_t days = tu_daysOf(now);
  const int64_t secs = now - days * 86400;
  hour = secs / 3600;
  minute = secs % 3600 / 60;
  second = secs % 60;
  unsigned m, d;
  tu_civilFromDays(days, year, m, d);
  month = m;
  day = d;
  return true;
}
unsigned int getBTCpriceValue(void) { return bitcoin_price; }

void getTime(unsigned long* currentHours, unsigned long* currentMinutes, unsigned long* currentSeconds){
  
  //Check if need an NTP call to check current time
  if((mTriggerUpdate == 0) || (millis() - mTriggerUpdate > UPDATE_PERIOD_h * 60 * 60 * 1000)){ //60 sec. * 60 min * 1000ms
    if(WiFi.status() == WL_CONNECTED) {
        // initialTime and mTriggerUpdate must always change together: taking
        // a new initialTime after a FAILED sync (old mTriggerUpdate kept)
        // counted the time since the last good sync twice, so the clock ran
        // up to several hours ahead until an NTP request succeeded again.
        if(timeClient.update()) { //NTP call to get current time
            mTriggerUpdate = millis();
            initialTime = timeClient.getEpochTime(); // Guarda la hora inicial (en segundos desde 1970)
            Serial.println("TimeClient NTPupdateTime");
#ifdef M5STICK_C_PLUS2
            if (plus2RtcWrite((int64_t)initialTime - 3600L * s_appliedTimezone))
                Serial.println("On-board RTC set from NTP");
#endif
        }
    }
  }

  const int64_t now = localNow();
  const int64_t secs = now - tu_daysOf(now) * 86400; // seconds into the day

  // convierte la hora actual en horas, minutos y segundos
  *currentHours = secs / 3600;
  *currentMinutes = secs % 3600 / 60;
  *currentSeconds = secs % 60;
}

// UTC now; false until the clock has been set (NTP, or the RTC on the Plus2).
// With sync (monitor task only) the NTP call is made here when it is due, as
// getTime() does.
bool getUtcTime(time_t &utc, bool sync){
  if (sync) {
    unsigned long h, m, s;
    getTime(&h, &m, &s);
  }
  int64_t u;
  if (!utcNow(u, sync)) return false; // sync == monitor task: the only one allowed to read the RTC chip
  utc = (time_t)u;
  return true;
}

String getDate(){
  int y; unsigned m, d;
  tu_civilFromDays(tu_daysOf(localNow()), y, m, d);

  char currentDate[20];
  snprintf(currentDate, sizeof(currentDate), "%02u/%02u/%04d", d, m, y);
  return String(currentDate);
}

String getTime(void){
  unsigned long currentHours, currentMinutes, currentSeconds;
  getTime(&currentHours, &currentMinutes, &currentSeconds);

  char LocalHour[24];
  snprintf(LocalHour, sizeof(LocalHour), "%02lu:%02lu", currentHours, currentMinutes);
  
  String mystring(LocalHour);
  return LocalHour;
}

enum EHashRateScale
{
  HashRateScale_99KH,
  HashRateScale_999KH,
  HashRateScale_9MH
};

static EHashRateScale s_hashrate_scale = HashRateScale_99KH;
static uint32_t s_skip_first = 3;
static double s_top_hashrate = 0.0;

static std::list<double> s_hashrate_avg_list;
static double s_hashrate_summ = 0.0;
static uint8_t s_hashrate_recalc = 0;

String getCurrentHashRate(unsigned long mElapsed)
{
  double hashrate = (double)elapsedKHs * 1000.0 / (double)mElapsed;

  s_hashrate_summ += hashrate;
  s_hashrate_avg_list.push_back(hashrate);
  if (s_hashrate_avg_list.size() > 10)
  {
    s_hashrate_summ -= s_hashrate_avg_list.front();
    s_hashrate_avg_list.pop_front();
  }

  ++s_hashrate_recalc;
  if (s_hashrate_recalc == 0)
  {
    s_hashrate_summ = 0.0;
    for (auto itt = s_hashrate_avg_list.begin(); itt != s_hashrate_avg_list.end(); ++itt)
      s_hashrate_summ += *itt;
  }

  double avg_hashrate = s_hashrate_summ / (double)s_hashrate_avg_list.size();
  if (avg_hashrate < 0.0)
    avg_hashrate = 0.0;

  if (s_skip_first > 0)
  {
    s_skip_first--;
  } else
  {
    if (avg_hashrate > s_top_hashrate)
    {
      s_top_hashrate = avg_hashrate;
      if (avg_hashrate > 999.9)
        s_hashrate_scale = HashRateScale_9MH;
      else if (avg_hashrate > 99.9)
        s_hashrate_scale = HashRateScale_999KH;
    }
  }

  switch (s_hashrate_scale)
  {
    case HashRateScale_99KH:
      return String(avg_hashrate, 2);
    case HashRateScale_999KH:
      return String(avg_hashrate, 1);
    default:
      // No room for a decimal from 1 MH/s up, but a miner that hovers around
      // it keeps the decimal whenever it reads below.
      if (avg_hashrate < 999.95)
        return String(avg_hashrate, 1);
      return String((int)avg_hashrate );
  }
}

mining_data getMiningData(unsigned long mElapsed)
{
  mining_data data;

  char best_diff_string[16] = {0};
  suffix_string(getBestDiff(), best_diff_string, 16, 0);

  char timeMining[24] = {0}; // [15] overflowed from 10000 days
  uint64_t tm = upTime;
  int secs = tm % 60;
  tm /= 60;
  int mins = tm % 60;
  tm /= 60;
  int hours = tm % 24;
  int days = tm / 24;
  snprintf(timeMining, sizeof(timeMining), "%01d  %02d:%02d:%02d", days, hours, mins, secs);

  data.completedShares = shares;
  data.totalMHashes = String((unsigned long)(getTotalHashes() / 1000000));
  data.totalKHashes = String((unsigned long long)totalKHashes);
  data.currentHashRate = getCurrentHashRate(mElapsed);
  data.templates = templates;
  data.bestDiff = best_diff_string;
  data.timeMining = timeMining;
  data.valids = valids;
  data.temp = String(temperatureRead(), 0);
  data.currentTime = getTime();

  return data;
}

clock_data getClockData(unsigned long mElapsed)
{
  clock_data data;

  data.completedShares = shares;
  data.totalKHashes = String((unsigned long long)totalKHashes);
  data.currentHashRate = getCurrentHashRate(mElapsed);
  data.btcPrice = getBTCprice();
  data.blockHeight = getBlockHeight();
  data.currentTime = getTime();
  data.currentDate = getDate();

  return data;
}

clock_data_t getClockData_t(unsigned long mElapsed)
{
  clock_data_t data;

  data.valids = valids;
  data.currentHashRate = getCurrentHashRate(mElapsed);
  getTime(&data.currentHours, &data.currentMinutes, &data.currentSeconds);

  return data;
}

coin_data getCoinData(unsigned long mElapsed)
{
  coin_data data;

  updateGlobalData(); // Update gData vars asking mempool APIs

  data.completedShares = shares;
  data.totalKHashes = String((unsigned long long)totalKHashes);
  data.currentHashRate = getCurrentHashRate(mElapsed);
  data.btcPrice = getBTCprice();
  data.currentTime = getTime();
#ifdef SCREEN_FEES_ENABLE
  data.hourFee = String(gData.hourFee);
  data.fastestFee = String(gData.fastestFee);
  data.economyFee = String(gData.economyFee);
  data.minimumFee = String(gData.minimumFee);
#endif
  data.halfHourFee = String(gData.halfHourFee) + " sat/vB";
  data.netwrokDifficulty = gData.difficulty;
  data.globalHashRate = gData.globalHash;
  data.blockHeight = getBlockHeight();

  unsigned long currentBlock = data.blockHeight.toInt();
  unsigned long remainingBlocks = (((currentBlock / HALVING_BLOCKS) + 1) * HALVING_BLOCKS) - currentBlock;
  data.progressPercent = (HALVING_BLOCKS - remainingBlocks) * 100 / HALVING_BLOCKS;
  data.remainingBlocks = String(remainingBlocks) + " BLOCKS";

  return data;
}

// Which heliospool region server our stratum connection landed on. The stats
// API is per-region: a user only has live data on the server they mine to,
// is unknown (404) on servers they never used, and keeps stale data (zero
// workers) on servers they used in the past. The generic btc.heliospool.com
// hostname is a geo load balancer, so when the hostname doesn't name a
// region, match the address the stratum connection actually uses against the
// regional stratum hostnames. (Not a fresh lookup of the pool hostname: the
// balancer can hand out a different region from one lookup to the next, and
// the stats then come from the wrong server for the whole uptime.)
extern IPAddress serverIP; // mining.cpp; 1.1.1.1 until the pool has been resolved
static const char* const HELIOS_REGIONS[] = {"eu-west", "us-west", "ca-east", "au-south"};
#define HELIOS_REGION_COUNT (sizeof(HELIOS_REGIONS) / sizeof(HELIOS_REGIONS[0]))
static String heliosRegion;          // region in use, empty = not worked out yet
static IPAddress heliosRegionIp;     // stratum address it was worked out for
static bool heliosRegionInHost = false; // the pool hostname names it: nothing to work out
static bool heliosRegionSure = false;   // matched by address, or seen with live workers
static uint8_t heliosMisses = 0;        // consecutive answers from a wrong server
#define HELIOS_RETRY_ms 30000           // how soon to ask the next region after one

// True when the region has to be worked out (again): never done, or the
// stratum connection has since moved to another address.
static bool heliosRegionStale(void) {
    if (heliosRegionInHost) return false;
    return heliosRegion.isEmpty() || heliosRegionIp != serverIP;
}

String getHeliosRegion(void) {
    if (!heliosRegionStale()) return heliosRegion;
    const String& host = activePoolAddress();
    const char* named = NULL;
    for (const char* r : HELIOS_REGIONS) {
        if (host.indexOf(r) >= 0) named = r;
    }
    if (!named && host.indexOf("heliospool.eu") >= 0) named = "eu-west";
    if (!named && host.indexOf("heliospool.asia") >= 0) named = "au-south";
    if (named) {
        heliosRegionInHost = true;
        return heliosRegion = named;
    }
    const IPAddress poolIp = serverIP;
    if (WiFi.status() == WL_CONNECTED && poolIp != IPAddress(1, 1, 1, 1)) {
        for (const char* r : HELIOS_REGIONS) {
            IPAddress regionIp;
            if (WiFi.hostByName((String("btc-") + r + ".heliospool.com").c_str(), regionIp) && regionIp == poolIp) {
                Serial.printf("heliospool region: %s\n", r);
                heliosRegionIp = poolIp;
                heliosRegionSure = true;
                return heliosRegion = r;
            }
        }
    }
    return "eu-west"; // best guess; not cached so a later call can retry the lookup
}

// Judge a stats answer. If it came from the wrong region's server -- the user
// is unknown there, or (when the region is only a guess) has no live workers
// there -- switch poolAPIUrl to the next region, so a wrong region corrects
// itself instead of sticking. Returns true if the next request should follow
// soon instead of after the usual UPDATE_POOL_min. Once every region has been
// asked without finding live workers (a new wallet has none anywhere yet),
// start over from the address match at the usual pace: every request pauses
// the HW miner for its TLS handshake.
static bool heliosJudgeAnswer(int httpCode, int workers) {
    if (heliosRegionInHost) return false;
    if (httpCode == HTTP_CODE_OK && workers > 0) {
        heliosRegionSure = true;
        heliosMisses = 0;
        return false;
    }
    const bool wrongServer = httpCode == HTTP_CODE_NOT_FOUND || (httpCode == HTTP_CODE_OK && !heliosRegionSure);
    if (!wrongServer) return false;
    if (heliosMisses >= HELIOS_REGION_COUNT - 1) {
        heliosRegion = "";
        heliosMisses = 0;
        return false;
    }
    size_t i = 0;
    while (i < HELIOS_REGION_COUNT && heliosRegion != HELIOS_REGIONS[i]) i++;
    heliosRegion = HELIOS_REGIONS[(i + 1) % HELIOS_REGION_COUNT];
    heliosRegionIp = serverIP;
    heliosRegionSure = false;
    {
        std::lock_guard<std::mutex> lock(s_poolApiMutex);
        poolAPIUrl = "https://api-btc-" + heliosRegion + ".heliospool.com/users/";
    }
    Serial.printf("heliospool region: trying %s\n", heliosRegion.c_str());
    heliosMisses++;
    return true;
}

String getPoolAPIUrl(void) {
    std::lock_guard<std::mutex> lock(s_poolApiMutex);
    // The pool being mined on: the main one, or the backup while it is down
    const String& pool = activePoolAddress();
    const int poolPort = activePoolPort();
    int style = POOL_API_PUBLICPOOL;
    String url = String(getPublicPool);
    if (pool == "public-pool.io") {
        url = "https://public-pool.io:40557/api/client/";
    }
    else if (pool.indexOf("hmpool.io") >= 0) {
        // hmpool runs one pool per coin (btc.hmpool.io, bch.hmpool.io,
        // digi.hmpool.io), each with its own stats API: asking the btc one
        // about a DGB address answers 200 with an empty worker list. Within
        // a coin the regional stratum endpoints (eu.btc.hmpool.io,
        // eu.digi.hmpool.io, ...) share one set of stats -- confirmed live
        // on both btc and digi -- so the region prefix is dropped.
        String coin = "btc";
        const int coinEnd = pool.indexOf(".hmpool.io");
        if (coinEnd > 0)
            coin = pool.substring(pool.lastIndexOf('.', coinEnd - 1) + 1, coinEnd);
        if (coin == "btc") {
            // GET https://btc.hmpool.io/api/miner/<address>: hmpool's own
            // schema. It groups workers by name, so several miners sharing
            // one worker name count as one.
            url = "https://btc.hmpool.io/api/miner/";
            style = POOL_API_HMPOOL;
        } else {
            // The digi and bch pools also answer the public-pool schema at
            // /api/client/<address> (btc has no such route: 404). It lists
            // one entry per live connection, so every miner is counted even
            // when they share a worker name, and it is a fraction of the
            // size of /api/miner.
            url = "https://" + coin + ".hmpool.io/api/client/";
        }
    }
    else if (pool.indexOf("heliospool.") >= 0) {
        // heliospool.com/heliospool-api: each region's server publishes its
        // own stats at https://api-{coin}-{region}.heliospool.com/users/<address>
        // (the old btc.heliospool.{com,eu,asia}/api/users path is gone -- those
        // hosts are now stratum-only and 443 just times out).
        url = "https://api-btc-" + getHeliosRegion() + ".heliospool.com/users/";
        style = POOL_API_CKPOOL;
    }
    else {
        if (pool == "pool.nerdminers.org") {
            // Runs a ckpool-solo fork (golden-guy/ckpool-solo@nerdminer_v2);
            // its per-user JSON at this same /users/<address> path uses the
            // same hashrate1hr/workers/bestever fields as heliospool.
            url = "https://pool.nerdminers.org/users/";
            style = POOL_API_CKPOOL;
        }
        else {
            switch (poolPort) {
                case 3333:
                    if (pool == "pool.sethforprivacy.com")
                        url = "https://pool.sethforprivacy.com/api/client/";
                    if (pool == "pool.solomining.de")
                        url = "https://pool.solomining.de/api/client/";
                    // Add more cases for other addresses with port 3333 if needed
                    break;
                case 2018:
                    // Local instance of public-pool.io on Umbrel or Start9
                    url = "http://" + pool + ":2019/api/client/";
                    break;
                default:
                    url = String(getPublicPool);
                    break;
            }
        }
    }
    poolApiStyle = style;
    poolAPIUrl = url;
    return url;
}

pool_data getPoolData(void){
    //pool_data pData;    
    if((mPoolUpdate == 0) || (millis() - mPoolUpdate > UPDATE_POOL_min * 60 * 1000)){      
        if (WiFi.status() != WL_CONNECTED) return pData;
        //Make first API call to get global hash and current difficulty
        // Explicit, freshly-constructed WiFiClientSecure per call (rather
        // than letting HTTPClient manage an internal one implicitly) --
        // some pool servers' TLS 1.3 setups were consistently rejecting
        // the 2nd+ call in a session with a fatal alert while the 1st
        // always succeeded, which points at stale session/socket state
        // being reused across calls. A fresh client + explicit stop()
        // avoids that.
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.setTimeout(10000);
        try {
          String btcWallet = Settings.BtcWallet;
          // Serial.println(btcWallet);
          if (btcWallet.indexOf(".")>0) btcWallet = btcWallet.substring(0,btcWallet.indexOf("."));
          int apiStyle = POOL_API_PUBLICPOOL;
#ifdef SCREEN_WORKERS_ENABLE
          // setup_monitor() runs before the stratum connection exists (and
          // may not have run at all yet), and that connection can move to
          // another region's server later: work the URL out again whenever
          // it is missing or heliospool's region isn't current.
          const bool heliosPool = activePoolAddress().indexOf("heliospool.") >= 0;
          String apiUrl;
          {
              std::lock_guard<std::mutex> lock(s_poolApiMutex);
              apiUrl = poolAPIUrl;
              apiStyle = poolApiStyle;
          }
          if (apiUrl.isEmpty() || (heliosPool && heliosRegionStale())) {
              apiUrl = getPoolAPIUrl();
              std::lock_guard<std::mutex> lock(s_poolApiMutex);
              apiStyle = poolApiStyle;
          }
          Serial.println("Pool API : " + apiUrl+btcWallet);
          http.begin(client, apiUrl+btcWallet);
#else
          http.begin(client, String(getPublicPool)+btcWallet);
#endif
          // The TLS handshake inside GET() uses mbedTLS's own hardware SHA
          // acceleration (SHA384/512), which does NOT block against mining's
          // esp_sha_lock_engine(SHA2_256) -- confirmed via a crash backtrace
          // showing abort() inside sha_hal_read_digest() when both ran
          // concurrently. g_hwShaMutex closes that gap; see mining.h.
          int httpCode;
          {
            HW_SHA_REQUEST();
            g_hwShaMutex.lock();
            httpCode = http.GET();
            g_hwShaMutex.unlock();
          }
          if (httpCode == HTTP_CODE_OK) {
              String payload = http.getString();
              // Serial.println(payload);
              double temp;
              if (apiStyle == POOL_API_CKPOOL) {
                // ckpool-solo family (heliospool, nerdminers.org):
                // { "bestever": N, "workers": N, "hashrate1hr": "1041G", ... }
                // "workers" here is already a count, and hashrate1hr is a
                // pre-formatted SI string (e.g. "1041G"), not a raw number.
                StaticJsonDocument<300> filter;
                filter["bestever"] = true;
                filter["workers"] = true;
                filter["hashrate1hr"] = true;
                StaticJsonDocument<1024> doc;
                deserializeJson(doc, payload, DeserializationOption::Filter(filter));
                if (doc.containsKey("workers")) pData.workersCount = doc["workers"].as<int>();
                if (doc.containsKey("hashrate1hr")) pData.workersHash = doc["hashrate1hr"].as<String>();
                if (doc.containsKey("bestever")) {
                  temp = doc["bestever"].as<double>();
                  char best_diff_string[16] = {0};
                  suffix_string(temp, best_diff_string, 16, 0);
                  pData.bestDifficulty = String(best_diff_string);
                }
                doc.clear();
              } else if (apiStyle == POOL_API_HMPOOL) {
                // hmpool: { "best_share_difficulty": N, "workers": [{"active": B, "hashrate": N}, ...] }
                // (hmpool's top-level "best_difficulty" is the current
                // VarDiff target, not a best-ever share, so it's not used here.)
                // "workers" also lists ones that have gone offline
                // ("active": false, hashrate 0); only live ones are counted.
                StaticJsonDocument<300> filter;
                filter["best_share_difficulty"] = true;
                filter["workers"][0]["active"] = true;
                filter["workers"][0]["hashrate"] = true;
                StaticJsonDocument<2048> doc;
                deserializeJson(doc, payload, DeserializationOption::Filter(filter));
                const JsonArray& workers = doc["workers"].as<JsonArray>();
                int activeWorkers = 0;
                float totalhashs = 0;
                for (const JsonObject& worker : workers) {
                  if (!(worker["active"] | true)) continue;
                  activeWorkers++;
                  totalhashs += worker["hashrate"].as<double>();
                }
                pData.workersCount = activeWorkers;
                char totalhashs_s[16] = {0};
                suffix_string(totalhashs, totalhashs_s, 16, 0);
                pData.workersHash = String(totalhashs_s);
                if (doc.containsKey("best_share_difficulty")) {
                  temp = doc["best_share_difficulty"].as<double>();
                  char best_diff_string[16] = {0};
                  suffix_string(temp, best_diff_string, 16, 0);
                  pData.bestDifficulty = String(best_diff_string);
                }
                doc.clear();
              } else {
                // public-pool.io, sethforprivacy, solomining, hmpool (digi, bch)
                StaticJsonDocument<300> filter;
                filter["bestDifficulty"] = true;
                filter["workersCount"] = true;
                filter["workers"][0]["sessionId"] = true;
                filter["workers"][0]["hashRate"] = true;
                StaticJsonDocument<2048> doc;
                deserializeJson(doc, payload, DeserializationOption::Filter(filter));
                //Serial.println(serializeJsonPretty(doc, Serial));
                if (doc.containsKey("workersCount")) pData.workersCount = doc["workersCount"].as<int>();
                const JsonArray& workers = doc["workers"].as<JsonArray>();
                float totalhashs = 0;
                for (const JsonObject& worker : workers) {
                  totalhashs += worker["hashRate"].as<double>();
                  /* Serial.print(worker["sessionId"].as<String>()+": ");
                  Serial.print(" - "+worker["hashRate"].as<String>()+": ");
                  Serial.println(totalhashs); */
                }
                char totalhashs_s[16] = {0};
                suffix_string(totalhashs, totalhashs_s, 16, 0);
                pData.workersHash = String(totalhashs_s);

                if (doc.containsKey("bestDifficulty")) {
                temp = doc["bestDifficulty"].as<double>();
                char best_diff_string[16] = {0};
                suffix_string(temp, best_diff_string, 16, 0);
                pData.bestDifficulty = String(best_diff_string);
                }
                doc.clear();
              }
              mPoolUpdate = millis();
#ifdef SCREEN_WORKERS_ENABLE
              if (heliosPool && heliosJudgeAnswer(httpCode, pData.workersCount))
                  mPoolUpdate = millis() - (UPDATE_POOL_min * 60 * 1000 - HELIOS_RETRY_ms);
#endif
              Serial.println("\n####### Pool Data OK!");
          } else {
              Serial.println("\n####### Pool Data HTTP Error!");
              Serial.println(httpCode);
              String payload = http.getString();
              Serial.println(payload);
              // Must still back off on failure, or a persistently-failing
              // pool API gets hammered every ~2s instead of once a minute —
              // enough to trip some pools' WAF/anti-abuse rate limiting and
              // turn a transient failure into a permanent one.
              mPoolUpdate = millis();
#ifdef SCREEN_WORKERS_ENABLE
              if (heliosPool && heliosJudgeAnswer(httpCode, 0))
                  mPoolUpdate = millis() - (UPDATE_POOL_min * 60 * 1000 - HELIOS_RETRY_ms);
#endif
              pData.bestDifficulty = "P";
              pData.workersHash = "E";
              pData.workersCount = 0;
              http.end();
              client.stop();
              return pData;
          }
          http.end();
          client.stop();
        } catch(...) {
          Serial.println("####### Pool Error!");
          mPoolUpdate = millis();
          pData.bestDifficulty = "P";
          pData.workersHash = "Error";
          pData.workersCount = 0;
          http.end();
          client.stop();
          return pData;
        }
    }
    return pData;
}

void requestCurrencyChange(const char *code) {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    s_newCurrency = code;
}

String currencySetting(void) {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    return s_newCurrency ? String(s_newCurrency) : Settings.Currency;
}

void poolSettingsChanged(void) {
    s_poolChanged = true;
}

void applyPendingSettings(void) {
    const int tz = Settings.Timezone;
    if (tz != s_appliedTimezone) {
        // initialTime was taken with the old offset in it: shift it, so the
        // clock moves now rather than at the next NTP sync (hours away).
        if (initialTime != 0)
            initialTime += (long)(tz - s_appliedTimezone) * 3600;
        timeClient.setTimeOffset(3600 * tz);
        s_appliedTimezone = tz;
    }

    bool currencyChanged = false;
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        if (s_newCurrency) {
            Settings.Currency = s_newCurrency;
            s_newCurrency = nullptr;
            currencyChanged = true;
        }
    }
    if (currencyChanged) {
        bitcoin_price = 0; // the last figure is in the old currency
        mBTCUpdate = 0;    // fetch the price again next time it's shown
        mBTCTry = 0;       // ...right away, not after the retry delay
    }

    if (s_poolChanged.exchange(false)) {
        // Work the stats URL out again for the new pool and fetch at once;
        // until then show nothing rather than the old pool's numbers.
        {
            std::lock_guard<std::mutex> lock(s_poolApiMutex);
            poolAPIUrl = "";
            poolApiStyle = POOL_API_PUBLICPOOL;
        }
        heliosRegion = "";
        heliosRegionInHost = false;
        heliosRegionSure = false;
        heliosMisses = 0;
        pData = pool_data();
        mPoolUpdate = 0;
    }
}

// ---------------------------------------------------------------------------
// BTC price and block height for the live web monitor. It shows the cached
// values only (it never starts a fetch itself), and the screens fetch them
// only on the pages that show them: a board left on its miner page never had
// any. Each HTTPS fetch pauses the HW miner (~1.5 s), so the web page asks
// for them sparingly: once when it is opened, then once an hour while it
// stays open. getBTCprice() / getBlockHeight() still skip the request if a
// screen fetched within their usual period. Monitor task only.
// -D NO_WEB_PRICE leaves the fetch out: on a board whose screens never fetch
// anything (T-QT), it would link the HTTPS/TLS stack (~127 KB) into an app
// partition that has no room for it.
// ---------------------------------------------------------------------------
#define WEB_PRICE_REFRESH_ms (60UL * 60 * 1000) // while the page stays open
#define WEB_WATCHED_ms (60UL * 1000)            // no poll for this long: closed

static std::atomic<bool> s_webOpened{false};
static std::atomic<uint32_t> s_webSeenMs{0};

void noteWebMonitorOpened(void)
{
    s_webSeenMs = millis();
    s_webOpened = true;
}

void noteWebMonitorPoll(void)
{
    s_webSeenMs = millis();
}

void webMonitorDataTick(void)
{
    static uint32_t lastWebFetch = 0;
    const uint32_t now = millis();
    bool due = s_webOpened.exchange(false);
    if (!due) {
        const uint32_t seen = s_webSeenMs;
        due = seen != 0 && now - seen < WEB_WATCHED_ms && now - lastWebFetch >= WEB_PRICE_REFRESH_ms;
    }
    if (!due)
        return;
    lastWebFetch = now;
#ifndef NO_WEB_PRICE
    getBTCprice();
    getBlockHeight();
#endif
}
