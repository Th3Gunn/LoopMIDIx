    /*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <stdbool.h>
extern void esp_rom_delay_us(uint32_t us);

/* Configuration via menuconfig (see Kconfig.projbuild added entries) */
#define SHIFT_DATA_PIN    CONFIG_SHIFT_DATA_PIN
#define SHIFT_CLOCK_PIN   CONFIG_SHIFT_CLOCK_PIN
#define SHIFT_LATCH_PIN   CONFIG_SHIFT_LATCH_PIN
#define BUTTON_PIN        CONFIG_BUTTON_PIN


#define DEBOUNCE_MS       500
#define ESP_INTR_FLAG_DEFAULT 0

static const char *TAG = "gpio_shift";
static QueueHandle_t gpio_evt_queue = NULL;
static uint8_t sr_state = 0; // 8-bit shift register state, QA is bit0

// Shift out LSB first so bit0 -> QA on 74HC595
static void shift595_write(uint8_t data)
{
    // Ensure clock and latch low
    gpio_set_level(SHIFT_LATCH_PIN, 0);
    gpio_set_level(SHIFT_CLOCK_PIN, 0);

    for (int i = 0; i < 8; i++) {
        int bit = (data >> i) & 0x1; // LSB first
        gpio_set_level(SHIFT_DATA_PIN, bit);
        // Pulse clock
        gpio_set_level(SHIFT_CLOCK_PIN, 1);
        esp_rom_delay_us(1);
        gpio_set_level(SHIFT_CLOCK_PIN, 0);
        esp_rom_delay_us(1);
    }
    // Latch data
    gpio_set_level(SHIFT_LATCH_PIN, 1);
    esp_rom_delay_us(1);
    gpio_set_level(SHIFT_LATCH_PIN, 0);

    ESP_LOGI(TAG, "Shift reg write: 0x%02X", data);
}

static void IRAM_ATTR button_isr(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void button_task(void* arg)
{
    uint32_t io_num;

    for (;;) {
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {

            // Wait debounce time
            vTaskDelay(DEBOUNCE_MS / portTICK_PERIOD_MS);

            // Read stable level
            int level = gpio_get_level(io_num);

            // Active-low button: toggle ONLY on press
            if (level == 0) {
                // Toggle QA (bit 0)
                sr_state ^= 0x80;
                shift595_write(sr_state);
            }

            // Clear any extra edges accumulated during debounce
            xQueueReset(gpio_evt_queue);
        }
    }
}


void app_main(void)
{
    // Configure shift register pins as outputs
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << SHIFT_DATA_PIN) | (1ULL << SHIFT_CLOCK_PIN) | (1ULL << SHIFT_LATCH_PIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    // Initialize outputs to default safe states
    gpio_set_level(SHIFT_DATA_PIN, 0);
    gpio_set_level(SHIFT_CLOCK_PIN, 0);
    gpio_set_level(SHIFT_LATCH_PIN, 0);

    // Configure button pin as input with pull-up, interrupt on any edge
    gpio_config_t btn_conf = {};
    btn_conf.intr_type = GPIO_INTR_ANYEDGE;
    btn_conf.mode = GPIO_MODE_INPUT;
    btn_conf.pin_bit_mask = (1ULL << BUTTON_PIN);
    btn_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    btn_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&btn_conf);

    // Create event queue and task
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    xTaskCreate(button_task, "button_task", 2048, NULL, 10, NULL);

    // Install ISR and add handler
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(BUTTON_PIN, button_isr, (void*) BUTTON_PIN);

    ESP_LOGI(TAG, "Setup complete. Button on GPIO %d, Shift pins: D=%d C=%d L=%d", BUTTON_PIN, SHIFT_DATA_PIN, SHIFT_CLOCK_PIN, SHIFT_LATCH_PIN);

    // Ensure shift register initial state
    sr_state = 0x00;
    shift595_write(sr_state);

    // Main loop prints status (optional)
    while (1) {
        ESP_LOGI(TAG, "SR state: 0x%02X", sr_state);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}
