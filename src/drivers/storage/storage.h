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
#ifdef AUTO_DST_EU
#define DEFAULT_TIMEZONE	1	// winter offset, summer time added automatically
#else
#define DEFAULT_TIMEZONE	2
#endif
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
#define LED_MODE_OFF		0
#define LED_MODE_STATUS		1	// red at start-up, blue while connecting, blinks the colour while mining
#define LED_MODE_STEADY		2	// the colour, always on
#define DEFAULT_LEDMODE		LED_MODE_STATUS
#define DEFAULT_LEDCOLOR	0xFF0000	// 0xRRGGBB
#define DEFAULT_LEDBRIGHTNESS	255
#define DEFAULT_LEDSCREENOFF	false
#define DEFAULT_WEBPASSWORD	DEFAULT_WIFIPW	// LAN settings page login (user admin)

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
#define JSON_SPIFFS_KEY_LEDMODE		"ledMode"
#define JSON_SPIFFS_KEY_LEDCOLOR	"ledColor"
#define JSON_SPIFFS_KEY_LEDBRIGHTNESS	"ledBrightness"
#define JSON_SPIFFS_KEY_LEDSCREENOFF	"ledScreenOff"
#define JSON_SPIFFS_KEY_BACKUPPOOLURL	"backupPoolString"
#define JSON_SPIFFS_KEY_BACKUPPOOLPORT	"backupPortNumber"
#define JSON_SPIFFS_KEY_BACKUPPOOLPASS	"backupPoolPassword"
#define JSON_SPIFFS_KEY_WEBPASSWORD	"webPassword"

// settings
struct TSettings
{
	String WifiSSID{ DEFAULT_SSID };
	String WifiPW{ DEFAULT_WIFIPW };
	String PoolAddress{ DEFAULT_POOLURL };
	char BtcWallet[80]{ DEFAULT_WALLETID };
	char PoolPassword[80]{ DEFAULT_POOLPASS };
	int PoolPort{ DEFAULT_POOLPORT };
	// Used while the pool above is down (same wallet); no address = none
	String BackupPoolAddress{ "" };
	int BackupPoolPort{ DEFAULT_POOLPORT };
	char BackupPoolPassword[80]{ DEFAULT_POOLPASS };
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
	// Boards with an RGB LED (CYD)
	int ledMode{ DEFAULT_LEDMODE };	// LED_MODE_*
	uint32_t ledColor{ DEFAULT_LEDCOLOR };	// 0xRRGGBB
	int ledBrightness{ DEFAULT_LEDBRIGHTNESS };	// 1-255
	bool ledScreenOff{ DEFAULT_LEDSCREENOFF };	// LED off while the screen is off
	String WebPassword{ DEFAULT_WEBPASSWORD };	// LAN settings page login, never empty
};

#endif // _STORAGE_H_