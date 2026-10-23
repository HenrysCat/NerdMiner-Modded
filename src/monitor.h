#ifndef MONITOR_API_H
#define MONITOR_API_H

#include <Arduino.h>

// Monitor states
#define SCREEN_MINING   0
#define SCREEN_CLOCK    1
#define SCREEN_GLOBAL   2
#define NO_SCREEN       3   //Used when board has no TFT

//Time update period
#define UPDATE_PERIOD_h   5

//API BTC price (Update to USDT cus it's more liquidity and flow price updade)   

//#define getBTCAPI "https://api.coindesk.com/v1/bpi/currentprice.json" -- doesn't work anymore
//#define getBTCAPI "https://api.blockchain.com/v3/exchange/tickers/BTC-USDT" -- updates infrequently
#define getBTCAPI "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=" // + currency code

#ifndef UPDATE_BTC_min // a board can stretch it from platformio.ini
#define UPDATE_BTC_min   1
#endif

//API Block height
#define getHeightAPI "https://mempool.space/api/blocks/tip/height"
#ifndef UPDATE_Height_min // a board can stretch it from platformio.ini
#define UPDATE_Height_min 2
#endif

//APIs Global Stats
#define getGlobalHash "https://mempool.space/api/v1/mining/hashrate/3d"
#define getDifficulty "https://mempool.space/api/v1/difficulty-adjustment"
#define getFees "https://mempool.space/api/v1/fees/recommended"
#ifndef UPDATE_Global_min // a board can stretch it from platformio.ini
#define UPDATE_Global_min 2
#endif

//API public-pool.io
// https://public-pool.io:40557/api/client/btcString
#define getPublicPool "https://public-pool.io:40557/api/client/" // +btcString
// Each refresh holds g_hwShaMutex for the whole HTTPS request (~1.5 s with the
// TLS handshake), which pauses the hardware miner. At 1 min that was ~2.4% of
// all mining time; 5 min costs ~0.5% and pool stats barely change in 5 min.
// Pool stats (workers, pool rate, pool best): each fetch is an HTTPS request
// that pauses the HW miner ~1.5 s, a dip a 10 s hashrate reading shows as -13%.
#ifndef UPDATE_POOL_min
#define UPDATE_POOL_min   15
#endif

#define NEXT_HALVING_EVENT 1050000 //840000
#define HALVING_BLOCKS 210000

enum NMState {
  NM_waitingConfig,
  NM_Connecting,
  NM_hashing
};

typedef struct{
  uint8_t screen;
  bool rotation;
  NMState NerdStatus;
}monitor_data;

typedef struct{
  String globalHash; //hexahashes
  String currentBlock;
  String difficulty;
  String blocksHalving;
  float progressPercent;
  int remainingBlocks;
  int halfHourFee;
#ifdef NERDMINER_T_HMI
  int fastestFee;
  int hourFee;
  int economyFee;
  int minimumFee;
#endif
}global_data;

typedef struct {
  String completedShares;
  String totalMHashes;
  String totalKHashes;
  String currentHashRate;
  String templates;
  String bestDiff;
  String timeMining;
  String valids;
  String temp;
  String currentTime;
}mining_data;

typedef struct {
  String completedShares;
  String totalKHashes;
  String currentHashRate;
  String btcPrice;
  String blockHeight;
  String currentTime;  
  String currentDate;
}clock_data;

typedef struct {
  String currentHashRate;
  String valids;
  unsigned long currentHours;
  unsigned long currentMinutes;
  unsigned long currentSeconds;
}clock_data_t;

typedef struct {
  String completedShares;
  String totalKHashes;
  String currentHashRate;
  String btcPrice;
  String currentTime;
  String halfHourFee;
#ifdef NERDMINER_T_HMI
  String hourFee;
  String fastestFee;
  String economyFee;
  String minimumFee;
#endif
  String netwrokDifficulty;
  String globalHashRate;
  String blockHeight;
  float progressPercent;
  String remainingBlocks;
}coin_data;

typedef struct{
  int workersCount;       // Workers count, how many nerdminers using your address
  String workersHash;     // Workers Total Hash Rate
  String bestDifficulty;  // Your miners best difficulty
}pool_data;

void setup_monitor(void);

mining_data getMiningData(unsigned long mElapsed);
clock_data getClockData(unsigned long mElapsed);
coin_data getCoinData(unsigned long mElapsed);
pool_data getPoolData(void);

clock_data_t getClockData_t(unsigned long mElapsed);
String getPoolAPIUrl(void);
bool getUtcTime(time_t &utc, bool sync = false);

// True once real data is in (see monitor.cpp); screens show "--" until then.
bool isTimeSynced(void);
bool isBlockHeightKnown(void);
unsigned int getBTCpriceValue(void); // raw price in the selected currency, 0 = unknown
uint32_t getBlockHeightValue(void);  // last block height fetched, 0 = unknown (any task)
bool getLocalClock(int &hour, int &minute, int &second, int &day, int &month, int &year); // no NTP call (any task)
long currentDstOffset(void);         // summer-time offset in force now, seconds (AUTO_DST_EU)

// Settings changed on the LAN settings page without a restart. The monitor
// task (screens, price, pool stats) applies them itself, between screen
// refreshes, in applyPendingSettings(): a new Settings.Timezone, a currency
// handed over with requestCurrencyChange() (a kCurrencies code), and, after
// poolSettingsChanged() from the stratum task, the pool stats lookup.
// currencySetting() is the currency in use or the one waiting.
void requestCurrencyChange(const char *code);
String currencySetting(void);
void poolSettingsChanged(void);
void applyPendingSettings(void);

// Live web monitor (webConfig.cpp): the page was loaded / polled /api/status.
// webMonitorDataTick() (monitor task, once a second) then fetches the BTC price
// and block height once when the page opens and once an hour while it stays
// open; nothing while nobody is watching.
void noteWebMonitorOpened(void);
void noteWebMonitorPoll(void);
void webMonitorDataTick(void);

#endif //MONITOR_API_H
