#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/spi_master.h"
#include "esp_log.h"

static const char *TAG = "MIDI_SWITCHER";

//buttons pins
#define BTN_1_PIN           47
#define BTN_2_PIN           16
#define BTN_3_PIN           20
#define BTN_4_PIN           38
#define BTN_5_PIN           21
#define BTN_6_PIN           0
#define BTN_7_PIN           19
#define BTN_8_PIN           45
#define BTN_9_PIN           13
#define BTN_10_PIN          14

//74HC595 pins
#define SHIFT_DATA_PIN      1  //SER
#define SHIFT_CLOCK_PIN     2  //
#define SHIFT_LATCH_PIN     41  //
#define OE_PIN              40  // ~OE (Output Enable) pin
#define SRCLR_PIN           42  // SRCLR (Serial Clear) pin

//display pins
#define PIN_NUM_MOSI        11
#define PIN_NUM_CLK         12
#define PIN_NUM_CS          10

//UART pins
#define MIDI_1_TX_PIN         15
#define MIDI_1_RX_PIN         7
#define MIDI_2_TX_PIN         5
#define MIDI_2_RX_PIN         6

// UART port configuration (do wyrzucenia - było tylko po to by się skompilowało)
#define MIDI_UART_TX          UART_NUM_1
#define MIDI_UART_RX          UART_NUM_2
#define MIDI_TX_PIN           MIDI_1_TX_PIN
#define MIDI_RX_PIN           MIDI_2_RX_PIN

// expression ADC pin
#define EXP_ADC_PIN           4

//amp switch pins
#define AMP_SWCH_R            48
#define AMP_SWCH_T            39

typedef struct {
    char name[9];
    uint16_t relay_flags;
    uint8_t midi_pc;
    uint8_t button_flags[4];
} Preset;

#define NUM_BANKS 5
#define PRESETS_PER_BANK 4

Preset banki[NUM_BANKS][PRESETS_PER_BANK];
int active_bank = 0;
int active_preset_idx = 0;

static spi_device_handle_t spi_max;

uint8_t get_char_segment(char c) {
    switch (toupper(c)) {
        case '0': return 0x7E; case '1': return 0x30; case '2': return 0x6D;
        case '3': return 0x79; case '4': return 0x33; case '5': return 0x5B;
        case '6': return 0x5F; case '7': return 0x70; case '8': return 0x7F;
        case '9': return 0x7B; 
        case 'A': return 0x77; case 'B': return 0x1F; 
        case 'C': return 0x4E; case 'D': return 0x3D; 
        case ' ': return 0x00; default: return 0x00;
    }
}

void max7219_send(uint8_t reg, uint8_t data) {
    uint8_t tx_data[2] = { reg, data };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx_data };
    spi_device_transmit(spi_max, &t);
}

void Display_preset_name(const char* name) {
    for(int i = 1; i <= 8; i++) max7219_send(i, 0x00);
    
    for (int i = 0; i < 8; i++) {
        if (name[i] == '\0') break;
        max7219_send(8 - i, get_char_segment(name[i])); 
    }
}

void MIDI_TX(uint8_t channel, uint8_t pc_value) {
    uint8_t status = 0xC0 | (channel & 0x0F);
    uint8_t msg[2] = { status, pc_value & 0x7F };
    uart_write_bytes(MIDI_UART_TX, (const char *)msg, 2);
    ESP_LOGI(TAG, "TX -> PC: %d na kanale %d", pc_value, channel + 1);
}

static void MIDI_RX(void *arg) {
    uint8_t data[128];
    while (1) {
        int rxBytes = uart_read_bytes(MIDI_UART_RX, data, sizeof(data), pdMS_TO_TICKS(100));
        if (rxBytes > 0) {
            ESP_LOGI(TAG, "RX -> Otrzymano %d bajtów MIDI", rxBytes);
        }
    }
}

extern void esp_rom_delay_us(uint32_t us);

void update_relays(uint8_t flags) {
    gpio_set_level(SHIFT_LATCH_PIN, 0);
    gpio_set_level(SHIFT_CLOCK_PIN, 0);
    for (int i = 0; i < 8; i++) {
        gpio_set_level(SHIFT_DATA_PIN, (flags >> i) & 0x1);
        gpio_set_level(SHIFT_CLOCK_PIN, 1);
        esp_rom_delay_us(1);
        gpio_set_level(SHIFT_CLOCK_PIN, 0);
        esp_rom_delay_us(1);
    }
    gpio_set_level(SHIFT_LATCH_PIN, 1);
    esp_rom_delay_us(1);
    gpio_set_level(SHIFT_LATCH_PIN, 0);
}

void load_preset(int bank, int preset) {
    active_bank = bank;
    active_preset_idx = preset;
    Preset active_preset = banki[bank][preset];

    update_relays(active_preset.relay_flags);
    MIDI_TX(0, active_preset.midi_pc);
    Display_preset_name(active_preset.name);
}

