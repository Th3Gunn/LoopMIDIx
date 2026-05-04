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

// ================= PINS =================
// buttons pins
#define BTN_1_PIN           47
#define BTN_2_PIN           16
#define BTN_3_PIN           20
#define BTN_4_PIN           38
#define BTN_5_PIN           21 // BANK DOWN
#define BTN_6_PIN           0  // BANK UP
#define BTN_7_PIN           19
#define BTN_8_PIN           45
#define BTN_9_PIN           13
#define BTN_10_PIN          14

// 74HC595 pins
#define SHIFT_DATA_PIN      1  // SER
#define SHIFT_CLOCK_PIN     2  // SRCLK
#define SHIFT_LATCH_PIN     41 // RCLK
#define OE_PIN              40 // ~OE (Output Enable) pin
#define SRCLR_PIN           42 // SRCLR (Serial Clear) pin

// display pins
#define PIN_NUM_MOSI        11
#define PIN_NUM_CLK         12
#define PIN_NUM_CS          10

// UART pins
#define MIDI_1_TX_PIN       15
#define MIDI_1_RX_PIN       7
#define MIDI_2_TX_PIN       5
#define MIDI_2_RX_PIN       6

// expression ADC pin
#define EXP_ADC_PIN         4

// amp switch pins
#define AMP_SWCH_R          48
#define AMP_SWCH_T          39

// ================= STRUCTURES & DATA =================
typedef struct {
    char name[9];
    uint16_t relay_flags;
    uint8_t midi_pc;
    uint8_t button_flags[4];
} Preset;

#define NUM_BANKS 5
#define PRESETS_PER_BANK 4
#define TOTAL_PRESETS (NUM_BANKS * PRESETS_PER_BANK)

Preset presety[TOTAL_PRESETS];
int active_bank = 0;
int active_preset_idx = 0; // Absolute index (0 to 19)

static spi_device_handle_t spi_max;

// ================= DISPLAY =================
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
    // Clear display first
    for(int i = 1; i <= 8; i++) max7219_send(i, 0x00);
    
    for (int i = 0; i < 8; i++) {
        if (name[i] == '\0') break;
        max7219_send(8 - i, get_char_segment(name[i])); 
    }
}

// ================= MIDI =================
static const char* midi_status_string(uint8_t status) {
    switch (status & 0xF0) {
        case 0x80: return "Note Off";
        case 0x90: return "Note On";
        case 0xA0: return "Poly Pressure";
        case 0xB0: return "Control Change";
        case 0xC0: return "Program Change";
        case 0xD0: return "Channel Pressure";
        case 0xE0: return "Pitch Bend";
        default:
            switch (status) {
                case 0xF0: return "SysEx Start";
                case 0xF1: return "Time Code";
                case 0xF2: return "Song Position";
                case 0xF3: return "Song Select";
                case 0xF6: return "Tune Request";
                case 0xF7: return "SysEx End";
                case 0xF8: return "Timing Clock";
                case 0xFA: return "Start";
                case 0xFB: return "Continue";
                case 0xFC: return "Stop";
                case 0xFE: return "Active Sensing";
                case 0xFF: return "Reset";
                default: return "Unknown";
            }
    }
}

static int midi_message_length(uint8_t status) {
    switch (status & 0xF0) {
        case 0xC0:
        case 0xD0:
            return 2;
        case 0x80:
        case 0x90:
        case 0xA0:
        case 0xB0:
        case 0xE0:
            return 3;
        default:
            switch (status) {
                case 0xF1: return 2;
                case 0xF2: return 3;
                case 0xF3: return 2;
                case 0xF6:
                case 0xF7:
                case 0xF8:
                case 0xFA:
                case 0xFB:
                case 0xFC:
                case 0xFE:
                case 0xFF:
                    return 1;
                default: return 1;
            }
    }
}

static void log_midi_bytes(const char *label, const uint8_t *data, int len) {
    int i = 0;
    while (i < len) {
        uint8_t status = data[i];
        if (status & 0x80) {
            int msg_len = midi_message_length(status);
            const char *type = midi_status_string(status);
            int channel = (status < 0xF0) ? (status & 0x0F) + 1 : -1;

            if (i + msg_len > len) {
                if (channel > 0) {
                    ESP_LOGI(TAG, "%s -> truncated status=0x%02X type=%s channel=%d bytes=%d",
                             label,
                             status,
                             type,
                             channel,
                             len - i);
                } else {
                    ESP_LOGI(TAG, "%s -> truncated status=0x%02X type=%s channel=n/a bytes=%d",
                             label,
                             status,
                             type,
                             len - i);
                }
                break;
            }

            if (msg_len == 1) {
                if (channel > 0) {
                    ESP_LOGI(TAG, "%s -> type=%s channel=%d",
                             label,
                             type,
                             channel);
                } else {
                    ESP_LOGI(TAG, "%s -> type=%s channel=n/a",
                             label,
                             type);
                }
            } else if (msg_len == 2) {
                if (channel > 0) {
                    ESP_LOGI(TAG, "%s -> type=%s channel=%d code0=%d",
                             label,
                             type,
                             channel,
                             data[i + 1]);
                } else {
                    ESP_LOGI(TAG, "%s -> type=%s channel=n/a code0=%d",
                             label,
                             type,
                             data[i + 1]);
                }
            } else {
                if (channel > 0) {
                    ESP_LOGI(TAG, "%s -> type=%s channel=%d code0=%d code1=%d",
                             label,
                             type,
                             channel,
                             data[i + 1],
                             data[i + 2]);
                } else {
                    ESP_LOGI(TAG, "%s -> type=%s channel=n/a code0=%d code1=%d",
                             label,
                             type,
                             data[i + 1],
                             data[i + 2]);
                }
            }
            i += msg_len;
        } else {
            ESP_LOGI(TAG, "%s -> running/data byte=0x%02X", label, status);
            i++;
        }
    }
}

