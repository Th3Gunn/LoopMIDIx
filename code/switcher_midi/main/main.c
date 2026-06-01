#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h" 
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"
        
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
    MIDI_MSG_CC
} MidiMsgType;

typedef struct {
    uint8_t type;    
    uint8_t channel; 
    uint8_t data1;   
    uint8_t data2;   
    uint8_t is_tx;    // 1 = TX (Wysyłana), 0 = RX (Odbierana/Aktywująca)
    uint8_t uart_num; // 1 = UART_NUM_1, 2 = UART_NUM_2, 3 = USB MIDI
} MidiMessageConfig;


typedef struct {
    uint16_t relay_flags; 
    uint8_t button_flags[4];
    uint8_t extra_btn_modes[4]; 
    uint8_t exp_channel; 
    uint8_t exp_cc_num;  
    MidiMessageConfig midi_msgs[10]; 
} Preset;

#define NUM_BANKS 128
#define PRESETS_PER_BANK 4
#define TOTAL_PRESETS (NUM_BANKS * PRESETS_PER_BANK)

Preset presety[TOTAL_PRESETS];
int active_preset_idx = 0; 
uint8_t active_bank = 0;
uint8_t selected_bank = 0;

bool in_menu_mode = false;
uint8_t menu_current_item = 0; 
uint8_t menu_sub_step = 0;     
int menu_temp_val = 0;     
uint8_t menu_bit_cursor = 0; 

static spi_device_handle_t spi_max;
static adc_oneshot_unit_handle_t adc1_handle; 

#define MIDI_SOURCE_UART1 1
#define MIDI_SOURCE_UART2 2
#define MIDI_SOURCE_USB   3

#if CONFIG_TINYUSB_ENABLED && CFG_TUD_MIDI
enum {
    USB_MIDI_ITF_NUM = 0,
    USB_MIDI_ITF_STREAMING,
    USB_MIDI_ITF_COUNT
};

enum {
    USB_MIDI_EP_NUM = 0,
};

#define USB_MIDI_DESCRIPTOR_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_MIDI * TUD_MIDI_DESC_LEN)

static const char *usb_midi_string_desc[] = {
    (char[]){0x09, 0x04},
    "Switcher MIDI",
    "Switcher MIDI Device",
    "123456",
    "USB MIDI",
};

static const uint8_t usb_midi_fs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, USB_MIDI_ITF_COUNT, 0, USB_MIDI_DESCRIPTOR_TOTAL_LEN, 0, 100),
    TUD_MIDI_DESCRIPTOR(USB_MIDI_ITF_NUM, 4, USB_MIDI_EP_NUM, (0x80 | USB_MIDI_EP_NUM), 64),
};

#if (TUD_OPT_HIGH_SPEED)
static const uint8_t usb_midi_hs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, USB_MIDI_ITF_COUNT, 0, USB_MIDI_DESCRIPTOR_TOTAL_LEN, 0, 100),
    TUD_MIDI_DESCRIPTOR(USB_MIDI_ITF_NUM, 4, USB_MIDI_EP_NUM, (0x80 | USB_MIDI_EP_NUM), 512),
};
#endif
#endif

// Function Prototypes
void load_preset(int preset_idx);
void update_menu_display(void);
void update_live_display(void);
void MIDI_TX(uart_port_t uart_num, uint8_t channel, uint8_t pc_value);
void save_preset_to_flash(int idx);
void load_presets_from_flash(void);
static void midi_write_bytes(uint8_t target, const uint8_t *msg, size_t len);
static void process_midi_byte(uint8_t byte, uint8_t midi_source);

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
        case 'I': return 0x04;
        case 'L': return 0x0E; case 'P': return 0x67;
        case 'S': return 0x5B; case 'U': return 0x1C;
        case 'H': return 0x17; case 'X': return 0x37;
        case 'M': return 0x76; case 'T': return 0x0F;
        case 'R': return 0x05; case 'N': return 0x15; 
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

