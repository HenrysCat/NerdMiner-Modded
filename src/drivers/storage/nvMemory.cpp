#include "nvMemory.h"

#ifdef NVMEM_SPIFFS

#include <SPIFFS.h>
#include <FS.h>
#include <ArduinoJson.h>

#include "../devices/device.h"
#include "storage.h"

nvMemory::nvMemory() : Initialized_(false){};

// Written first, then renamed over JSON_CONFIG_FILE once complete: opening the
// config itself with "w" emptied it at once, so a power cut during a save left
// no settings at all (the miner came back in its setup portal, then mined to
// the default address).
#define JSON_CONFIG_TMP JSON_CONFIG_FILE ".tmp"

// The config as JSON on the serial log, without the passwords in it.
static void printConfig(const JsonDocument &json)
{
    DynamicJsonDocument copy(json.memoryUsage() + 512);
    copy.set(json);
    for (const char *key : {JSON_SPIFFS_KEY_POOLPASS, JSON_SPIFFS_KEY_BACKUPPOOLPASS, JSON_SPIFFS_KEY_WEBPASSWORD,
                            JSON_SPIFFS_KEY_MQTTPASS})
    {
        const char *v = copy[key].as<const char *>();
        if (v && *v)
            copy[key] = "***";
    }
    serializeJsonPretty(copy, Serial);
    Serial.print('\n');
}

nvMemory::~nvMemory()
{
    if (Initialized_)
        SPIFFS.end();
};

/// @brief Save settings to config file on SPIFFS
/// @param TSettings* Settings to be saved.
/// @return true on success
bool nvMemory::saveConfig(TSettings* Settings)
{
    if (init())
    {
        // Save Config in JSON format
        Serial.println(F("SPIFS: Saving configuration."));

        // Create a JSON document (on the heap: it has outgrown a small stack)
        DynamicJsonDocument json(3072); // room for the MQTT fields too
        json[JSON_SPIFFS_KEY_POOLURL] = Settings->PoolAddress;
        json[JSON_SPIFFS_KEY_POOLPORT] = Settings->PoolPort;
        json[JSON_SPIFFS_KEY_POOLPASS] = Settings->PoolPassword;
        json[JSON_SPIFFS_KEY_WALLETID] = Settings->BtcWallet;
        json[JSON_SPIFFS_KEY_BACKUPPOOLURL] = Settings->BackupPoolAddress;
        json[JSON_SPIFFS_KEY_BACKUPPOOLPORT] = Settings->BackupPoolPort;
        json[JSON_SPIFFS_KEY_BACKUPPOOLPASS] = Settings->BackupPoolPassword;
        json[JSON_SPIFFS_KEY_WEBPASSWORD] = Settings->WebPassword;
        json[JSON_SPIFFS_KEY_MQTTENABLED] = Settings->MqttEnabled;
        json[JSON_SPIFFS_KEY_MQTTHOST] = Settings->MqttHost;
        json[JSON_SPIFFS_KEY_MQTTPORT] = Settings->MqttPort;
        json[JSON_SPIFFS_KEY_MQTTUSER] = Settings->MqttUser;
        json[JSON_SPIFFS_KEY_MQTTPASS] = Settings->MqttPass;
        json[JSON_SPIFFS_KEY_MQTTTOPIC] = Settings->MqttTopic;
        json[JSON_SPIFFS_KEY_TIMEZONE] = Settings->Timezone;
        json[JSON_SPIFFS_KEY_STATS2NV] = Settings->saveStats;
        json[JSON_SPIFFS_KEY_INVCOLOR] = Settings->invertColors;
        json[JSON_SPIFFS_KEY_BRIGHTNESS] = Settings->Brightness;
        json[JSON_SPIFFS_KEY_FLIPDISPLAY] = Settings->flipDisplay;
        json[JSON_SPIFFS_KEY_CURRENCY] = Settings->Currency;
        json[JSON_SPIFFS_KEY_SCREENTIMEOUT] = Settings->ScreenTimeout;
        json[JSON_SPIFFS_KEY_NIGHTDIM] = Settings->nightDim;
        json[JSON_SPIFFS_KEY_NIGHTBRIGHTNESS] = Settings->NightBrightness;
        json[JSON_SPIFFS_KEY_NIGHTFADE] = Settings->NightFade;
        json[JSON_SPIFFS_KEY_LATITUDE] = Settings->Latitude;
        json[JSON_SPIFFS_KEY_LONGITUDE] = Settings->Longitude;
#ifdef NO_DISPLAY
        json[JSON_SPIFFS_KEY_STATUSLED] = Settings->statusLed;
#endif
#ifdef HAS_RGB_LED
        json[JSON_SPIFFS_KEY_LEDMODE] = Settings->ledMode;
        json[JSON_SPIFFS_KEY_LEDCOLOR] = Settings->ledColor;
        json[JSON_SPIFFS_KEY_LEDBRIGHTNESS] = Settings->ledBrightness;
        json[JSON_SPIFFS_KEY_LEDSCREENOFF] = Settings->ledScreenOff;
#endif

        printConfig(json);

        // Into the temporary file first
        File configFile = SPIFFS.open(JSON_CONFIG_TMP, "w");
        if (!configFile)
        {
            // Error, file did not open
            Serial.println("SPIFS: Failed to open config file for writing");
            return false;
        }
        const size_t expected = measureJson(json);
        const size_t written = serializeJson(json, configFile);
        configFile.close();
        if (written == 0 || written != expected)
        {
            // Error writing file (full?): the saved config stays as it was
            Serial.printf("SPIFS: Failed to write to file (%u of %u bytes)\n", (unsigned)written, (unsigned)expected);
            SPIFFS.remove(JSON_CONFIG_TMP);
            return false;
        }
        // ...then in place of the old one. SPIFFS can't rename over an
        // existing file; a power cut between the two steps leaves only the
        // complete temporary file, which loadConfig() picks up.
        if (SPIFFS.exists(JSON_CONFIG_FILE) && !SPIFFS.remove(JSON_CONFIG_FILE))
        {
            Serial.println("SPIFS: Failed to replace the config file");
            return false;
        }
        if (!SPIFFS.rename(JSON_CONFIG_TMP, JSON_CONFIG_FILE))
        {
            Serial.println("SPIFS: Failed to rename the new config file");
            return false;
        }
        return true;
    };
    return false;
}

