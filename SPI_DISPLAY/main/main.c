#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h" 

static const char *TAG = "MIDI_SWITCHER";

// Hardware Pins
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

#define SHIFT_DATA_PIN      1  
#define SHIFT_CLOCK_PIN     2  
#define SHIFT_LATCH_PIN     41 
#define OE_PIN              40 
#define SRCLR_PIN           42 

#define PIN_NUM_MOSI        11
#define PIN_NUM_CLK         12
#define PIN_NUM_CS          10

#define MIDI_1_TX_PIN       15 
#define MIDI_1_RX_PIN       7  
#define MIDI_2_TX_PIN       5  
#define MIDI_2_RX_PIN       6  

#define EXP_ADC_PIN         4  
#define AMP_SWCH_R          48
#define AMP_SWCH_T          39

// Structures & Enums
typedef enum {
    BTN_MODE_NONE = 0,          
    BTN_MODE_SUB_PATCH,         
    BTN_MODE_TAP_TEMPO,         
    BTN_MODE_MOMENTARY,         
    BTN_MODE_STOMP_TOGGLE       
} ExtraButtonMode;

typedef enum {
    MIDI_MSG_NONE = 0,
    MIDI_MSG_PC,
    MIDI_MSG_CC,
    MIDI_MSG_NOTE_ON,
    MIDI_MSG_NOTE_OFF
} MidiMsgType;

typedef struct {
    uint8_t type;    
    uint8_t channel; 
    uint8_t data1;   
    uint8_t data2;   
} MidiMessageConfig;

typedef struct {
    char name[9];
    uint16_t relay_flags; 
    uint8_t button_flags[4];
    uint8_t extra_btn_modes[4]; 
    
    uint8_t exp_channel; 
    uint8_t exp_cc_num;  
    
    MidiMessageConfig m1_tx; 
    MidiMessageConfig m1_rx; 
    MidiMessageConfig m2_tx; 
    MidiMessageConfig m2_rx; 
} Preset;

#define NUM_BANKS 5
#define PRESETS_PER_BANK 4
#define TOTAL_PRESETS (NUM_BANKS * PRESETS_PER_BANK)

Preset presety[TOTAL_PRESETS];
int active_bank = 0;
int selected_bank = 0;
int active_preset_idx = 0; 

bool in_menu_mode = false;
int menu_current_item = 0; 
int menu_sub_step = 0;     
int menu_target_dir = 0;   
int menu_temp_val = 0;     

static spi_device_handle_t spi_max;
static adc_oneshot_unit_handle_t adc1_handle; 

// Function Prototypes
void load_preset(int preset_idx);
void update_menu_display(void);
void MIDI_TX(uart_port_t uart_num, uint8_t channel, uint8_t pc_value);