void display_menu_text(const char *text, int cursor_pos) {
    for(int i = 1; i <= 8; i++) max7219_send(i, 0x00);
    for (int i = 0; i < 8 && text[i] != '\0'; i++) {
        uint8_t seg = get_char_segment(text[i]);
        if (i == cursor_pos) seg |= 0x80;
        max7219_send(8 - i, seg);
    }
}

void update_live_display(void) {
    uint8_t a_bank = 0;
    char a_let = ' ';
    uint8_t b_val = (selected_bank + 1) ;
    
    if (active_preset_idx >= 0) {
        a_bank = ((active_preset_idx / PRESETS_PER_BANK) + 1);
        a_let = 'A' + (active_preset_idx % PRESETS_PER_BANK);
    }

    char buf[9];
    if (active_preset_idx >= 0) {
        snprintf(buf, sizeof(buf), "%3d%c%4d", a_bank, a_let, b_val);
    } else {
        snprintf(buf, sizeof(buf), "--- %4d", b_val);
    }

    display_menu_text(buf, -1);
}

// MIDI Transmitters
static const char *midi_source_name(uint8_t midi_source)
{
    switch (midi_source) {
        case MIDI_SOURCE_UART1: return "UART1";
        case MIDI_SOURCE_UART2: return "UART2";
        case MIDI_SOURCE_USB: return "USB";
        default: return "UNKNOWN";
    }
}

static void midi_write_bytes(uint8_t target, const uint8_t *msg, size_t len)
{
    if (target == MIDI_SOURCE_USB) {
#if CONFIG_TINYUSB_ENABLED && CFG_TUD_MIDI
        if (tud_midi_mounted()) {
            tud_midi_stream_write(0, msg, len);
        } else {
            ESP_LOGW(TAG, "USB MIDI not mounted, dropping %u bytes", (unsigned)len);
        }
#else
        ESP_LOGW(TAG, "USB MIDI support is disabled at build time");
#endif
        return;
    }

    uart_port_t uart_port = (target == MIDI_SOURCE_UART2) ? UART_NUM_2 : UART_NUM_1;
    uart_write_bytes(uart_port, (const char *)msg, len);
}

void MIDI_TX(uart_port_t uart_num, uint8_t channel, uint8_t pc_value) {
    uint8_t status = 0xC0 | (channel & 0x0F);
    uint8_t msg[2] = { status, pc_value & 0x7F };
    midi_write_bytes((uint8_t)uart_num, msg, 2);
}

void send_flexible_midi(uint8_t midi_target, MidiMessageConfig cfg) {
    if (cfg.type == MIDI_MSG_NONE) return;
    uint8_t status = 0;
    uint8_t msg[3];
    int len = 0;

    switch(cfg.type) {
        case MIDI_MSG_PC:
            status = 0xC0 | (cfg.channel & 0x0F);
            msg[0] = status; msg[1] = cfg.data1 & 0x7F; len = 2;
            ESP_LOGI(TAG, "TX %s -> PC: %d, Ch: %d", midi_source_name(midi_target), cfg.data1, cfg.channel + 1);
            break;
        case MIDI_MSG_CC:
            status = 0xB0 | (cfg.channel & 0x0F);
            msg[0] = status; msg[1] = cfg.data1 & 0x7F; msg[2] = cfg.data2 & 0x7F; len = 3;
            ESP_LOGI(TAG, "TX %s -> CC: #%d, Val: %d, Ch: %d", midi_source_name(midi_target), cfg.data1, cfg.data2, cfg.channel + 1);
            break;
    }
    if (len > 0) midi_write_bytes(midi_target, msg, len);
}

void MIDI_CC_TX(uart_port_t uart_num, uint8_t channel, uint8_t cc_num, uint8_t cc_value) {
    uint8_t status = 0xB0 | (channel & 0x0F);
    uint8_t msg[3] = { status, cc_num & 0x7F, cc_value & 0x7F };
    midi_write_bytes((uint8_t)uart_num, msg, 3);
}