void MIDI_TX(uart_port_t uart_num, uint8_t channel, uint8_t pc_value) {
    uint8_t status = 0xC0 | (channel & 0x0F);
    uint8_t msg[2] = { status, pc_value & 0x7F };
    uart_write_bytes(uart_num, (const char *)msg, 2);
    ESP_LOGI(TAG, "TX UART%d -> PC: %d na kanale %d", (uart_num == UART_NUM_1 ? 1 : 2), pc_value, channel + 1);
}

static void MIDI_RX(void *arg) {
    uint8_t data[128];
    while (1) {
        size_t len1 = 0;
        uart_get_buffered_data_len(UART_NUM_1, &len1);
        if (len1 > 0) {
            int rx1 = uart_read_bytes(UART_NUM_1, data, sizeof(data), pdMS_TO_TICKS(10));
            if (rx1 > 0) {
                ESP_LOGI(TAG, "RX1 -> Otrzymano %d bajtów MIDI", rx1);
                log_midi_bytes("RX1", data, rx1);
            }
        }

        size_t len2 = 0;
        uart_get_buffered_data_len(UART_NUM_2, &len2);
        if (len2 > 0) {
            int rx2 = uart_read_bytes(UART_NUM_2, data, sizeof(data), pdMS_TO_TICKS(10));
            if (rx2 > 0) {
                ESP_LOGI(TAG, "RX2 -> Otrzymano %d bajtów MIDI", rx2);
                log_midi_bytes("RX2", data, rx2);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10)); // Non-blocking delay
    }
}

// ================= RELAYS =================
extern void esp_rom_delay_us(uint32_t us);

