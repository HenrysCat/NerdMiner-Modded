#include <Arduino.h>
#include <WiFi.h>
#include "mbedtls/md.h"
#include "HTTPClient.h"
#include <WiFiClientSecure.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <list>
#include "mining.h"
#include "utils.h"
#include "monitor.h"
#include "drivers/storage/storage.h"
#include "drivers/devices/device.h"
#include "currency.h"

extern uint32_t templates;
extern uint32_t hashes;
extern uint32_t Mhashes;
extern uint32_t totalKHashes;
extern uint32_t elapsedKHs;
extern uint64_t upTime;

extern uint32_t shares; // increase if blockhash has 32 bits of zeroes
extern uint32_t valids; // increased if blockhash <= targethalfshares

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


void setup_monitor(void){
    /******** TIME ZONE SETTING *****/

    timeClient.begin();
    
    // Adjust offset depending on your zone
    // GMT +2 in seconds (zona horaria de Europa Central)
    timeClient.setTimeOffset(3600 * Settings.Timezone);

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

void updateGlobalData(void){
    
    if((mGlobalUpdate == 0) || (millis() - mGlobalUpdate > UPDATE_Global_min * 60 * 1000)){
    
        if (WiFi.status() != WL_CONNECTED) return;
            
        //Make first API call to get global hash and current difficulty
        HTTPClient http;
        http.setTimeout(10000);
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

String getBlockHeight(void){
    
    if((mHeightUpdate == 0) || (millis() - mHeightUpdate > UPDATE_Height_min * 60 * 1000)){
    
        if (WiFi.status() != WL_CONNECTED) return current_block;
            
        HTTPClient http;
        http.setTimeout(10000);
        try {
        http.begin(getHeightAPI);
        int httpCode;
        { HW_SHA_REQUEST(); std::lock_guard<std::mutex> shaLock(g_hwShaMutex); httpCode = http.GET(); }

        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            payload.trim();

            current_block = payload;

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

    if((mBTCUpdate == 0) || (millis() - mBTCUpdate > UPDATE_BTC_min * 60 * 1000)){

        if (WiFi.status() != WL_CONNECTED) {
            snprintf(price_buffer, sizeof(price_buffer), "%s%u", cur.prefix, bitcoin_price);
            return String(price_buffer);
        }

        HTTPClient http;
        http.setTimeout(10000);
        bool priceUpdated = false;

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

void getTime(unsigned long* currentHours, unsigned long* currentMinutes, unsigned long* currentSeconds){
  
  //Check if need an NTP call to check current time
  if((mTriggerUpdate == 0) || (millis() - mTriggerUpdate > UPDATE_PERIOD_h * 60 * 60 * 1000)){ //60 sec. * 60 min * 1000ms
    if(WiFi.status() == WL_CONNECTED) {
        if(timeClient.update()) mTriggerUpdate = millis(); //NTP call to get current time
        initialTime = timeClient.getEpochTime(); // Guarda la hora inicial (en segundos desde 1970)
        Serial.print("TimeClient NTPupdateTime ");
    }
  }

  unsigned long elapsedTime = (millis() - mTriggerUpdate) / 1000; // Tiempo transcurrido en segundos
  unsigned long currentTime = initialTime + elapsedTime; // La hora actual

  // convierte la hora actual en horas, minutos y segundos
  *currentHours = currentTime % 86400 / 3600;
  *currentMinutes = currentTime % 3600 / 60;
  *currentSeconds = currentTime % 60;
}

String getDate(){
  
  unsigned long elapsedTime = (millis() - mTriggerUpdate) / 1000; // Tiempo transcurrido en segundos
  unsigned long currentTime = initialTime + elapsedTime; // La hora actual

  // Convierte la hora actual (epoch time) en una estructura tm
  struct tm *tm = localtime((time_t *)&currentTime);

  int year = tm->tm_year + 1900; // tm_year es el número de años desde 1900
  int month = tm->tm_mon + 1;    // tm_mon es el mes del año desde 0 (enero) hasta 11 (diciembre)
  int day = tm->tm_mday;         // tm_mday es el día del mes

  char currentDate[20];
  sprintf(currentDate, "%02d/%02d/%04d", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900);

  return String(currentDate);
}

String getTime(void){
  unsigned long currentHours, currentMinutes, currentSeconds;
  getTime(&currentHours, &currentMinutes, &currentSeconds);

  char LocalHour[10];
  sprintf(LocalHour, "%02d:%02d", currentHours, currentMinutes);
  
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
      return String((int)avg_hashrate );
  }
}

mining_data getMiningData(unsigned long mElapsed)
{
  mining_data data;

  char best_diff_string[16] = {0};
  suffix_string(best_diff, best_diff_string, 16, 0);

  char timeMining[15] = {0};
  uint64_t tm = upTime;
  int secs = tm % 60;
  tm /= 60;
  int mins = tm % 60;
  tm /= 60;
  int hours = tm % 24;
  int days = tm / 24;
  sprintf(timeMining, "%01d  %02d:%02d:%02d", days, hours, mins, secs);

  data.completedShares = shares;
  data.totalMHashes = Mhashes;
  data.totalKHashes = totalKHashes;
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
  data.totalKHashes = totalKHashes;
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
  data.totalKHashes = totalKHashes;
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
    const String& host = Settings.PoolAddress;
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
    int style = POOL_API_PUBLICPOOL;
    String url = String(getPublicPool);
    if (Settings.PoolAddress == "public-pool.io") {
        url = "https://public-pool.io:40557/api/client/";
    }
    else if (Settings.PoolAddress.indexOf("hmpool.io") >= 0) {
        // hmpool runs one pool per coin (btc.hmpool.io, bch.hmpool.io,
        // digi.hmpool.io), each with its own stats API: asking the btc one
        // about a DGB address answers 200 with an empty worker list. Within
        // a coin the regional stratum endpoints (eu.btc.hmpool.io,
        // eu.digi.hmpool.io, ...) share one set of stats -- confirmed live
        // on both btc and digi -- so the region prefix is dropped.
        String coin = "btc";
        const int coinEnd = Settings.PoolAddress.indexOf(".hmpool.io");
        if (coinEnd > 0)
            coin = Settings.PoolAddress.substring(Settings.PoolAddress.lastIndexOf('.', coinEnd - 1) + 1, coinEnd);
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
    else if (Settings.PoolAddress.indexOf("heliospool.") >= 0) {
        // heliospool.com/heliospool-api: each region's server publishes its
        // own stats at https://api-{coin}-{region}.heliospool.com/users/<address>
        // (the old btc.heliospool.{com,eu,asia}/api/users path is gone -- those
        // hosts are now stratum-only and 443 just times out).
        url = "https://api-btc-" + getHeliosRegion() + ".heliospool.com/users/";
        style = POOL_API_CKPOOL;
    }
    else {
        if (Settings.PoolAddress == "pool.nerdminers.org") {
            // Runs a ckpool-solo fork (golden-guy/ckpool-solo@nerdminer_v2);
            // its per-user JSON at this same /users/<address> path uses the
            // same hashrate1hr/workers/bestever fields as heliospool.
            url = "https://pool.nerdminers.org/users/";
            style = POOL_API_CKPOOL;
        }
        else {
            switch (Settings.PoolPort) {
                case 3333:
                    if (Settings.PoolAddress == "pool.sethforprivacy.com")
                        url = "https://pool.sethforprivacy.com/api/client/";
                    if (Settings.PoolAddress == "pool.solomining.de")
                        url = "https://pool.solomining.de/api/client/";
                    // Add more cases for other addresses with port 3333 if needed
                    break;
                case 2018:
                    // Local instance of public-pool.io on Umbrel or Start9
                    url = "http://" + Settings.PoolAddress + ":2019/api/client/";
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
          const bool heliosPool = Settings.PoolAddress.indexOf("heliospool.") >= 0;
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