// MIDI Receiver Parser (PC + CC only)
static int midi_source_index(uint8_t midi_source)
{
    switch (midi_source) {
        case MIDI_SOURCE_UART2: return 1;
        case MIDI_SOURCE_USB: return 2;
        case MIDI_SOURCE_UART1:
        default:
            return 0;
    }
}

static void process_midi_byte(uint8_t byte, uint8_t midi_source)
{
    static uint8_t status[3] = {0, 0, 0};
    static uint8_t data1[3]  = {0, 0, 0};
    static int state[3]      = {0, 0, 0};

    int idx = midi_source_index(midi_source);

    // Status byte
    if (byte >= 0x80) {
        status[idx] = byte;
        state[idx] = 1;
        return;
    }

    uint8_t cmd = status[idx] & 0xF0;
    uint8_t ch  = status[idx] & 0x0F;

    // First data byte
    if (state[idx] == 1) {
        data1[idx] = byte;
        // PROGRAM CHANGE (1 data byte)
        if (cmd == 0xC0) {
            for (int i = 0; i < TOTAL_PRESETS; i++) {
                for (int m = 0; m < 10; m++) {
                    MidiMessageConfig rx_cfg = presety[i].midi_msgs[m];

                    if (rx_cfg.is_tx == 0 &&rx_cfg.type == MIDI_MSG_PC)
                    {
                        if (midi_source == rx_cfg.uart_num &&
                            rx_cfg.channel == ch &&
                            rx_cfg.data1 == data1[idx])
                        {
                            if (i == active_preset_idx) {
                                state[idx] = 0;
                                return;
                            }
                            ESP_LOGW(TAG, "[RX%d] Zdalna aktywacja presetu %d%c", idx + 1,(i / PRESETS_PER_BANK) + 1,'A' + (i % PRESETS_PER_BANK));
                            load_preset(i);
                            state[idx] = 0;
                            return;
                        }
                    }
                }
            }

            state[idx] = 0;
        }
        // CONTROL CHANGE -> wait for second byte
        else if (cmd == 0xB0) {
            state[idx] = 2;
        }
        else {
            state[idx] = 0;
        }
    }

    // Second data byte (CC only)
    else if (state[idx] == 2) {
        uint8_t data2 = byte;
        for (int i = 0; i < TOTAL_PRESETS; i++) {
            for (int m = 0; m < 10; m++) {
                MidiMessageConfig rx_cfg = presety[i].midi_msgs[m];
                if (rx_cfg.is_tx == 0 &&rx_cfg.type == MIDI_MSG_CC)
                {
                    if (midi_source == rx_cfg.uart_num &&
                        rx_cfg.channel == ch &&
                        rx_cfg.data1 == data1[idx] &&
                        rx_cfg.data2 == data2)
                    {
                        if (i == active_preset_idx) {
                            state[idx] = 0;
                            return;
                        }

                        ESP_LOGW(TAG, "[RX%d] Zdalna aktywacja presetu %d%c", idx + 1, (i / PRESETS_PER_BANK) + 1, 'A' + (i % PRESETS_PER_BANK));

                        load_preset(i);
                        state[idx] = 0;
                        return;
                    }
                }
            }
        }

        state[idx] = 0;
    }
}

#if CONFIG_TINYUSB_ENABLED && CFG_TUD_MIDI
static void usb_midi_dispatch_packet(const uint8_t packet[4])
{
    uint8_t cin = packet[0] & 0x0F;

    if (cin == 0x0B) {
        process_midi_byte(packet[1], MIDI_SOURCE_USB);
        process_midi_byte(packet[2], MIDI_SOURCE_USB);
        process_midi_byte(packet[3], MIDI_SOURCE_USB);
    } else if (cin == 0x0C) {
        process_midi_byte(packet[1], MIDI_SOURCE_USB);
        process_midi_byte(packet[2], MIDI_SOURCE_USB);
    }
}
#endif

