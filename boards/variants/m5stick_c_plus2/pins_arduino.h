#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>

// M5StickC Plus2 (ESP32-PICO-V3-02)

#define EXTERNAL_NUM_INTERRUPTS 16
#define NUM_DIGITAL_PINS        40
#define NUM_ANALOG_INPUTS       16

#define analogInputToDigitalPin(p)  (((p)<20)?(analogChannelToDigitalPin(p)):-1)
#define digitalPinToInterrupt(p)    (((p)<40)?(p):-1)
#define digitalPinHasPWM(p)         (p < 34)

static const uint8_t LED_BUILTIN = 19;   // red LED + IR emitter, active high
#define BUILTIN_LED  LED_BUILTIN         // backward compatibility
#define LED_BUILTIN  LED_BUILTIN

static const uint8_t TX = 1;
static const uint8_t RX = 3;

// Grove port (HY2.0-4P)
static const uint8_t SDA = 32;
static const uint8_t SCL = 33;

// Internal I2C bus (BM8563 RTC, MPU6886 IMU)
static const uint8_t SDA_INT = 21;
static const uint8_t SCL_INT = 22;

// Display SPI bus
static const uint8_t SS   = 5;
static const uint8_t MOSI = 15;
static const uint8_t MISO = 36;
static const uint8_t SCK  = 13;

// Board functions
static const uint8_t BUTTON_A   = 37;
static const uint8_t BUTTON_B   = 39;
static const uint8_t BUTTON_PWR = 35;
static const uint8_t POWER_HOLD = 4;
static const uint8_t BUZZER     = 2;
static const uint8_t BAT_ADC    = 38;
static const uint8_t TFT_BL_PIN = 27;

// Header pins
static const uint8_t G0  = 0;
static const uint8_t G25 = 25;
static const uint8_t G26 = 26;
static const uint8_t G32 = 32;
static const uint8_t G33 = 33;
static const uint8_t G36 = 36;
static const uint8_t G37 = 37;
static const uint8_t G39 = 39;

static const uint8_t DAC1 = 25;
static const uint8_t DAC2 = 26;

static const uint8_t ADC1 = 36;
static const uint8_t ADC2 = 38;

#endif /* Pins_Arduino_h */