static void Handle_Buttons_Task(void* arg) {
    bool last_state[4] = {true, true, true, true}; 
    
    while (1) {
        bool b1 = gpio_get_level(BTN_1_PIN);
        bool b2 = gpio_get_level(BTN_2_PIN);
        bool b3 = gpio_get_level(BTN_3_PIN);
        bool b4 = gpio_get_level(BTN_4_PIN);

        if (!b1 && !b2 && (last_state[0] || last_state[1])) {
            active_bank = (active_bank > 0) ? active_bank - 1 : NUM_BANKS - 1;
            load_preset(active_bank, 0); 
            vTaskDelay(pdMS_TO_TICKS(300)); 
        } 
        else if (!b3 && !b4 && (last_state[2] || last_state[3])) {
            active_bank = (active_bank + 1) % NUM_BANKS;
            load_preset(active_bank, 0);
            vTaskDelay(pdMS_TO_TICKS(300)); 
        }
        else {
            bool current_state[4] = {b1, b2, b3, b4};
            for (int i = 0; i < 4; i++) {
                if (!current_state[i] && last_state[i]) { 
                    Preset current_p = banki[active_bank][active_preset_idx];
                    
                    if (current_p.button_flags[i] == 0) {
                        load_preset(active_bank, i);
                    } else if (current_p.button_flags[i] == 1) {
                        ESP_LOGI(TAG, "Przycisk %d działa jako STOMPBOX", i+1);
                    }
                }
            }
        }

        last_state[0] = b1; last_state[1] = b2; last_state[2] = b3; last_state[3] = b4;
        vTaskDelay(pdMS_TO_TICKS(20)); 
    }
}

void hw_init() {
    gpio_config_t btn_conf = {
        .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL<<BTN_1_PIN)|(1ULL<<BTN_2_PIN)|(1ULL<<BTN_3_PIN)|(1ULL<<BTN_4_PIN),
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn_conf);

    gpio_config_t shift_conf = {
        .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL<<SHIFT_DATA_PIN)|(1ULL<<SHIFT_CLOCK_PIN)|(1ULL<<SHIFT_LATCH_PIN),
    };
    gpio_config(&shift_conf);

    spi_bus_config_t buscfg = { .mosi_io_num = PIN_NUM_MOSI, .miso_io_num = -1, .sclk_io_num = PIN_NUM_CLK, .max_transfer_sz = 2 };
    spi_device_interface_config_t devcfg = { .clock_speed_hz = 1*1000*1000, .mode = 0, .spics_io_num = PIN_NUM_CS, .queue_size = 1 };
    spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    spi_bus_add_device(SPI2_HOST, &devcfg, &spi_max);
    
    max7219_send(0x0C, 0x01); 
    max7219_send(0x0B, 0x07); 
    max7219_send(0x09, 0x00); 
    max7219_send(0x0A, 0x08); 
    
    uart_config_t uart_cfg = { .baud_rate = 31250, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1 };
    uart_driver_install(MIDI_UART_TX, 256, 0, 0, NULL, 0);
    uart_param_config(MIDI_UART_TX, &uart_cfg);
    uart_set_pin(MIDI_UART_TX, MIDI_TX_PIN, -1, -1, -1);
    
    uart_driver_install(MIDI_UART_RX, 1024, 0, 0, NULL, 0);
    uart_param_config(MIDI_UART_RX, &uart_cfg);
    uart_set_pin(MIDI_UART_RX, -1, MIDI_RX_PIN, -1, -1);
}

void app_main(void) {
    hw_init();
    gpio_set_level(OE_PIN, 0);
    gpio_set_level(SRCLR_PIN, 1);

    strcpy(banki[0][0].name, "1A"); 
    banki[0][0].relay_flags = 0b0000000100000000; 
    banki[0][0].midi_pc = 10;
    banki[0][0].button_flags[0] = 0; 

    strcpy(banki[0][1].name, "1B"); 
    banki[0][1].relay_flags = 0b0000001000000000; 
    banki[0][1].midi_pc = 11;
    banki[0][1].button_flags[1] = 0;

    strcpy(banki[1][0].name, "1C"); 
    banki[1][0].relay_flags = 0b0000111100000000; 
    banki[1][0].midi_pc = 20;

    load_preset(0, 0);

    xTaskCreate(Handle_Buttons_Task, "buttons_task", 2048, NULL, 5, NULL);
    xTaskCreate(MIDI_RX, "midi_rx_task", 4096, NULL, 4, NULL);
}

//przy wciśnięciu 3 i 4 wyświetlacz robi się czarny - brak wyświetlanych nazw presetów
//lepiej zrobić strukturę presetów jako tablicę jednoowymiarową (można przypisać sobie przyciski np 5 i 6 do zmiany banków i wtedy po prostu zwiększasz lub zmniejszasz indeks o 4) przyciski 1-4 niech na razie będą tylko do zmiany presetów w obrębie banku, odpowiednio ABCD)
//z MIDI wgl jest coś nie tak, nawet tego nie testowałem - powinny być 2 pełne niezależne porty UART, każdy z RX i TX, 
//zmieniłem relay_flags na uint16_t (bo jest 14 przekaźników), poza tym przełączanie przekaźników nie działa - weź pod uwagę to, że 74HC595 ma jeszcze piny OE i SRCLR!!!