static void MIDI_RX_Task(void *arg) {
    uint8_t byte;
    while (1) {
#if CONFIG_TINYUSB_ENABLED && CFG_TUD_MIDI
        uint8_t packet[4];
#endif
        while (uart_read_bytes(UART_NUM_1, &byte, 1, 0) > 0) process_midi_byte(byte, MIDI_SOURCE_UART1);
        while (uart_read_bytes(UART_NUM_2, &byte, 1, 0) > 0) process_midi_byte(byte, MIDI_SOURCE_UART2);
#if CONFIG_TINYUSB_ENABLED && CFG_TUD_MIDI
        while (tud_midi_available()) {
            if (tud_midi_packet_read(packet)) {
                usb_midi_dispatch_packet(packet);
            }
        }
#endif
        vTaskDelay(1); 
    }
}

// Relays & Amp Switches
extern void esp_rom_delay_us(uint32_t us);
void update_relays(uint16_t flags) {
    gpio_set_level(AMP_SWCH_R, (flags >> 0) & 0x1); 
    gpio_set_level(AMP_SWCH_T, (flags >> 1) & 0x1); 
    gpio_set_level(SHIFT_LATCH_PIN, 0); gpio_set_level(SHIFT_CLOCK_PIN, 0);
    for (int i = 4; i < 10; i++) {
        gpio_set_level(SHIFT_DATA_PIN, (flags >> i) & 0x1); 
        gpio_set_level(SHIFT_CLOCK_PIN, 1);
        esp_rom_delay_us(1); 
        gpio_set_level(SHIFT_CLOCK_PIN, 0); 
        esp_rom_delay_us(1);
    }
    for(int i = 2; i < 4; i++) {
        gpio_set_level(SHIFT_DATA_PIN, (flags >> i) & 0x1); 
        gpio_set_level(SHIFT_CLOCK_PIN, 1);
        esp_rom_delay_us(1); 
        gpio_set_level(SHIFT_CLOCK_PIN, 0); 
        esp_rom_delay_us(1);
    }
    for (int i = 10; i < 16; i++) {
        gpio_set_level(SHIFT_DATA_PIN, (flags >> i) & 0x1); 
        gpio_set_level(SHIFT_CLOCK_PIN, 1);
        esp_rom_delay_us(1); 
        gpio_set_level(SHIFT_CLOCK_PIN, 0); 
        esp_rom_delay_us(1);
    }
    gpio_set_level(SHIFT_LATCH_PIN, 1); esp_rom_delay_us(1); gpio_set_level(SHIFT_LATCH_PIN, 0);
    ESP_LOGW(TAG, "Relays updated: 0x%04X (bits: %d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d)",
        flags,
        (flags>>15)&1, (flags>>14)&1, (flags>>13)&1, (flags>>12)&1,
        (flags>>11)&1, (flags>>10)&1, (flags>>9)&1, (flags>>8)&1,
        (flags>>7)&1, (flags>>6)&1, (flags>>5)&1, (flags>>4)&1,
        (flags>>3)&1, (flags>>2)&1, (flags>>1)&1, (flags>>0)&1);
}

// Switcher Logic
void load_preset(int preset_idx) {
    if(preset_idx >= TOTAL_PRESETS || preset_idx < 0) return;
    active_preset_idx = preset_idx; active_bank = preset_idx / PRESETS_PER_BANK; selected_bank = active_bank; 
    Preset current = presety[preset_idx];
    
    update_relays(current.relay_flags);
    
    for (int i = 0; i < 10; i++) {
        if (current.midi_msgs[i].is_tx == 1 && current.midi_msgs[i].type != MIDI_MSG_NONE) {
            send_flexible_midi(current.midi_msgs[i].uart_num, current.midi_msgs[i]);
        }
    }
    
    update_live_display();
}