void update_relays(uint16_t flags) {
    gpio_set_level(SHIFT_LATCH_PIN, 0);
    gpio_set_level(SHIFT_CLOCK_PIN, 0);
    
    // Shift 16 bits for two daisy-chained 74HC595
    for (int i = 0; i < 16; i++) {
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

// ================= LOGIC =================
void load_preset(int preset_idx) {
    if(preset_idx >= TOTAL_PRESETS || preset_idx < 0) return;

    active_preset_idx = preset_idx;
    active_bank = preset_idx / PRESETS_PER_BANK;
    
    Preset current = presety[preset_idx];

    update_relays(current.relay_flags);
    // Przykładowo wysyłamy sygnał na UART2, ale możesz dostosować
    MIDI_TX(UART_NUM_2, 0, current.midi_pc); 
    Display_preset_name(current.name);
}

static void Handle_Buttons_Task(void* arg) {
    bool last_state[6] = {true, true, true, true, true, true}; 
    
    while (1) {
        bool current_state[6];
        current_state[0] = gpio_get_level(BTN_1_PIN);
        current_state[1] = gpio_get_level(BTN_2_PIN);
        current_state[2] = gpio_get_level(BTN_3_PIN);
        current_state[3] = gpio_get_level(BTN_4_PIN);
        current_state[4] = gpio_get_level(BTN_5_PIN); // BANK DOWN
        current_state[5] = gpio_get_level(BTN_6_PIN); // BANK UP

        // BANK DOWN (BTN 5)
        if (!current_state[4] && last_state[4]) {
            active_bank = (active_bank > 0) ? active_bank - 1 : NUM_BANKS - 1;
            load_preset(active_bank * PRESETS_PER_BANK); // Wczytaj preset A z nowego banku
        } 
        // BANK UP (BTN 6)
        else if (!current_state[5] && last_state[5]) {
            active_bank = (active_bank + 1) % NUM_BANKS;
            load_preset(active_bank * PRESETS_PER_BANK);
        }
        // PRESETY 1-4 (A, B, C, D)
        else {
            for (int i = 0; i < 4; i++) {
                if (!current_state[i] && last_state[i]) { 
                    int target_preset_idx = (active_bank * PRESETS_PER_BANK) + i;
                    
                    Preset current_p = presety[active_preset_idx];
                    
                    if (current_p.button_flags[i] == 0) {
                        load_preset(target_preset_idx);
                    } else if (current_p.button_flags[i] == 1) {
                        ESP_LOGI(TAG, "Przycisk %d dziala jako STOMPBOX", i+1);
                    }
                }
            }
        }

        for(int i=0; i<6; i++) last_state[i] = current_state[i];
        vTaskDelay(pdMS_TO_TICKS(20)); 
    }
}

// ================= INIT =================
void hw_init() {
    // Buttons setup (1ULL pozwala na shift bitów powyżej 32, wymagane dla pinów ESP np. 47)
    gpio_config_t btn_conf = {
        .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL<<BTN_1_PIN)|(1ULL<<BTN_2_PIN)|(1ULL<<BTN_3_PIN)|
                        (1ULL<<BTN_4_PIN)|(1ULL<<BTN_5_PIN)|(1ULL<<BTN_6_PIN),
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn_conf);

    // Shift register setup - DODAŁEM OE I SRCLR
    gpio_config_t shift_conf = {
        .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL<<SHIFT_DATA_PIN)|(1ULL<<SHIFT_CLOCK_PIN)|
                        (1ULL<<SHIFT_LATCH_PIN)|(1ULL<<OE_PIN)|(1ULL<<SRCLR_PIN),
    };
    gpio_config(&shift_conf);

    // SPI init
    spi_bus_config_t buscfg = { .mosi_io_num = PIN_NUM_MOSI, .miso_io_num = -1, .sclk_io_num = PIN_NUM_CLK, .max_transfer_sz = 2 };
    spi_device_interface_config_t devcfg = { .clock_speed_hz = 1*1000*1000, .mode = 0, .spics_io_num = PIN_NUM_CS, .queue_size = 1 };
    spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    spi_bus_add_device(SPI2_HOST, &devcfg, &spi_max);
    
    max7219_send(0x0C, 0x01); 
    max7219_send(0x0B, 0x07); 
    max7219_send(0x09, 0x00); 
    max7219_send(0x0A, 0x08); 
    
    // UARTs init (2 pełne niezależne porty)
    uart_config_t uart_cfg = { .baud_rate = 31250, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1 };
    
    uart_driver_install(UART_NUM_1, 1024, 1024, 0, NULL, 0);
    uart_param_config(UART_NUM_1, &uart_cfg);
    uart_set_pin(UART_NUM_1, MIDI_1_TX_PIN, MIDI_1_RX_PIN, -1, -1);
    
    uart_driver_install(UART_NUM_2, 1024, 1024, 0, NULL, 0);
    uart_param_config(UART_NUM_2, &uart_cfg);
    uart_set_pin(UART_NUM_2, MIDI_2_TX_PIN, MIDI_2_RX_PIN, -1, -1);
}

void app_main(void) {
    hw_init();
    
    // Ustawienie układów 74HC595 do pracy (działa dzięki dodaniu ich do `shift_conf`)
    gpio_set_level(OE_PIN, 0);
    gpio_set_level(SRCLR_PIN, 1);

    // Czyszczenie całej tablicy, zapobiega wyświetlaniu śmieci z pamięci
    memset(presety, 0, sizeof(presety));

    // Preset 0 (Bank 0, A)
    strcpy(presety[0].name, "1A"); 
    presety[0].relay_flags = 0b0000000100000000; 
    presety[0].midi_pc = 10;
    presety[0].button_flags[0] = 0; 

    // Preset 1 (Bank 0, B)
    strcpy(presety[1].name, "1B"); 
    presety[1].relay_flags = 0b0010001000100000; 
    presety[1].midi_pc = 11;
    presety[1].button_flags[1] = 0;

    strcpy(presety[2].name, "1C"); 
    presety[2].relay_flags = 0b1110001000100000; 
    presety[2].midi_pc = 11;
    presety[2].button_flags[1] = 0;

    // Preset 4 (Bank 2, A)
    strcpy(presety[4].name, "2A"); 
    presety[4].relay_flags = 0b0000111100001000; 
    presety[4].midi_pc = 20;

    load_preset(0);

    xTaskCreate(Handle_Buttons_Task, "buttons_task", 2048, NULL, 5, NULL);
    xTaskCreate(MIDI_RX, "midi_rx_task", 4096, NULL, 4, NULL);
}

//teraz przełączanie relayów działa ale nie do końca - weź pod uwagę, że ostatnie 2 bity presety[].relay_flags odpowiadają za sterowanie 
//przekaźnikami od AMP_SWCH_R i AMP_SWCH_T - poza tym pod każdy 74hc595 są podpięte tylko po 6 przekaźników
//przyciski bank up/down powinny działać tak, że zmieniamy bank na wyższy ale jeszcze nie wybieramy presetu - zostajemy na starym i dopiero gdy wciśniemy przycisk 1-4 to wybieramy preset z nowego banku
//dodaj logi na konsoli o zmianie banku i presetu
//Można zacząć ogarniać EXP_ADC_PIN - czytanie z niego wartości z ADC (trzeba też to napisać tak żeby była możliwość połączenia tego z MIDI, np. wysyłanie komunikatu Control Change z wartością odczytaną z potencjometru) 
//więc trzeba będzie to skwantować na 128 poziomów (0-127) 

