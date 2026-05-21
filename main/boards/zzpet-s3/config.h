#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/adc.h>

// Audio: simplex INMP441-style mic + MAX98357A-style I2S amp (no codec, no MCLK, no PA enable)
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_METHOD_SIMPLEX

#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

// Buttons
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define TOUCH_BUTTON_GPIO       GPIO_NUM_10

// LEDs: WS2812
//   - Bottom pair: 2 LEDs on GPIO 8
//   - WiFi indicator: 1 LED on GPIO 48 (separate chain)
#define BUILTIN_LED_GPIO        GPIO_NUM_48
#define BOTTOM_LED_GPIO         GPIO_NUM_8
#define BOTTOM_LED_COUNT        2

// Battery sense: VBAT/2 divider on GPIO 2 (ADC1 CH1)
#define BATTERY_ADC_UNIT        ADC_UNIT_1
#define BATTERY_ADC_CHANNEL     ADC_CHANNEL_1
#define BATTERY_VOLTAGE_UPPER_RESISTOR  100000.0f
#define BATTERY_VOLTAGE_LOWER_RESISTOR  100000.0f

// OLED display: SH1106 128x64 over I2C @ 0x3C
#define DISPLAY_SDA_PIN         GPIO_NUM_41
#define DISPLAY_SCL_PIN         GPIO_NUM_42
#define DISPLAY_WIDTH           128
#define DISPLAY_HEIGHT          64
// Display is physically mounted "upside-down" on the PCB relative to the
// panel's natural orientation. The SH1106 ESP-IDF panel driver already
// applies SEG_REMAP=0xA1 + COM_SCAN=0xC8 (mirrored) by default; setting
// both flags to false here keeps those defaults so text reads upright.
#define DISPLAY_MIRROR_X        false
#define DISPLAY_MIRROR_Y        false
#define SH1106

// Servos (LEDC PWM @ 50 Hz)
//   FR = Front/Right leg, BR = Back/Right foot,
//   FL = Front/Left  leg, BL = Back/Left  foot.
#define SERVO_FR_GPIO           GPIO_NUM_13
#define SERVO_BR_GPIO           GPIO_NUM_14
#define SERVO_FL_GPIO           GPIO_NUM_17
#define SERVO_BL_GPIO           GPIO_NUM_18

#define SERVO_LEDC_TIMER        LEDC_TIMER_1
#define SERVO_LEDC_MODE         LEDC_LOW_SPEED_MODE
#define SERVO_LEDC_FREQ_HZ      50
#define SERVO_LEDC_RESOLUTION   LEDC_TIMER_13_BIT

#endif // _BOARD_CONFIG_H_