// Display MAX7219 Drivers
uint8_t get_char_segment(char c) {
    switch (toupper(c)) {
        case '0': return 0x7E; case '1': return 0x30; case '2': return 0x6D;
        case '3': return 0x79; case '4': return 0x33; case '5': return 0x5B;
        case '6': return 0x5F; case '7': return 0x70; case '8': return 0x7F;
        case '9': return 0x7B; 
        case 'A': return 0x77; case 'B': return 0x1F; 
        case 'C': return 0x4E; case 'D': return 0x3D; 
        case 'E': return 0x4F; case 'F': return 0x47;
        case 'L': return 0x0E; case 'P': return 0x67;
        case 'S': return 0x5B; case 'U': return 0x3E;
        case 'H': return 0x37; case 'X': return 0x37;
        case 'M': return 0x76; case 'T': return 0x0F;
        case 'R': return 0x05; case 'N': return 0x37; 
        case 'O': return 0x7E; case 'Y': return 0x3B;
        case '-': return 0x01; case '_': return 0x08;
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

// MIDI Transmitter
void MIDI_TX(uart_port_t uart_num, uint8_t channel, uint8_t pc_value) {
    uint8_t status = 0xC0 | (channel & 0x0F);
    uint8_t msg[2] = { status, pc_value & 0x7F };
    uart_write_bytes(uart_num, (const char *)msg, 2);
    ESP_LOGI(TAG, "TX UART%d -> PC: %d na kanale %d", (uart_num == UART_NUM_1 ? 1 : 2), pc_value, channel + 1);
}

void send_flexible_midi(uart_port_t uart_num, MidiMessageConfig cfg) {
    if (cfg.type == MIDI_MSG_NONE) return;
    uint8_t status = 0;
    uint8_t msg[3];
    int len = 0;

    switch(cfg.type) {
        case MIDI_MSG_PC:
            status = 0xC0 | (cfg.channel & 0x0F);
            msg[0] = status; msg[1] = cfg.data1 & 0x7F; len = 2;
            ESP_LOGI(TAG, "TX UART%d -> PC: %d, Ch: %d", (uart_num == UART_NUM_1 ? 1 : 2), cfg.data1, cfg.channel + 1);
            break;
        case MIDI_MSG_CC:
            status = 0xB0 | (cfg.channel & 0x0F);
            msg[0] = status; msg[1] = cfg.data1 & 0x7F; msg[2] = cfg.data2 & 0x7F; len = 3;
            ESP_LOGI(TAG, "TX UART%d -> CC: #%d, Val: %d, Ch: %d", (uart_num == UART_NUM_1 ? 1 : 2), cfg.data1, cfg.data2, cfg.channel + 1);
            break;
        case MIDI_MSG_NOTE_ON:
            status = 0x90 | (cfg.channel & 0x0F);
            msg[0] = status; msg[1] = cfg.data1 & 0x7F; msg[2] = cfg.data2 & 0x7F; len = 3;
            break;
        case MIDI_MSG_NOTE_OFF:
            status = 0x80 | (cfg.channel & 0x0F);
            msg[0] = status; msg[1] = cfg.data1 & 0x7F; msg[2] = cfg.data2 & 0x7F; len = 3;
            break;
    }
    if (len > 0) uart_write_bytes(uart_num, (const char *)msg, len);
}

void MIDI_CC_TX(uart_port_t uart_num, uint8_t channel, uint8_t cc_num, uint8_t cc_value) {
    uint8_t status = 0xB0 | (channel & 0x0F);
    uint8_t msg[3] = { status, cc_num & 0x7F, cc_value & 0x7F };
    uart_write_bytes(uart_num, (const char *)msg, 3);
}

// MIDI Receiver Parser (with echo filtering)
void process_midi_byte(uint8_t byte, uart_port_t uart_num) {
    static uint8_t status[2] = {0, 0};
    static uint8_t data1[2] = {0, 0};
    static int state[2] = {0, 0}; 
    int idx = (uart_num == UART_NUM_1) ? 0 : 1;

    if (byte >= 0x80) { status[idx] = byte; state[idx] = 1; } 
    else { 
        if (state[idx] == 1) {
            data1[idx] = byte;
            uint8_t cmd = status[idx] & 0xF0;
            if (cmd == 0xC0) { 
                uint8_t ch = status[idx] & 0x0F;
                ESP_LOGI(TAG, "[RX%d - MINI JACK] Przechwycono PC: %d na kanale %d (Echo odfiltrowane)", idx + 1, data1[idx], ch + 1);

                for (int i = 0; i < TOTAL_PRESETS; i++) {
                    MidiMessageConfig rx_cfg = (uart_num == UART_NUM_1) ? presety[i].m1_rx : presety[i].m2_rx;
                    if (rx_cfg.type == MIDI_MSG_PC && rx_cfg.channel == ch && rx_cfg.data1 == data1[idx]) {
                        if (i == active_preset_idx) {
                            break; 
                        }
                        ESP_LOGW(TAG, "[RX%d - MINI JACK] MATCH! Zdalna zmiana na preset: %s", idx + 1, presety[i].name);
                        load_preset(i); break;
                    }
                }
                state[idx] = 0; 
            } else if (cmd == 0xD0) { state[idx] = 0; } 
            else { state[idx] = 2; }
        } else if (state[idx] == 2) {
            uint8_t data2 = byte;
            uint8_t cmd = status[idx] & 0xF0;
            uint8_t ch = status[idx] & 0x0F;
            uint8_t target_type = (cmd == 0xB0) ? MIDI_MSG_CC : ((cmd == 0x90) ? MIDI_MSG_NOTE_ON : MIDI_MSG_NOTE_OFF);
            
            if (cmd == 0xB0) {
                ESP_LOGI(TAG, "[RX%d - MINI JACK] Przechwycono CC: #%d, Wartość: %d na kanale %d", idx + 1, data1[idx], data2, ch + 1);
            }

            for (int i = 0; i < TOTAL_PRESETS; i++) {
                MidiMessageConfig rx_cfg = (uart_num == UART_NUM_1) ? presety[i].m1_rx : presety[i].m2_rx;
                if (rx_cfg.type == target_type && rx_cfg.channel == ch && rx_cfg.data1 == data1[idx] && rx_cfg.data2 == data2) {
                    if (i == active_preset_idx) {
                        break; 
                    }
                    ESP_LOGW(TAG, "[RX%d - MINI JACK] MATCH (CC/NOTE)! Zdalna zmiana na preset: %s", idx + 1, presety[i].name);
                    load_preset(i); break;
                }
            }
            state[idx] = 0; 
        }
    }
}

static void MIDI_RX_Task(void *arg) {
    uint8_t byte;
    while (1) {
        while (uart_read_bytes(UART_NUM_1, &byte, 1, 0) > 0) process_midi_byte(byte, UART_NUM_1);
        while (uart_read_bytes(UART_NUM_2, &byte, 1, 0) > 0) process_midi_byte(byte, UART_NUM_2);
        vTaskDelay(1); 
    }
}

// Relays & Amp Switches
extern void esp_rom_delay_us(uint32_t us);
void update_relays(uint16_t flags) {
    gpio_set_level(AMP_SWCH_R, (flags >> 0) & 0x1); 
    gpio_set_level(AMP_SWCH_T, (flags >> 1) & 0x1); 
    gpio_set_level(SHIFT_LATCH_PIN, 0); gpio_set_level(SHIFT_CLOCK_PIN, 0);
    for (int i = 0; i < 16; i++) {
        gpio_set_level(SHIFT_DATA_PIN, (flags >> i) & 0x1); gpio_set_level(SHIFT_CLOCK_PIN, 1);
        esp_rom_delay_us(1); gpio_set_level(SHIFT_CLOCK_PIN, 0); esp_rom_delay_us(1);
    }
    gpio_set_level(SHIFT_LATCH_PIN, 1); esp_rom_delay_us(1); gpio_set_level(SHIFT_LATCH_PIN, 0);
}

// Switcher Logic
void load_preset(int preset_idx) {
    if(preset_idx >= TOTAL_PRESETS || preset_idx < 0) return;
    active_preset_idx = preset_idx; active_bank = preset_idx / PRESETS_PER_BANK; selected_bank = active_bank; 
    Preset current = presety[preset_idx];
    
    update_relays(current.relay_flags);
    send_flexible_midi(UART_NUM_1, current.m1_tx);
    send_flexible_midi(UART_NUM_2, current.m2_tx);
    Display_preset_name(current.name);
}

// On-Device Menu Interface (8-char limit)
void update_menu_display(void) {
    char buf[32]; 
    
    if (menu_sub_step == 0) {
        const char* menu_names[] = {
            "1-L00PS ", 
            "2-A-SWCH", 
            "3-M1D1-1", 
            "4-M1D1-2", 
            "5-EXP   "  
        };
        Display_preset_name(menu_names[menu_current_item]);
        return;
    }
    
    if (menu_current_item == 0) { 
        snprintf(buf, sizeof(buf), "L   %04X", (menu_temp_val >> 4)); 
        Display_preset_name(buf); 
    }
    else if (menu_current_item == 1) {
        switch(menu_temp_val) {
            case 0:  Display_preset_name("A   N0NE"); break; 
            case 1:  Display_preset_name("A     _R"); break; 
            case 2:  Display_preset_name("A     T_"); break; 
            case 3:  Display_preset_name("A    T_R"); break; 
            default: break;
        }
    }
    else if (menu_current_item == 2 || menu_current_item == 3) { 
        char p_char = (menu_current_item == 2) ? '1' : '2'; 
        char* dir_str = (menu_target_dir == 0) ? "RX" : "TX"; 
        
        if (menu_sub_step == 1) { 
            snprintf(buf, sizeof(buf), "%c-%s D1R", p_char, dir_str); 
            Display_preset_name(buf); 
        }
        else if (menu_sub_step == 2) {
            if (menu_temp_val == MIDI_MSG_NONE)    snprintf(buf, sizeof(buf), "%c-TP N0N", p_char);
            if (menu_temp_val == MIDI_MSG_PC)      snprintf(buf, sizeof(buf), "%c-TP  PC", p_char);
            if (menu_temp_val == MIDI_MSG_CC)      snprintf(buf, sizeof(buf), "%c-TP  CC", p_char);
            if (menu_temp_val == MIDI_MSG_NOTE_ON) snprintf(buf, sizeof(buf), "%c-TP N0T", p_char);
            Display_preset_name(buf);
        }
        else if (menu_sub_step == 3) { 
            snprintf(buf, sizeof(buf), "%c-CH  %02d", p_char, menu_temp_val + 1); 
            Display_preset_name(buf); 
        }
        else if (menu_sub_step == 4) { 
            snprintf(buf, sizeof(buf), "%c-C1 %03d", p_char, menu_temp_val); 
            Display_preset_name(buf); 
        }
        else if (menu_sub_step == 5) { 
            snprintf(buf, sizeof(buf), "%c-C2 %03d", p_char, menu_temp_val); 
            Display_preset_name(buf); 
        }
    }
    else if (menu_current_item == 4) { 
        if (menu_sub_step == 1) { 
            snprintf(buf, sizeof(buf), "E-CH  %02d", menu_temp_val + 1); 
            Display_preset_name(buf); 
        }
        else if (menu_sub_step == 2) { 
            snprintf(buf, sizeof(buf), "E-CC %03d", menu_temp_val); 
            Display_preset_name(buf); 
        }
    }
}

// Button Matrix State Machine Task
static void Handle_Buttons_Task(void* arg) {
    vTaskDelay(pdMS_TO_TICKS(500));
    const uint8_t pins[10] = { BTN_1_PIN, BTN_2_PIN, BTN_3_PIN, BTN_4_PIN, BTN_5_PIN, BTN_6_PIN, BTN_7_PIN, BTN_8_PIN, BTN_9_PIN, BTN_10_PIN };
    bool last_state[10]; TickType_t press_time[10] = {0}; bool long_press_triggered[10] = {false}; static bool stomp_states[4] = {false}; 
    TickType_t menu_hold_start = 0; bool menu_hold_active = false; char preview_name[32];

    for(int i = 0; i < 10; i++) last_state[i] = gpio_get_level(pins[i]);

    while (1) {
        bool current_state[10]; TickType_t now = xTaskGetTickCount();
        for(int i = 0; i < 10; i++) current_state[i] = gpio_get_level(pins[i]);

        if (!current_state[4] && current_state[5] == 0) {
            if (!menu_hold_active) { menu_hold_start = now; menu_hold_active = true; }
            else if ((now - menu_hold_start) > pdMS_TO_TICKS(1000)) {
                in_menu_mode = !in_menu_mode; menu_current_item = 0; menu_sub_step = 0; menu_hold_active = false;
                if (in_menu_mode) update_menu_display();
                else if (active_preset_idx >= 0) load_preset(active_preset_idx);
                while(gpio_get_level(pins[4]) == 0 || gpio_get_level(pins[5]) == 0) vTaskDelay(pdMS_TO_TICKS(50));
                for(int i = 0; i < 10; i++) current_state[i] = gpio_get_level(pins[i]);
            }
        } else { menu_hold_active = false; }

        if (in_menu_mode && active_preset_idx >= 0) {
            Preset *edit_p = &presety[active_preset_idx];
            MidiMessageConfig *msg = (menu_current_item == 2) ? 
                (menu_target_dir == 0 ? &edit_p->m1_rx : &edit_p->m1_tx) : 
                (menu_target_dir == 0 ? &edit_p->m2_rx : &edit_p->m2_tx);

            // BTN 6 (BANK UP)
            if (!current_state[5] && last_state[5]) {
                if (menu_sub_step == 0) {
                    menu_current_item = (menu_current_item + 1) % 5;
                } else {
                    if (menu_current_item == 0) { 
                        menu_temp_val = (menu_temp_val + 16) & 0xFFF0;
                    } 
                    else if (menu_current_item == 1) { 
                        menu_temp_val = (menu_temp_val + 1) % 4;
                    } 
                    else if (menu_current_item == 2 || menu_current_item == 3) { 
                        if (menu_sub_step == 1)      menu_target_dir = (menu_target_dir + 1) % 2;
                        else if (menu_sub_step == 2) menu_temp_val = (menu_temp_val + 1) % 5;
                        else if (menu_sub_step == 3) menu_temp_val = (menu_temp_val + 1) % 16;
                        else if ((menu_sub_step == 4 || menu_sub_step == 5) && menu_temp_val < 127) menu_temp_val++;
                    } 
                    else if (menu_current_item == 4) { 
                        if (menu_sub_step == 1)      menu_temp_val = (menu_temp_val + 1) % 16; 
                        else if (menu_sub_step == 2 && menu_temp_val < 127) menu_temp_val++;   
                    }
                }
                update_menu_display();
            }

            // BTN 5 (BANK DOWN)
            if (!current_state[4] && last_state[4]) {
                if (menu_sub_step == 0) {
                    menu_current_item = (menu_current_item > 0) ? menu_current_item - 1 : 4;
                } else {
                    if (menu_current_item == 0) { 
                        menu_temp_val = (menu_temp_val - 16) & 0xFFF0;
                    } 
                    else if (menu_current_item == 1) { 
                        menu_temp_val = (menu_temp_val > 0) ? menu_temp_val - 1 : 3;
                    } 
                    else if (menu_current_item == 2 || menu_current_item == 3) { 
                        if (menu_sub_step == 1)      menu_target_dir = (menu_target_dir > 0) ? menu_target_dir - 1 : 1;
                        else if (menu_sub_step == 2) menu_temp_val = (menu_temp_val > 0) ? menu_temp_val - 1 : 4;
                        else if (menu_sub_step == 3) menu_temp_val = (menu_temp_val > 0) ? menu_temp_val - 1 : 15;
                        else if ((menu_sub_step == 4 || menu_sub_step == 5) && menu_temp_val > 0) menu_temp_val--;
                    } 
                    else if (menu_current_item == 4) { 
                        if (menu_sub_step == 1)      menu_temp_val = (menu_temp_val > 0) ? menu_temp_val - 1 : 15; 
                        else if (menu_sub_step == 2 && menu_temp_val > 0) menu_temp_val--;                         
                    }
                }
                update_menu_display();
            }

            // BTN 1 (PRESET A - ENTER)
            if (!current_state[0] && last_state[0]) {
                if (menu_sub_step == 0) { 
                    menu_sub_step = 1;
                    if (menu_current_item == 0) menu_temp_val = edit_p->relay_flags & 0xFFF0;
                    if (menu_current_item == 1) menu_temp_val = edit_p->relay_flags & 0x0003;
                    if (menu_current_item == 2 || menu_current_item == 3) menu_target_dir = 0; 
                    if (menu_current_item == 4) menu_temp_val = edit_p->exp_channel;
                } else {
                    if (menu_current_item == 0) { edit_p->relay_flags = (edit_p->relay_flags & 0x000F) | (menu_temp_val & 0xFFF0); menu_sub_step = 0; }
                    else if (menu_current_item == 1) { edit_p->relay_flags = (edit_p->relay_flags & 0xFFFC) | (menu_temp_val & 0x0003); menu_sub_step = 0; }
                    else if (menu_current_item == 2 || menu_current_item == 3) { 
                        if (menu_sub_step == 1) { menu_sub_step = 2; menu_temp_val = msg->type; }
                        else if (menu_sub_step == 2) { msg->type = menu_temp_val; if(menu_temp_val == MIDI_MSG_NONE) menu_sub_step = 0; else { menu_sub_step = 3; menu_temp_val = msg->channel; } }
                        else if (menu_sub_step == 3) { msg->channel = menu_temp_val; menu_sub_step = 4; menu_temp_val = msg->data1; }
                        else if (menu_sub_step == 4) { msg->data1 = menu_temp_val; if(msg->type == MIDI_MSG_PC) menu_sub_step = 0; else { menu_sub_step = 5; menu_temp_val = msg->data2; } }
                        else if (menu_sub_step == 5) { msg->data2 = menu_temp_val; menu_sub_step = 0; }
                    }
                    else if (menu_current_item == 4) { 
                        if (menu_sub_step == 1) { edit_p->exp_channel = menu_temp_val; menu_sub_step = 2; menu_temp_val = edit_p->exp_cc_num; }
                        else if (menu_sub_step == 2) { edit_p->exp_cc_num = menu_temp_val; menu_sub_step = 0; }
                    }
                }
                update_menu_display();
            }

            // BTN 2 (PRESET B - BACK)
            if (!current_state[1] && last_state[1]) {
                if (menu_sub_step > 0) menu_sub_step--; 
                else in_menu_mode = false; 
                update_menu_display();
            }

        } else {
            // Live Performance Mode
            if (!current_state[4] && last_state[4]) {
                selected_bank = (selected_bank > 0) ? selected_bank - 1 : NUM_BANKS - 1;
                if (active_preset_idx >= 0) {
                    snprintf(preview_name, sizeof(preview_name), "%s     %d", presety[active_preset_idx].name, selected_bank + 1);
                } else {
                    snprintf(preview_name, sizeof(preview_name), "--     %d", selected_bank + 1);
                }
                Display_preset_name(preview_name);
            } 
            if (!current_state[5] && last_state[5]) {
                selected_bank = (selected_bank + 1) % NUM_BANKS;
                if (active_preset_idx >= 0) {
                    snprintf(preview_name, sizeof(preview_name), "%s     %d", presety[active_preset_idx].name, selected_bank + 1);
                } else {
                    snprintf(preview_name, sizeof(preview_name), "--     %d", selected_bank + 1);
                }
                Display_preset_name(preview_name);
            }
            
            for (int i = 0; i < 4; i++) {
                if (!current_state[i] && last_state[i]) { 
                    int target_preset_idx = (selected_bank * PRESETS_PER_BANK) + i;
                    if (target_preset_idx == active_preset_idx) { active_preset_idx = -1; update_relays(0); Display_preset_name("  --  "); }
                    else { load_preset(target_preset_idx); }
                }
            }
            if (active_preset_idx >= 0) {
                Preset current_p = presety[active_preset_idx];
                for (int b = 6; b < 10; b++) {
                    int btn_idx = b - 6; uint8_t mode = current_p.extra_btn_modes[btn_idx]; if (mode == BTN_MODE_NONE) continue;
                    switch (mode) {
                        case BTN_MODE_SUB_PATCH:
                            if (!current_state[b] && last_state[b]) { press_time[b] = now; long_press_triggered[b] = false; }
                            if (!current_state[b] && !long_press_triggered[b]) { if ((now - press_time[b]) > pdMS_TO_TICKS(600)) { MIDI_TX(UART_NUM_2, 0, 50); long_press_triggered[b] = true; } }
                            if (current_state[b] && !last_state[b]) { if (!long_press_triggered[b]) MIDI_CC_TX(UART_NUM_2, 0, 20, 127); } break;
                        case BTN_MODE_TAP_TEMPO: if (!current_state[b] && last_state[b]) MIDI_CC_TX(UART_NUM_2, 0, 64, 127); break;
                        case BTN_MODE_MOMENTARY:
                            if (!current_state[b] && last_state[b]) { MIDI_CC_TX(UART_NUM_2, 0, 21, 127); update_relays(current_p.relay_flags | 0b0000000000010000); }
                            if (current_state[b] && !last_state[b]) { MIDI_CC_TX(UART_NUM_2, 0, 21, 0); update_relays(current_p.relay_flags); } break;
                        case BTN_MODE_STOMP_TOGGLE: if (!current_state[b] && last_state[b]) { stomp_states[btn_idx] = !stomp_states[btn_idx]; MIDI_CC_TX(UART_NUM_2, 0, 22, stomp_states[btn_idx] ? 127 : 0); } break;
                    }
                }
            }
        }
        for(int i = 0; i < 10; i++) last_state[i] = current_state[i];
        vTaskDelay(pdMS_TO_TICKS(20)); 
    }
}

// Expression Pedal Monitor Task
static void Expression_Pedal_Task(void* arg) {
    int raw_val = 0; int last_cc_val = -1;
    while(1) {
        if (adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &raw_val) == ESP_OK) {
            int current_cc_val = (raw_val * 127) / 4095;
            if (current_cc_val != last_cc_val) {
                last_cc_val = current_cc_val;
                uint8_t target_ch = (active_preset_idx >= 0) ? presety[active_preset_idx].exp_channel : 0;
                uint8_t target_cc = (active_preset_idx >= 0) ? presety[active_preset_idx].exp_cc_num : 11;
                
                uint8_t status = 0xB0 | (target_ch & 0x0F);
                uint8_t msg[3] = { status, target_cc & 0x7F, current_cc_val & 0x7F };
                
                ESP_LOGI(TAG, "TX UART2 -> CC: #%d, Val: %d, Ch: %d", target_cc, current_cc_val, target_ch + 1);
                uart_write_bytes(UART_NUM_2, (const char *)msg, 3);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50)); 
    }
}

// Hardware Low-Level Init
void hw_init() {
    gpio_config_t btn_conf = { .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_INPUT, .pin_bit_mask = (1ULL<<BTN_1_PIN)|(1ULL<<BTN_2_PIN)|(1ULL<<BTN_3_PIN)|(1ULL<<BTN_4_PIN)|(1ULL<<BTN_5_PIN)|(1ULL<<BTN_6_PIN)|(1ULL<<BTN_7_PIN)|(1ULL<<BTN_8_PIN)|(1ULL<<BTN_9_PIN)|(1ULL<<BTN_10_PIN), .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&btn_conf);
    gpio_config_t shift_conf = { .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = (1ULL<<SHIFT_DATA_PIN)|(1ULL<<SHIFT_CLOCK_PIN)|(1ULL<<SHIFT_LATCH_PIN)|(1ULL<<OE_PIN)|(1ULL<<SRCLR_PIN) };
    gpio_config(&shift_conf);
    gpio_config_t amp_conf = { .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = (1ULL<<AMP_SWCH_R) | (1ULL<<AMP_SWCH_T) };
    gpio_config(&amp_conf);

    spi_bus_config_t buscfg = { .mosi_io_num = PIN_NUM_MOSI, .miso_io_num = -1, .sclk_io_num = PIN_NUM_CLK, .max_transfer_sz = 2 };
    spi_device_interface_config_t devcfg = { .clock_speed_hz = 1*1000*1000, .mode = 0, .spics_io_num = PIN_NUM_CS, .queue_size = 1 };
    spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO); spi_bus_add_device(SPI2_HOST, &devcfg, &spi_max);
    max7219_send(0x0C, 0x01); max7219_send(0x0B, 0x07); max7219_send(0x09, 0x00); max7219_send(0x0A, 0x08); max7219_send(0x0F, 0x00); 
    
    uart_config_t uart_cfg = { .baud_rate = 31250, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1 };
    uart_driver_install(UART_NUM_1, 1024, 1024, 0, NULL, 0); uart_param_config(UART_NUM_1, &uart_cfg); uart_set_pin(UART_NUM_1, MIDI_1_TX_PIN, MIDI_1_RX_PIN, -1, -1);
    uart_driver_install(UART_NUM_2, 1024, 1024, 0, NULL, 0); uart_param_config(UART_NUM_2, &uart_cfg); uart_set_pin(UART_NUM_2, MIDI_2_TX_PIN, MIDI_2_RX_PIN, -1, -1);

    adc_oneshot_unit_init_cfg_t init_config1 = { .unit_id = ADC_UNIT_1 }; adc_oneshot_new_unit(&init_config1, &adc1_handle);
    adc_oneshot_chan_cfg_t adc_config = { .bitwidth = ADC_BITWIDTH_12, .atten = ADC_ATTEN_DB_12 }; adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &adc_config);
}