/// @brief Load settings from config file located in SPIFFS.
/// @param TSettings* Struct to update with new settings.
/// @return true on success
bool nvMemory::loadConfig(TSettings* Settings)
{
    // Uncomment if we need to format filesystem
    // SPIFFS.format();

    // Load existing configuration file
    // Read configuration from FS json

    if (init())
    {
        if (!SPIFFS.exists(JSON_CONFIG_FILE) && SPIFFS.exists(JSON_CONFIG_TMP))
        {
            // Power cut between the two steps of saveConfig(): the temporary
            // file is complete (it is only renamed after a full write)
            Serial.println("SPIFS: Restoring the config saved just before a power cut");
            SPIFFS.rename(JSON_CONFIG_TMP, JSON_CONFIG_FILE);
        }
        if (SPIFFS.exists(JSON_CONFIG_FILE))
        {
            // The file exists, reading and loading
            File configFile = SPIFFS.open(JSON_CONFIG_FILE, "r");
            if (configFile)
            {
                Serial.println("SPIFS: Loading config file");
                DynamicJsonDocument json(3072); // room for the MQTT fields too
                DeserializationError error = deserializeJson(json, configFile);
                configFile.close();
                printConfig(json);
                if (!error)
                {
                    Settings->PoolAddress = json[JSON_SPIFFS_KEY_POOLURL] | Settings->PoolAddress;
                    // bounded copies, and none at all when the key is missing (the stock
                    // strcpy then copied the field onto itself)
                    if (const char *v = json[JSON_SPIFFS_KEY_POOLPASS].as<const char *>())
                        strlcpy(Settings->PoolPassword, v, sizeof(Settings->PoolPassword));
                    if (const char *v = json[JSON_SPIFFS_KEY_WALLETID].as<const char *>())
                        strlcpy(Settings->BtcWallet, v, sizeof(Settings->BtcWallet));
                    if (json.containsKey(JSON_SPIFFS_KEY_POOLPORT))
                        Settings->PoolPort = json[JSON_SPIFFS_KEY_POOLPORT].as<int>();
                    Settings->BackupPoolAddress = json[JSON_SPIFFS_KEY_BACKUPPOOLURL] | Settings->BackupPoolAddress;
                    Settings->BackupPoolPort = json[JSON_SPIFFS_KEY_BACKUPPOOLPORT] | Settings->BackupPoolPort;
                    strlcpy(Settings->BackupPoolPassword, json[JSON_SPIFFS_KEY_BACKUPPOOLPASS] | Settings->BackupPoolPassword, sizeof(Settings->BackupPoolPassword));
                    Settings->WebPassword = json[JSON_SPIFFS_KEY_WEBPASSWORD] | DEFAULT_WEBPASSWORD;
                    if (Settings->WebPassword.isEmpty())
                        Settings->WebPassword = DEFAULT_WEBPASSWORD;
                    Settings->MqttEnabled = json[JSON_SPIFFS_KEY_MQTTENABLED] | Settings->MqttEnabled;
                    Settings->MqttHost = json[JSON_SPIFFS_KEY_MQTTHOST] | Settings->MqttHost;
                    Settings->MqttPort = json[JSON_SPIFFS_KEY_MQTTPORT] | Settings->MqttPort;
                    Settings->MqttUser = json[JSON_SPIFFS_KEY_MQTTUSER] | Settings->MqttUser;
                    Settings->MqttPass = json[JSON_SPIFFS_KEY_MQTTPASS] | Settings->MqttPass;
                    Settings->MqttTopic = json[JSON_SPIFFS_KEY_MQTTTOPIC] | Settings->MqttTopic;
                    if (json.containsKey(JSON_SPIFFS_KEY_TIMEZONE))
                        Settings->Timezone = json[JSON_SPIFFS_KEY_TIMEZONE].as<int>();
                    if (json.containsKey(JSON_SPIFFS_KEY_STATS2NV))
                        Settings->saveStats = json[JSON_SPIFFS_KEY_STATS2NV].as<bool>();
                    if (json.containsKey(JSON_SPIFFS_KEY_INVCOLOR)) {
                        Settings->invertColors = json[JSON_SPIFFS_KEY_INVCOLOR].as<bool>();
                    } else {
                        Settings->invertColors = false;
                    }
                    if (json.containsKey(JSON_SPIFFS_KEY_BRIGHTNESS)) {
                        Settings->Brightness = json[JSON_SPIFFS_KEY_BRIGHTNESS].as<int>();
                    } else {
                        Settings->Brightness = 250;
                    }
                    Settings->flipDisplay = json[JSON_SPIFFS_KEY_FLIPDISPLAY] | false;
                    Settings->Currency = json[JSON_SPIFFS_KEY_CURRENCY] | DEFAULT_CURRENCY;
                    Settings->ScreenTimeout = json[JSON_SPIFFS_KEY_SCREENTIMEOUT] | DEFAULT_SCREENTIMEOUT;
                    Settings->nightDim = json[JSON_SPIFFS_KEY_NIGHTDIM] | DEFAULT_NIGHTDIM;
                    Settings->NightBrightness = json[JSON_SPIFFS_KEY_NIGHTBRIGHTNESS] | DEFAULT_NIGHTBRIGHTNESS;
                    Settings->NightFade = json[JSON_SPIFFS_KEY_NIGHTFADE] | DEFAULT_NIGHTFADE;
                    Settings->Latitude = json[JSON_SPIFFS_KEY_LATITUDE] | 0.0f;
                    Settings->Longitude = json[JSON_SPIFFS_KEY_LONGITUDE] | 0.0f;
#ifdef NO_DISPLAY
                    Settings->statusLed = json[JSON_SPIFFS_KEY_STATUSLED] | DEFAULT_STATUSLED;
#endif
#ifdef HAS_RGB_LED
                    Settings->ledMode = json[JSON_SPIFFS_KEY_LEDMODE] | DEFAULT_LEDMODE;
                    Settings->ledColor = json[JSON_SPIFFS_KEY_LEDCOLOR] | (uint32_t)DEFAULT_LEDCOLOR;
                    Settings->ledBrightness = json[JSON_SPIFFS_KEY_LEDBRIGHTNESS] | DEFAULT_LEDBRIGHTNESS;
                    Settings->ledScreenOff = json[JSON_SPIFFS_KEY_LEDSCREENOFF] | DEFAULT_LEDSCREENOFF;
#endif
                    return true;
                }
                else
                {
                    // Error loading JSON data
                    Serial.println("SPIFS: Error parsing config file!");
                }
            }
            else
            {
                Serial.println("SPIFS: Error opening config file!");
            }
        }
        else
        {
            Serial.println("SPIFS: No config file available!");
        }
    }
    return false;
}

/// @brief Delete config file from SPIFFS
/// @return true on successs
bool nvMemory::deleteConfig()
{
    Serial.println("SPIFS: Erasing config file..");
    SPIFFS.remove(JSON_CONFIG_TMP);
    return SPIFFS.remove(JSON_CONFIG_FILE); //Borramos fichero
}

/// @brief Prepare and mount SPIFFS
/// @return true on success
bool nvMemory::init()
{
    if (!Initialized_)
    {
        Serial.println("SPIFS: Mounting File System...");
        // May need to make it begin(true) first time you are using SPIFFS
        Initialized_ = SPIFFS.begin(false) || SPIFFS.begin(true);
        Initialized_ ? Serial.println("SPIFS: Mounted") : Serial.println("SPIFS: Mounting failed.");
    }
    else
    {
        Serial.println("SPIFS: Already Mounted");
    }
    return Initialized_;
};

#else

nvMemory::nvMemory() {}
nvMemory::~nvMemory() {}
bool nvMemory::saveConfig(TSettings* Settings) { return false; }
bool nvMemory::loadConfig(TSettings* Settings) { return false; }
bool nvMemory::deleteConfig() { return false; }
bool nvMemory::init() { return false; }


#endif //NVMEM_TYPE