// On-Device Menu Interface
void update_menu_display(void) {
    char buf[32]; 
    
    if (menu_sub_step == 0) {
        const char* menu_names[] = {
            "L00PS ", "A-SUUTCH", 
            "NNIDI- 1", "NN1D1- 2", "NNIDI- 3", "NNIDI- 4", "NNIDI- 5", 
            "NNIDI- 6", "NNIDI- 7", "NNIDI- 8", "NNIDI- 9", "NNIDI-10",
            "EXP PDL"  
        };
        display_menu_text(menu_names[menu_current_item], -1);
        return;
    }
    
    if (menu_current_item == 0) { 
        uint8_t part = menu_bit_cursor / 4; 
        uint8_t start_bit = 4 + (part * 4); 
        snprintf(buf, sizeof(buf), "L%d  %d%d%d%d", part + 1,
                 (menu_temp_val >> (start_bit + 0)) & 1,
                 (menu_temp_val >> (start_bit + 1)) & 1,
                 (menu_temp_val >> (start_bit + 2)) & 1,
                 (menu_temp_val >> (start_bit + 3)) & 1);
        
        uint8_t cursor_on_screen = 4 + (menu_bit_cursor % 4); 
        display_menu_text(buf, cursor_on_screen); 
    }
    else if (menu_current_item == 1) {
        switch(menu_temp_val) {
            case 0:  display_menu_text("N0NE    ", -1); break; 
            case 1:  display_menu_text("RIN9    ", -1); break; 
            case 2:  display_menu_text("TIP     ", -1); break; 
            case 3:  display_menu_text("TIP_RIN9", -1); break; 
            default: break;
        }
    }
    else if (menu_current_item >= 2 && menu_current_item <= 11) { 
        uint8_t m_idx = menu_current_item - 2;
        
        if (menu_sub_step == 1) { 
            snprintf(buf, sizeof(buf), "M%d   %s", m_idx + 1, menu_temp_val ? "TX" : "RX"); 
            display_menu_text(buf, -1); 
        }
        else if (menu_sub_step == 2) {
            snprintf(buf, sizeof(buf), "M%d   U-%d", m_idx + 1, menu_temp_val); 
            display_menu_text(buf, -1); 
        }
        else if (menu_sub_step == 3) {
            if (menu_temp_val == MIDI_MSG_NONE)    snprintf(buf, sizeof(buf), "TP N0NE");
            if (menu_temp_val == MIDI_MSG_PC)      snprintf(buf, sizeof(buf), "TP  PC");
            if (menu_temp_val == MIDI_MSG_CC)      snprintf(buf, sizeof(buf), "TP  CC");
            display_menu_text(buf, -1);
        }
        else if (menu_sub_step == 4) { 
            snprintf(buf, sizeof(buf), "CH  %d", menu_temp_val + 1); 
            display_menu_text(buf, -1); 
        }
        else if (menu_sub_step == 5) { 
            snprintf(buf, sizeof(buf), "C1  %d", menu_temp_val); 
            display_menu_text(buf, -1); 
        }
        else if (menu_sub_step == 6) { 
            snprintf(buf, sizeof(buf), "C2  %d", menu_temp_val); 
            display_menu_text(buf, -1); 
        }
    }
    else if (menu_current_item == 12) { 
        if (menu_sub_step == 1) { 
            snprintf(buf, sizeof(buf), "E-CH  %02d", menu_temp_val + 1); 
            display_menu_text(buf, -1); 
        }
        else if (menu_sub_step == 2) { 
            snprintf(buf, sizeof(buf), "E-CC %03d", menu_temp_val); 
            display_menu_text(buf, -1); 
        }
    }
}