void app_main(void) {
    hw_init();
    gpio_set_level(OE_PIN, 0); gpio_set_level(SRCLR_PIN, 1);
    memset(presety, 0, sizeof(presety));

    for (int b = 0; b < NUM_BANKS; b++) {
        for (int p = 0; p < PRESETS_PER_BANK; p++) {
            int idx = (b * PRESETS_PER_BANK) + p;
            snprintf(presety[idx].name, sizeof(presety[idx].name), "%d%c", b + 1, 'A' + p);
            presety[idx].relay_flags = (1 << (15 - p)); 
            
            presety[idx].m1_tx.type = MIDI_MSG_PC; presety[idx].m1_tx.channel = 0; presety[idx].m1_tx.data1 = 10 + idx;
            presety[idx].m1_rx.type = MIDI_MSG_PC; presety[idx].m1_rx.channel = 0; presety[idx].m1_rx.data1 = 10 + idx;
            presety[idx].m2_tx.type = MIDI_MSG_PC; presety[idx].m2_tx.channel = 0; presety[idx].m2_tx.data1 = 10 + idx;
            presety[idx].m2_rx.type = MIDI_MSG_PC; presety[idx].m2_rx.channel = 0; presety[idx].m2_rx.data1 = 10 + idx;

            presety[idx].extra_btn_modes[0] = BTN_MODE_SUB_PATCH;   
            presety[idx].extra_btn_modes[1] = BTN_MODE_TAP_TEMPO;   
            presety[idx].extra_btn_modes[2] = BTN_MODE_MOMENTARY;   
            presety[idx].extra_btn_modes[3] = BTN_MODE_STOMP_TOGGLE; 
            
            presety[idx].exp_channel = 0; 
            presety[idx].exp_cc_num = 11; 
        }
    }

    load_preset(0);
    xTaskCreate(Handle_Buttons_Task, "buttons_task", 4096, NULL, 5, NULL);
    xTaskCreate(MIDI_RX_Task, "midi_rx_task", 4096, NULL, 4, NULL); 
    xTaskCreate(Expression_Pedal_Task, "exp_pedal_task", 4096, NULL, 4, NULL); 
}