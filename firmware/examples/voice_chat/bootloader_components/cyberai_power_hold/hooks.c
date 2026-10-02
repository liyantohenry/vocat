/*
 * Power-hold hook for the CyberAI EchoEar clone.
 *
 * The stock firmware drives GPIO4, GPIO7, GPIO15 and GPIO21 high. GPIO7 is the power latch
 * (driving it low switches the board off), GPIO21 powers the touch controller, GPIO15's role is
 * unknown and GPIO4 is the speaker amplifier enable (switched by the ES8311 driver in the app).
 * Any chip or system reset returns the pads to their default state and cuts the power.
 *
 * This hook drives GPIO7/15/21 high as early as possible and enables the RTC pad hold, which
 * survives system resets (esp_restart, panics, watchdogs), so software reboots no longer switch
 * the board off. The pads stay high until a full power cycle.
 */
#include <stdint.h>
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_rom_gpio.h"

/* Function used to tell the linker to include this file with all its symbols. */
void bootloader_hooks_include(void)
{
}

static const uint32_t s_hold_pins[] = {7, 15, 21};

/* Earlier builds of this hook (and the JTAG flash script) also held GPIO4, and holds survive
 * resets, so release it: a held pad ignores the codec driver's amplifier on/off writes.
 */
#define PA_ENABLE_PIN 4

void bootloader_before_init(void)
{
    uint32_t hold_mask = 0;
    for (int i = 0; i < sizeof(s_hold_pins) / sizeof(s_hold_pins[0]); i++) {
        const uint32_t pin = s_hold_pins[i];
        esp_rom_gpio_pad_select_gpio(pin);
        REG_WRITE(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * pin, SIG_GPIO_OUT_IDX);
        REG_WRITE(GPIO_OUT_W1TS_REG, 1u << pin);
        REG_WRITE(GPIO_ENABLE_W1TS_REG, 1u << pin);
        /* GPIO0-21 are RTC pads: hold bit n in RTC_CNTL_PAD_HOLD_REG holds GPIOn */
        hold_mask |= 1u << pin;
    }
    REG_SET_BIT(RTC_CNTL_PAD_HOLD_REG, hold_mask);
    REG_CLR_BIT(RTC_CNTL_PAD_HOLD_REG, 1u << PA_ENABLE_PIN);
}

void bootloader_after_init(void)
{
}