// Button Matrix State Machine Task
static void Handle_Buttons_Task(void* arg) {
    vTaskDelay(pdMS_TO_TICKS(500));
    const uint8_t pins[10] = { BTN_1_PIN, BTN_2_PIN, BTN_3_PIN, BTN_4_PIN, BTN_5_PIN, BTN_6_PIN, BTN_7_PIN, BTN_8_PIN, BTN_9_PIN, BTN_10_PIN };
    bool last_state[10]; TickType_t press_time[10] = {0}; bool long_press_triggered[10] = {false}; static bool stomp_states[4] = {false}; 
    TickType_t menu_hold_start = 0; bool menu_hold_active = false;
    
    static uint32_t last_tap_time = 0;

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

            if (!current_state[3] && last_state[3]) {
                if (menu_current_item == 0 && menu_sub_step == 1) {
                    menu_bit_cursor = (menu_bit_cursor + 1) % 12;
                    update_menu_display();
                }
            }

            if (!current_state[2] && last_state[2]) {
                if (menu_current_item == 0 && menu_sub_step == 1) {
                    menu_bit_cursor = (menu_bit_cursor > 0) ? menu_bit_cursor - 1 : 11;
                    update_menu_display();
                }
            }

            if (!current_state[5] && last_state[5]) {
                if (menu_sub_step == 0) {
                    menu_current_item = (menu_current_item + 1) % 13; 
                } else {
                    if (menu_current_item == 0) menu_temp_val |= (1 << (4 + menu_bit_cursor)); 
                    else if (menu_current_item == 1) menu_temp_val = (menu_temp_val + 1) % 4;
                    else if (menu_current_item >= 2 && menu_current_item <= 11) { 
                        if (menu_sub_step == 1)      menu_temp_val = (menu_temp_val + 1) % 2; 
                        else if (menu_sub_step == 2) menu_temp_val = (menu_temp_val >= 3) ? 1 : menu_temp_val + 1; 
                        else if (menu_sub_step == 3) menu_temp_val = (menu_temp_val + 1) % 3;
                        else if (menu_sub_step == 4) menu_temp_val = (menu_temp_val + 1) % 16;
                        else if ((menu_sub_step == 5 || menu_sub_step == 6) && menu_temp_val < 127) menu_temp_val++;
                    } 
                    else if (menu_current_item == 12) { 
                        if (menu_sub_step == 1)      menu_temp_val = (menu_temp_val + 1) % 16; 
                        else if (menu_sub_step == 2 && menu_temp_val < 127) menu_temp_val++;   
                    }
                }
                update_menu_display();
            }

            if (!current_state[4] && last_state[4]) {
                if (menu_sub_step == 0) {
                    menu_current_item = (menu_current_item > 0) ? menu_current_item - 1 : 12;
                } else {
                    if (menu_current_item == 0) menu_temp_val &= ~(1 << (4 + menu_bit_cursor)); 
                    else if (menu_current_item == 1) menu_temp_val = (menu_temp_val > 0) ? menu_temp_val - 1 : 3;
                    else if (menu_current_item >= 2 && menu_current_item <= 11) { 
                        if (menu_sub_step == 1)      menu_temp_val = (menu_temp_val > 0) ? menu_temp_val - 1 : 1;
                        else if (menu_sub_step == 2) menu_temp_val = (menu_temp_val <= 1) ? 3 : menu_temp_val - 1;
                        else if (menu_sub_step == 3) menu_temp_val = (menu_temp_val > 0) ? menu_temp_val - 1 : 4;
                        else if (menu_sub_step == 4) menu_temp_val = (menu_temp_val > 0) ? menu_temp_val - 1 : 15;
                        else if ((menu_sub_step == 5 || menu_sub_step == 6) && menu_temp_val > 0) menu_temp_val--;
                    } 
                    else if (menu_current_item == 12) { 
                        if (menu_sub_step == 1)      menu_temp_val = (menu_temp_val > 0) ? menu_temp_val - 1 : 15; 
                        else if (menu_sub_step == 2 && menu_temp_val > 0) menu_temp_val--;                                                                                   
                    }
                }
                update_menu_display();
            }

            if (!current_state[0] && last_state[0]) {
                if (menu_sub_step == 0) { 
                    menu_sub_step = 1;
                    if (menu_current_item == 0) { menu_temp_val = edit_p->relay_flags; menu_bit_cursor = 0; }
                    if (menu_current_item == 1) menu_temp_val = edit_p->relay_flags & 0x0003;
                    if (menu_current_item >= 2 && menu_current_item <= 11) {
                        menu_temp_val = edit_p->midi_msgs[menu_current_item - 2].is_tx;
                    }
                    if (menu_current_item == 12) menu_temp_val = edit_p->exp_channel;
                } else {
                    bool data_committed = false;

                    if (menu_current_item == 0) { 
                        edit_p->relay_flags = (edit_p->relay_flags & 0x000F) | (menu_temp_val & 0xFFF0); 
                        menu_sub_step = 0; data_committed = true;
                    }
                    else if (menu_current_item == 1) { 
                        edit_p->relay_flags = (edit_p->relay_flags & 0xFFFC) | (menu_temp_val & 0x0003); 
                        menu_sub_step = 0; data_committed = true;
                    }
                    else if (menu_current_item >= 2 && menu_current_item <= 11) { 
                        int m_idx = menu_current_item - 2;
                        MidiMessageConfig *msg = &edit_p->midi_msgs[m_idx];

                        if (menu_sub_step == 1) { 
                            if (menu_temp_val == 0) {
                                uint8_t target_uart = msg->uart_num;
                                for(int i = 0; i < 10; i++) {
                                    if(i != m_idx && edit_p->midi_msgs[i].is_tx == 0 && edit_p->midi_msgs[i].uart_num == target_uart) {
                                        edit_p->midi_msgs[i].is_tx = 1;
                                        ESP_LOGW(TAG, "M%d wymuszony na TX", i + 1);
                                    }
                                }
                            }
                            msg->is_tx = menu_temp_val; menu_sub_step = 2; menu_temp_val = msg->uart_num; 
                        }
                        else if (menu_sub_step == 2) { 
                            if (msg->is_tx == 0) {
                                for(int i = 0; i < 10; i++) {
                                    if(i != m_idx && edit_p->midi_msgs[i].is_tx == 0 && edit_p->midi_msgs[i].uart_num == menu_temp_val) {
                                        edit_p->midi_msgs[i].is_tx = 1;
                                    }
                                }
                            }
                            msg->uart_num = menu_temp_val; menu_sub_step = 3; menu_temp_val = msg->type; 
                        }
                        else if (menu_sub_step == 3) { msg->type = menu_temp_val; if(menu_temp_val == MIDI_MSG_NONE) { menu_sub_step = 0; data_committed = true; } else { menu_sub_step = 4; menu_temp_val = msg->channel; } }
                        else if (menu_sub_step == 4) { msg->channel = menu_temp_val; menu_sub_step = 5; menu_temp_val = msg->data1; }
                        else if (menu_sub_step == 5) { msg->data1 = menu_temp_val; if(msg->type == MIDI_MSG_PC) { menu_sub_step = 0; data_committed = true; } else { menu_sub_step = 6; menu_temp_val = msg->data2; } }
                        else if (menu_sub_step == 6) { msg->data2 = menu_temp_val; menu_sub_step = 0; data_committed = true; }
                    }
                    else if (menu_current_item == 12) { 
                        if (menu_sub_step == 1) { edit_p->exp_channel = menu_temp_val; menu_sub_step = 2; menu_temp_val = edit_p->exp_cc_num; }
                        else if (menu_sub_step == 2) { edit_p->exp_cc_num = menu_temp_val; menu_sub_step = 0; data_committed = true; }
                    }

                    if (data_committed) save_preset_to_flash(active_preset_idx);
                }
                update_menu_display();
            }

            if (!current_state[1] && last_state[1]) {
                if (menu_sub_step > 0) menu_sub_step--; 
                else in_menu_mode = false; 
                update_menu_display();
            }

        } else {
            // Live Performance Mode
            if (!current_state[4] && last_state[4]) {
                selected_bank = (selected_bank > 0) ? selected_bank - 1 : NUM_BANKS - 1;
                update_live_display();
            } 
            if (!current_state[5] && last_state[5]) {
                selected_bank = (selected_bank + 1) % NUM_BANKS;
                update_live_display();
            }
            
            for (int i = 0; i < 4; i++) {
                if (!current_state[i] && last_state[i]) { 
                    int target_preset_idx = (selected_bank * PRESETS_PER_BANK) + i;
                    if (target_preset_idx == active_preset_idx) { 
                        active_preset_idx = -1; update_relays(0); update_live_display(); 
                    } else { load_preset(target_preset_idx); }
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
                        
                        case BTN_MODE_TAP_TEMPO: 
                            if (!current_state[b] && last_state[b]) { 
                                uint32_t now_ms = pdTICKS_TO_MS(now);
                                uint32_t interval = now_ms - last_tap_time;
                                last_tap_time = now_ms;
                                if (interval >= 250 && interval <= 1500) {
                                    uint8_t bpm_val = 60000 / interval;
                                    MIDI_CC_TX(UART_NUM_1, current_p.exp_channel, 93, bpm_val);
                                    MIDI_CC_TX(UART_NUM_2, current_p.exp_channel, 93, bpm_val);
                                }
                                MIDI_CC_TX(UART_NUM_1, current_p.exp_channel, 64, 127);
                                MIDI_CC_TX(UART_NUM_2, current_p.exp_channel, 64, 127);
                            } 
                            break;
                            
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

void save_preset_to_flash(int idx)
{
    if (idx < 0 || idx >= TOTAL_PRESETS) return;

    nvs_handle_t nvs_handle;

    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed");
        return;
    }

    char key[8];
    snprintf(key, sizeof(key), "p%03d", idx);

    err = nvs_set_blob(
        nvs_handle,
        key,
        &presety[idx],
        sizeof(Preset)
    );

    if (err == ESP_OK)
        err = nvs_commit(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Save preset failed: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
}

void load_presets_from_flash(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);

    nvs_handle_t nvs_handle;

    err = nvs_open("storage", NVS_READWRITE, &nvs_handle);

    if (err != ESP_OK) return;

    for (int i = 0; i < TOTAL_PRESETS; i++) {

        char key[8];
        snprintf(key, sizeof(key), "p%03d", i);

        size_t required_size = sizeof(Preset);

        err = nvs_get_blob(
            nvs_handle,
            key,
            &presety[i],
            &required_size
        );

        if (err == ESP_ERR_NVS_NOT_FOUND) {

            memset(&presety[i], 0, sizeof(Preset));

            presety[i].exp_channel = 0;
            presety[i].exp_cc_num = 11;

            for(int m = 0; m < 10; m++) {
                presety[i].midi_msgs[m].is_tx = 1;
                presety[i].midi_msgs[m].uart_num = 1;
            }
        }
    }

    nvs_close(nvs_handle);
}

void app_main(void) {
    hw_init();
    gpio_set_level(OE_PIN, 0); 
    gpio_set_level(SRCLR_PIN, 1);
    memset(presety, 0, sizeof(presety));

#if CONFIG_TINYUSB_ENABLED && CFG_TUD_MIDI
    ESP_LOGI(TAG, "USB MIDI initialization");
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.string = usb_midi_string_desc;
    tusb_cfg.descriptor.string_count = sizeof(usb_midi_string_desc) / sizeof(usb_midi_string_desc[0]);
    tusb_cfg.descriptor.full_speed_config = usb_midi_fs_config_desc;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = usb_midi_hs_config_desc;
    tusb_cfg.descriptor.qualifier = NULL;
#endif
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB MIDI initialization DONE");
#else
    ESP_LOGW(TAG, "USB MIDI support is disabled in this build");
#endif

    load_presets_from_flash();
    load_preset(0);
    ESP_LOGI(TAG, "test");

    xTaskCreate(Handle_Buttons_Task, "buttons_task", 4096, NULL, 5, NULL);
    xTaskCreate(MIDI_RX_Task, "midi_rx_task", 4096, NULL, 4, NULL); 
    xTaskCreate(Expression_Pedal_Task, "exp_pedal_task", 4096, NULL, 4, NULL); 
}