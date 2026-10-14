#ifndef _ESP32_2432S028R
#define _ESP32_2432S028R

#define PIN_BUTTON_1 0
//#define PIN_BUTTON_2 22  // Not used
// #define PIN_ENABLE5V 21  // Not used
// RGB LED on the back (common anode, active low); colour and mode are set on
// the LAN settings page, see drivers/displays/rgbLed.cpp
#define HAS_RGB_LED
#define LED_PIN      4   // Red pin
#define LED_PIN_G    16  // Green pin
#define LED_PIN_B    17  // Blue pin

// Pin defines for the SD card interface
// This is working for both, ESP32 2432S028R and ESP 2432S028_2USB boards 
// --------------------------------------
// use SPI interface
// (default SPI unit provided by <SPI.h>)
// setup SPI pins.

#define SDSPI_CS    5
#define SDSPI_CLK   18
#define SDSPI_MOSI  23
#define SDSPI_MISO  19

// Pin defines for the SD card interface
// This is working for both, ESP32 2432S028R and ESP 2432S028_2USB boards 
// --------------------------------------
// use SPI interface
// (default SPI unit provided by <SPI.h>)
// setup SPI pins.

#define SDSPI_CS    5
#define SDSPI_CLK   18
#define SDSPI_MOSI  23
#define SDSPI_MISO  19

// calls api to retrieve worker metrics
#define SCREEN_WORKERS_ENABLE (1)

#endif
