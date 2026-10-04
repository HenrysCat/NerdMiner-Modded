#ifndef _ESP32_4IN_ST7796_H
#define _ESP32_4IN_ST7796_H

// 4.0" "CYD" (ESP32-WROOM-32E: ESP32-D0WD-V3, 4MB flash, CH340) with a
// 320x480 ST7796 panel and an XPT2046 resistive touch controller.

#define PIN_BUTTON_1 0 // BOOT

// Panel: the TFT_eSPI pins are set in platformio.ini (HSPI: SCK 14, MOSI 13,
// MISO 12, CS 15, DC 2). The panel has no reset GPIO.
#define LCD_PIN_BL 27 // active high

// Touch: on the panel's SPI bus, with its own chip select
#define TOUCH_PIN_CS 33
#define TOUCH_PIN_IRQ 36 // low while the panel is pressed

#define ESP32_4IN_ST7796_DISPLAY

// calls api to retrieve worker metrics
#define SCREEN_WORKERS_ENABLE (1)

#endif
