// M5StickC Plus2 early board init.
//
// initVariant() is called by the Arduino core (initArduino) before setup(),
// i.e. as early as possible after the bootloader.
//
// The Plus2 has no PMIC: it stays powered on battery only while GPIO4
// (POWER_HOLD) is driven HIGH. A normal ESP.restart() / watchdog reset puts
// every pad back to its default (floating) state, which would cut the power
// on battery instead of rebooting. GPIO4 is an RTC IO, so latching it with
// gpio_hold_en() keeps it HIGH across software resets.

#include "pins_arduino.h"
#include "driver/gpio.h"

static void plus2_output(gpio_num_t pin, uint32_t level)
{
    // Level first, then direction: the pad never glitches to the wrong state.
    gpio_set_level(pin, level);
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    gpio_set_level(pin, level);
}

extern "C" void initVariant(void)
{
    // 1) Keep the stick powered. While the pad is still latched from a
    //    previous run, configure the registers to output HIGH, then release
    //    the latch (pad switches straight to HIGH) and latch it again.
    plus2_output((gpio_num_t)POWER_HOLD, 1);
    gpio_hold_dis((gpio_num_t)POWER_HOLD);
    gpio_set_level((gpio_num_t)POWER_HOLD, 1);
    gpio_hold_en((gpio_num_t)POWER_HOLD);           // survives ESP.restart()

    // 2) Red LED / IR emitter off (active high).
    plus2_output((gpio_num_t)LED_BUILTIN, 0);

    // 3) Buzzer silent.
    plus2_output((gpio_num_t)BUZZER, 0);
}
