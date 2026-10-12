#ifndef _STORAGE_H_
#define _STORAGE_H_

#include <Arduino.h>

// config files

// default settings
#ifndef HAN
#define DEFAULT_SSID		"NerdMinerAP"
#else
#define DEFAULT_SSID		"HanSoloAP"
#endif
#define DEFAULT_WIFIPW		"MineYourCoins"
#define DEFAULT_POOLURL		"public-pool.io"
#define DEFAULT_POOLPASS	"x"
#define DEFAULT_WALLETID	"yourBtcAddress"
#define DEFAULT_POOLPORT	3333
#define DEFAULT_TIMEZONE	2
#define DEFAULT_SAVESTATS	false
#define DEFAULT_INVERTCOLORS	false
#define DEFAULT_BRIGHTNESS	250
#define DEFAULT_FLIPDISPLAY	false
#define DEFAULT_CURRENCY	"usd"
#define DEFAULT_STATUSLED	true
#define DEFAULT_SCREENTIMEOUT	0	// minutes, 0 = never
#define DEFAULT_NIGHTDIM	false
#define DEFAULT_NIGHTBRIGHTNESS	40	// backlight duty 1-255
#define DEFAULT_NIGHTFADE	60	// minutes, half before and half after sunset / sunrise

// JSON config files
#define JSON_CONFIG_FILE	"/config.json"

// JSON config file SD card (for user interaction, readme.md)
#define JSON_KEY_SSID		"SSID"
#define JSON_KEY_PASW		"WifiPW"
#define JSON_KEY_POOLURL	"PoolUrl"
#define JSON_KEY_POOLPASS	"PoolPassword"
#define JSON_KEY_WALLETID	"BtcWallet"
#define JSON_KEY_POOLPORT	"PoolPort"
#define JSON_KEY_TIMEZONE	"Timezone"
#define JSON_KEY_STATS2NV	"SaveStats"
#define JSON_KEY_INVCOLOR	"invertColors"
#define JSON_KEY_BRIGHTNESS	"Brightness"

// JSON config file SPIFFS (different for backward compatibility with existing devices)
#define JSON_SPIFFS_KEY_POOLURL		"poolString"
#define JSON_SPIFFS_KEY_POOLPORT	"portNumber"
#define JSON_SPIFFS_KEY_POOLPASS	"poolPassword"
#define JSON_SPIFFS_KEY_WALLETID	"btcString"
#define JSON_SPIFFS_KEY_TIMEZONE	"gmtZone"
#define JSON_SPIFFS_KEY_STATS2NV	"saveStatsToNVS"
#define JSON_SPIFFS_KEY_INVCOLOR	"invertColors"
#define JSON_SPIFFS_KEY_BRIGHTNESS	"Brightness"
#define JSON_SPIFFS_KEY_FLIPDISPLAY	"flipDisplay"
#define JSON_SPIFFS_KEY_CURRENCY	"currency"
#define JSON_SPIFFS_KEY_STATUSLED	"statusLed"
#define JSON_SPIFFS_KEY_SCREENTIMEOUT	"screenTimeout"
#define JSON_SPIFFS_KEY_NIGHTDIM	"nightDim"
#define JSON_SPIFFS_KEY_NIGHTBRIGHTNESS	"nightBrightness"
#define JSON_SPIFFS_KEY_NIGHTFADE	"nightFade"
#define JSON_SPIFFS_KEY_LATITUDE	"latitude"
#define JSON_SPIFFS_KEY_LONGITUDE	"longitude"

// settings
struct TSettings
{
	String WifiSSID{ DEFAULT_SSID };
	String WifiPW{ DEFAULT_WIFIPW };
	String PoolAddress{ DEFAULT_POOLURL };
	char BtcWallet[80]{ DEFAULT_WALLETID };
	char PoolPassword[80]{ DEFAULT_POOLPASS };
	int PoolPort{ DEFAULT_POOLPORT };
	int Timezone{ DEFAULT_TIMEZONE };
	bool saveStats{ DEFAULT_SAVESTATS };
	bool invertColors{ DEFAULT_INVERTCOLORS };
	int Brightness{ DEFAULT_BRIGHTNESS };
	bool flipDisplay{ DEFAULT_FLIPDISPLAY };	// rotate display 180 degrees (USB on the left)
	String Currency{ DEFAULT_CURRENCY };	// fiat currency for the BTC price (CoinGecko vs_currency code)
	bool statusLed{ DEFAULT_STATUSLED };	// boards without a screen: status LED on/off
	int ScreenTimeout{ DEFAULT_SCREENTIMEOUT };	// minutes without a touch until the backlight goes off, 0 = never
	// Night dimming: the backlight fades from Brightness to NightBrightness across
	// NightFade minutes centred on sunset, and back at sunrise, at this location.
	bool nightDim{ DEFAULT_NIGHTDIM };
	int NightBrightness{ DEFAULT_NIGHTBRIGHTNESS };
	int NightFade{ DEFAULT_NIGHTFADE };
	float Latitude{ 0 };	// decimal degrees, north positive; 0,0 = not set
	float Longitude{ 0 };	// decimal degrees, east positive
};

#endif // _STORAGE_H_