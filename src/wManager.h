#ifndef _WMANAGER_H
#define _WMANAGER_H

#include <mutex>
#include "drivers/storage/storage.h"

// Held while a String in Settings is changed after boot, and by a task on
// another core reading one (the LAN settings page): the pool fields are
// swapped in by the stratum task, the currency by the monitor task. See
// takePoolChange() in mining.cpp and applyPendingSettings() in monitor.cpp.
extern std::mutex g_settingsMutex;

void init_WifiManager();
void wifiManagerProcess();
void reset_configuration();
void saveSettingsToFlash(TSettings *settings);

#endif // _WMANAGER_H
