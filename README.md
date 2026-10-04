# NerdMiner Modded

**A modded fork of the NerdSoloMiner v2**

This is a modified version of the original **[NerdMiner_v2](https://github.com/BitMaker-hub/NerdMiner_v2)** by BitMaker-hub. All credit for the original project, the Stratum implementation and the multi-board support goes to the original authors and contributors. If you want the stock, widely tested firmware for any of the supported boards, use the original repository.

This is a **free and open source project** that let you try to reach a bitcoin block with a small piece of hardware.

The main aim of this project is to let you **learn more about minery** and to have a beautiful piece of hardware in your desktop.

Original NerdMiner_v2 project https://github.com/BitMaker-hub/NerdMiner_v2
(which itself started from https://github.com/valerio-vaccaro/HAN)

## What's different in this mod

![NerdMiner Modded running on a CYD](images/nerdminer-mod.gif)

> **⚠️ Tested on the CYD only.** The changes in this fork have been developed and tested on two boards: the **ESP32-2432S028R (2.8" "Cheap Yellow Display", CYD)** and the **4.0" CYD with the 320x480 ST7796 panel**. Other boards still build from the same code base as the original, but they have **not** been tested with these changes. Expect rough edges, and please report what you find.

- **High hashrate on the CYD** – the SHA-256 mining path for the classic ESP32 has been reworked to use the hardware SHA engine in a pipelined fashion, giving a sustained rate of roughly **950 KH/s** on the CYD (classic ESP32, both cores mining). Changes to the mining code are validated hash by hash against software, so the extra speed is not coming at the cost of invalid shares.
- **New "Pulse" display theme** – the CYD UI has been completely redesigned. It is bitmap-free and has three screens: **MINE**, **MARKET** and **CLOCK**. Tap the wordmark to cycle the accent colour, and tap the top-right corner to toggle the backlight.
- **4.0" CYD support** – the 4.0" 320x480 ST7796 variant of the CYD (ESP32-WROOM-32E, resistive touch) has its own build, `ESP32-4in-ST7796`: the same ~950 KH/s and the Pulse theme laid out for the larger 480x320 screen.
- **Classic theme still available** – prefer the original look? Build the `ESP32-2432S028R-Classic` environment (`pio run -e ESP32-2432S028R-Classic -t upload`).
- **BTC price in your currency** – choose which fiat currency the BTC price is shown in from the web settings page.
- **Improved web settings page** – the settings page stays reachable while mining, and now also has options for display flip (USB on the left), colour inversion, brightness and currency. After saving it waits for the device to restart and returns to the settings page automatically. See [Web settings page](#web-settings-page) for how to log in.

### Why this hashrate is real

The ~950 KH/s is what the miner actually hashes, not what a counter says:

- **Hardware limit** – the classic ESP32's SHA engine can't be loaded with an external midstate, so every nonce costs three hardware hash blocks plus two engine loads: about 228 CPU cycles of engine time, a ceiling of roughly 1.05 MH/s at 240 MHz. The mining loop keeps the engine busy for all but ~30 cycles of each nonce (257 cycles per nonce, ~930 KH/s); the second core adds ~25 KH/s in software.
- **Measured on the chip** – the loop's timing comes from an on-device bench (`-D SHA_BENCH`, see `src/sha_bench.cpp`) that checks every single hash against software, also while the other core hammers the peripheral bus. That is how the hardware quirk behind silently corrupted hashes in the usual pipelined loops was found: writes to the SHA registers less than three cycles apart can be dropped whenever the other core is using the same bus.
- **Only valid hashes are counted** – every candidate the hardware finds is recomputed in software before it is used. A hashrate counter on its own can look faster after an optimisation while the miner is quietly producing *fewer* valid shares, because corrupted hashes are still counted; here the firmware checks itself with a known-answer test at power-on and falls back to a slower loop if the fast one ever gets hashes wrong.
- **The rate is the rate it actually mines at** – it's the sustained average over a long run, not a short burst or a best-case peak.

If a number sounds too good for this chip, check whether the shares it finds actually keep up with it.

For the other features, supported boards, flashing and configuration, the original documentation below still applies.

## Requirements

- TTGO T-Display S3 or any supported boards (check Build tutorial 👇)
- 3D BOX [here](3d_files/)

### Project description

**ESP32 implementing Stratum protocol** to mine on solo pool. Pool can be changed but originally works with [public-pool.io](https://web.public-pool.io) (where Nerdminers are supported).

This project was initialy developed using ESP32-S3, but currently support other boards. It uses WifiManager to modify miner settings and save them to SPIFF.
The microMiner comes with several screens to monitor it's working procedure and also to show you network mining stats.
Currently includes:

- NerdMiner Screen > Mining data of Nerdminer
- ClockMiner Screen > Fashion style clock miner
- GlobalStats Screen > Global minery stats and relevant data

This miner is multicore and multithreads, both cores are used to mine and several threads are used to implementing stratum work and wifi stuff.
Every time an stratum job notification is received miner update its current work to not create stale shares.

**IMPORTANT** Miner is not seen by all standard pools due to its low share difficulty. You can check miner work remotely using specific pools specified down or seeing logs via UART.

**_Current project is still in developement and more features will be added_**

## Build Tutorial

### Hardware requirements

- LILYGO T-Display S3 (original one) or any other supported boards
- 3D BOX [here](3d_files/)

#### Current Supported Boards

- LILYGO T-Display S3 ([Aliexpress link\*](https://s.click.aliexpress.com/e/_Ddy7739))
- ESP32-WROOM-32, ESP32-Devkit1.. ([Aliexpress link\*](https://s.click.aliexpress.com/e/_DCzlUiX))
- LILYGO T-QT pro ([Aliexpress link\*](https://s.click.aliexpress.com/e/_DBQIr43))
- LILYGO T-Display 1.14 ([Aliexpress link\*](https://s.click.aliexpress.com/e/_DEqGvSJ))
- LILYGO T-Display S3 AMOLED ([Aliexpress link\*](https://s.click.aliexpress.com/e/_DmOIK6j))
- LILYGO T-Display S3 AMOLED Touch ([Board Info](https://www.lilygo.cc/products/t-display-s3-amoled?variant=43532279939253))
- LILYGO T-Dongle S3 ([Aliexpress link\*](https://s.click.aliexpress.com/e/_DmQCPyj))
- ESP32-2432S028R 2,8" ([Aliexpress link\*](https://s.click.aliexpress.com/e/_DdXkvLv) / Dev support: @nitroxgas / ⚡jadeddonald78@walletofsatoshi.com)
- 4.0" CYD, 320x480 ST7796 (ESP32-WROOM-32E, build environment `ESP32-4in-ST7796`)
- ESP32-cam ([Board Info](https://lastminuteengineers.com/getting-started-with-esp32-cam/) / Dev support: @elmo128)
- M5-StampS3 ([Aliexpress link\*](https://s.click.aliexpress.com/e/_DevABY3) / Dev support: @gyengus)
- Wemos Lolin S3 Mini ([Board Info](https://docs.platformio.org/en/latest/boards/espressif32/lolin_s3_mini.html))
- Wemos Lolin S2 Mini ([Board Info](https://docs.platformio.org/en/latest/boards/espressif32/lolin_s2_mini.html))
- Weact S3 Mini ([Board Info](https://github.com/WeActStudio/WeActStudio.ESP32S3-MINI))
- Weact ESP32-D0WD-V3 ([Board Info](https://github.com/WeActStudio/WeActStudio.ESP32CoreBoard))
- ESP32-S3 Devkit ([Board Info](https://docs.platformio.org/en/latest/boards/espressif32/esp32-s3-devkitm-1.html))
- ESP32-C3 Devkit ([Board Info](https://docs.platformio.org/en/latest/boards/espressif32/esp32-c3-devkitm-1.html))
- ESP32-C3 Super Mini ([Board Info](https://docs.platformio.org/en/latest/boards/espressif32/seeed_xiao_esp32c3.html))
- Waveshare ESP32-S3-GEEK ([Board Info](https://www.waveshare.com/wiki/ESP32-S3-GEEK))
- LILYGO T-HMI ([Aliexpress link\*](https://s.click.aliexpress.com/e/_oFII4s2)) / Dev support: @cosmicpsyop
- ESP32-C3 0.42 Inch OLED ([Aliexpress link\*](https://s.click.aliexpress.com/e/_oDmT4Id) / Dev support: @mrthiti / ⚡ wallet@thiti.dev)
- ESP32-S3 0.42 Inch OLED ([Aliexpress link\*](https://s.click.aliexpress.com/e/_oFIMUoh) / Dev support: @mrthiti / ⚡ wallet@thiti.dev)

\*Affiliate links

### Flash firmware

> **Flashing NerdMiner Modded:** use the web flasher at **https://henryscat.github.io/NerdMiner-Modded/** (Chrome, Edge or Brave). It offers a *full install* (factory image: bootloader, partition table and firmware, for a first flash) and a *firmware-only update* (keeps your settings). Pick your board there first: the 2.8" CYD (ESP32-2432S028R) or the 4.0" CYD (320x480 ST7796). You can also build and flash this repository with PlatformIO, e.g. `pio run -e ESP32-2432S028R -t upload` for the 2.8" board or `pio run -e ESP32-4in-ST7796 -t upload` for the 4.0" one.
>
> The online flasher and the prebuilt binaries in the sections below come from the **original** project and will install the original firmware, not the modded one.

#### microMiners Flashtool [Recommended]

Easyiest way to flash firmware. Build your own miner using the folowing firwmare flash tool:

1. Get a TTGO T-display S3 or any other supported board
1. Go to NM2 flasher online: https://flasher.bitronics.store/ (recommend via Google Chrome incognito mode)

#### Standard tool

Create your own miner using the online firwmare flash tool **ESPtool** and one of the **binary files** that you will find in the `bin` folder.
If you want you can compile the entire project using Arduino, PlatformIO or Expressif IDF.

1. Get a TTGO T-display S3 or any supported board
1. Download this repository
1. Go to ESPtool online: https://espressif.github.io/esptool-js/
1. Load the firmware with the binary from one of the sub-folders of `bin` corresponding to your board.
1. Plug your board and select each file from the sub-folder (`.bin` files).

### Update firmware

Update NerdMiner firmware following same flashing steps but only using the file 0x10000_firmware.bin.

#### Build troubleshooting

1. Online [ESP Tool](https://espressif.github.io/esptool-js/) works with chrome, chromium, brave
1. ESPtool recommendations: use 115200bps
1. Build errors > If during firmware download upload stops, it's recommended to enter the board in boot mode. Unplug cable, hold right bottom button and then plug cable. Try programming
1. In extreme case you can "Erase all flash" on ESPtool to clean all current configuration before uploading firmware. There has been cases that experimented Wifi failures until this was made.
1. In case of ESP32-WROOM Boards, could be necessary to put your board on boot mode. Hold boot button, press reset button and then program.

## NerdMiner configuration

After programming, you will only need to setup your Wifi and BTC address.

Note: when BTC address of your selected wallet is not provided, mining will not be started.

#### Wifi Accesspoint


1. Connect to NerdMinerAP
   - AP: NerdMinerAP
   - PASS: MineYourCoins
1. Set up your Wifi Network
1. Add your BTC address
1. Change the password if needed

   - If you are using public-pool.io and you want to set a custom name to your worker you can append a string with format _.yourworkername_ to the address


#### Web settings page

Once the miner is on your Wifi network it serves a settings page, also while it is mining, so you can change the pool, wallet, timezone, currency and display options without going back to the setup access point.

1. Open `http://<miner IP address>/` in a browser on the same network. On the CYD the IP address is shown at the top of the screen; it is also printed on the serial log at start-up and listed by your router.
1. Log in when the browser asks:
   - User: admin
   - Password: MineYourCoins
1. Change what you need and press **Save & Restart**. The miner restarts with the new settings and the page comes back by itself.

Note: the login is the same on every miner (the password is the one of the setup access point) and the page is plain HTTP, so only use it on a network you trust.

#### SD card (if available)

1. Format a SD card using Fat32.
1. Create a file named "config.json" in your card's root, containing the the following structure. Adjust the settings to your needs:
```
{
  "SSID": "myWifiSSID",
  "WifiPW": "myWifiPassword",
  "PoolUrl": "public-pool.io",
  "PoolPort": 3333,
  "PoolPassword": "x",
  "BtcWallet": "walletID",
  "Timezone": 2,
  "SaveStats": false
}
```

1. Insert the SD card.
1. Hold down the "reset configurations" button as described below to reset the configurations and/or boot without settings in your nvmemory.
1. Power down to remove the SD card. It is not needed for mining.

#### Pool selection

Recommended low difficulty share pools:

| Pool URL          | Port  | Web URL                    | Status                                                             |
| ----------------- | ----- | -------------------------- | ------------------------------------------------------------------ |
| public-pool.io    | 3333 | https://web.public-pool.io | Open Source Solo Bitcoin Mining Pool supporting open source miners |
| pool.nerdminers.org    | 3333  | https://nerdminers.org     | The official Nerdminer pool site - Mantained by @golden-guy |
| pool.nerdminer.io | 3333  | https://nerdminer.io       | Mantained by CHMEX                                                 |
| pool.pyblock.xyz  | 3333  | https://pool.pyblock.xyz/  | Mantained by curly60e                                              |
| pool.sethforprivacy.com  | 3333  | https://pool.sethforprivacy.com/  | Mantained by @sethforprivacy - public-pool fork      |
| pool.stompi.de  | 3333  | http://web.stompi.de  | Mantained by @odinstar - public-pool fork      |
|pool.solomining.de| 3333  | https://pool.solomining.de/ | Mantained by https://x.com/solo_mining |

Other standard pools not compatible with low difficulty share:

| Pool URL                 | Port | Web URL                                   |
| ------------------------ | ---- | ----------------------------------------- |
| solo.ckpool.org          | 3333 | https://solo.ckpool.org/                  |
| btc.zsolo.bid            | 6057 | https://zsolo.bid/en/btc-solo-mining-pool |
| eu.stratum.slushpool.com | 3333 | https://braiins.com/pool                  |

### Buttons

#### One button devices:

- One click > change screen.
- Double click > change screen orientation.
- Tripple click > turn the screen off and on again.
- Hold 5 seconds > **reset the configurations and reboot** your NerdMiner.

#### Two button devices:

With the USB-C port to the right:

**TOP BUTTON**

- One click > change screen.
- Hold 5 seconds > top right button to **reset the configurations and reboot** your NerdMiner.
- Hold and power up > enter **configuration mode** and edit current config via Wifi. You could change your settings or verify them.

**BOTTOM BUTTON**

- One Click > turn the screen off and on again
- Double click > change orientation (default is USB-C to the right)

#### Build video

[![Ver video aquí](https://img.youtube.com/vi/POUT2R_opDs/0.jpg)](https://youtu.be/POUT2R_opDs)

## Developers

### Project guidelines

- Current project was adapted to work with PlatformIO
- Current project works with ESP32-S3 and ESP32-wroom.
- Partition squeme should be build as huge app
- All libraries needed shown on platform.ini

### Job done

- [x] Move project to platformIO
- [x] Bug rectangle on screen when 1milion shares
- [x] Bug memory leaks
- [x] Bug Reboots when received JSON contains some null values
- [x] Implement midstate sha256
- [x] Bug Wificlient DNS unresolved on Wifi.h
- [x] Code refactoring
- [x] Add blockHeight to screen
- [x] Add clock to show current time
- [x] Add new screen with global mining stats
- [x] Add pool support for low difficulty miners
- [x] Add best difficulty on miner screen
- [x] Add suport to standard ESP32 dev-kit / ESP32-WROOM
- [x] Code changes to support adding multiple boards
- [x] Add support to TTGO T-display 1.14
- [x] Add support to Amoled

### In process

- [ ] Create a daisy chain protocol via UART or I2C to support ESP32 hashboards
- [ ] Create new screen like clockMiner but with BTC price
- [ ] Add support to control BM1397
- [ ] Add password field in web configuration form

### Donations/Project contributions

If you would like to contribute and help dev team with this project you can send a donation to the following LN address ⚡teamnerdminer@getalby.com⚡ or using one of the affiliate links above.

If you want to order a fully assembled Nerdminer you can contribute to my job at 🛒[bitronics.store](https://bitronics.store)🛒

Enjoy
