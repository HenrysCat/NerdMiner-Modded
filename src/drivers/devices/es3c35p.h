#ifndef _ES3C35P_H
#define _ES3C35P_H

// LCDWiki ES3C35P (also sold rebadged, e.g. Hosyond): ESP32-S3, 16MB flash,
// 8MB octal PSRAM, native USB, 3.5" 320x480 ST77922 panel on quad SPI with
// the touch controller built into the same chip.

#define PIN_BUTTON_1 0 // BOOT

// Panel (quad SPI, these are the FSPI IOMUX pins). The panel's reset line is
// tied to the chip's EN pin, there is no GPIO for it.
#define LCD_PIN_CS 10
#define LCD_PIN_SCK 12
#define LCD_PIN_D0 11
#define LCD_PIN_D1 13
#define LCD_PIN_D2 14
#define LCD_PIN_D3 9
#define LCD_PIN_BL 41 // active high

// Touch (I2C, shared with the ES8311 audio codec)
#define TOUCH_PIN_SDA 38
#define TOUCH_PIN_SCL 39
#define TOUCH_PIN_RST 48
#define TOUCH_I2C_ADDR 0x55

#define AMP_PIN_SHUTDOWN 1 // speaker amplifier, low = enabled

#define ES3C35P_DISPLAY

// calls api to retrieve worker metrics
#define SCREEN_WORKERS_ENABLE (1)

#endif
