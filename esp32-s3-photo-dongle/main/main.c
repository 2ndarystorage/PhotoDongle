/*
 * BLE Photo Dongle for ESP32-S3-0.96-SD (Spotpear / GNPE board)
 *
 * Receives 160x80 RGB565 frames from an iPhone over BLE (NimBLE),
 * stores them on the microSD card, and shows them as a slideshow.
 *
 * Hardware (vendor definition for this exact board):
 *   chip: ESP32-S3N4R2 (4MB flash, 2MB PSRAM)
 *   LCD : ST7735-family 160x80, SPI2
 *         MOSI=4  SCLK=5  CS=8  DC=6  RST=7  BL=3 (LEDC PWM, positive logic)
 *         MADCTL=0xA0 (RGB order; the vendor's 0xA8 selects BGR and swaps
 *         red/blue for RGB565 data), offsets X+1 / Y+26, vendor init sequence
 *   SD  : SPI mode on SPI3  MOSI=12  MISO=14  SCK=13  CS=11 (20MHz)
 *   BTN : GPIO9 (BOOT) -> short press skips to the next photo
 *
 * BLE protocol (custom GATT service 0xFFF0, device name "PHOTO-DONGLE"):
 *   CTRL 0xFFF1 (write): command packets
 *       [0x01][u32 LE total_size]  BEGIN  (open temp file, expect total_size bytes)
 *       [0x02]                     COMMIT (close, rename to PHOTO_xxx.565)
 *       [0x03][u16 LE interval_s][u8 order][u8 scale]  SETTINGS (persisted in NVS)
 *       [0x04]                     CLEAR  (delete all photos)
 *       [0x05]                     NEXT   (skip to next photo; exits VJ mode)
 *       [0x06]                     VJ_BEGIN (wipe VJ frames; following
 *                                   BEGIN/DATA/COMMIT transfers go to /vj)
 *       [0x07]                     VJ_PLAY  (finish receiving, play VJ loop)
 *       [0x08]                     VJ_STOP  (leave VJ mode, back to slideshow)
 *       [0x09]                     VJ_RESUME (play stored VJ frames again)
 *   DATA 0xFFF2 (write): raw frame bytes, appended while receiving
 *   STAT 0xFFF3 (read/notify): [u8 state][u16 LE count][u16 LE current]
 *                               [u16 LE interval_s][u8 order][u8 scale]
 *   Frame format: exactly 25600 bytes, RGB565 big-endian, row-major 160x80.
 *   state: 0=idle 1=receiving 2=error 4=VJ playing
 *   order: 0=sequential 1=random ; scale: 0=fit 1=cover 2=shrink
 *   VJ mode: up to 64 effect frames stored in /vj/F000.565.., played back
 *   in a loop at ~8fps with a breathing backlight. BOOT button exits VJ.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_flash.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "nvs_flash.h"
#include "nvs.h"

/* NimBLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "os/os_mbuf.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "PHOTOFRAME";

/* ---------------- pins (vendor) ---------------- */
#define PIN_LCD_MOSI  4
#define PIN_LCD_SCLK  5
#define PIN_LCD_CS    8
#define PIN_LCD_DC    6
#define PIN_LCD_RST   7
#define PIN_LCD_BL    3

#define PIN_SD_MOSI   12
#define PIN_SD_MISO   14
#define PIN_SD_SCK    13
#define PIN_SD_CS     11

#define PIN_BTN       9

#define LCD_W 160
#define LCD_H 80
#define LCD_X_OFF 1
#define LCD_Y_OFF 26

#define FRAME_BYTES (LCD_W * LCD_H * 2)
#define MAX_PHOTOS  500
#define SD_MOUNT_POINT "/sdcard"
#define TMP_PATH SD_MOUNT_POINT "/.recv.tmp"
#define VJ_DIR SD_MOUNT_POINT "/vj"
#define MAX_VJ_FRAMES 64
#define VJ_FRAME_MS 120

#define FW_VERSION "1.1"   /* shown at the top-left of the screen */

/* ---------------- shared state ---------------- */
typedef struct {
    uint16_t interval_s;   /* seconds per photo, 1..3600 */
    uint8_t  order;        /* 0 sequential, 1 random */
    uint8_t  scale;        /* 0 fit, 1 cover, 2 shrink (applied on the phone) */
} settings_t;

static settings_t s_settings = { .interval_s = 5, .order = 0, .scale = 1 };

static bool     s_sd_ok = false;
static uint16_t s_photo_idx[MAX_PHOTOS];       /* sorted photo numbers */
static int      s_photo_count = 0;
static int      s_current = 0;                 /* index into s_photo_idx */

/* VJ mode state */
static int      s_vj_count = 0;                /* frames stored in VJ_DIR */
static bool     s_vj_rx = false;               /* receiving a VJ frame set */
static int      s_vj_rx_idx = 0;               /* next VJ frame number */
static bool     s_vj_active = false;           /* playing the VJ loop */
static int      s_vj_frame = 0;                /* current VJ frame */

static SemaphoreHandle_t s_sd_mutex;
static EventGroupHandle_t s_events;
#define EV_NEXT     (1 << 0)
#define EV_CHANGED  (1 << 1)
#define EV_VJ       (1 << 2)

/* ---------------- LCD ---------------- */
static spi_device_handle_t s_lcd;
static uint16_t s_fb[LCD_W * LCD_H];

#define RGB565(r, g, b) ((uint16_t)(((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define COL_BLACK   RGB565(0, 0, 0)
#define COL_WHITE   RGB565(255, 255, 255)
#define COL_RED     RGB565(255, 0, 0)
#define COL_GREEN   RGB565(0, 255, 0)
#define COL_BLUE    RGB565(0, 0, 255)
#define COL_YELLOW  RGB565(255, 255, 0)
#define COL_CYAN    RGB565(0, 255, 255)
#define COL_MAGENTA RGB565(255, 0, 255)
#define COL_GRAY    RGB565(64, 64, 64)

static void lcd_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_LCD_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_lcd, &t));
}

static void lcd_data(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    gpio_set_level(PIN_LCD_DC, 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_lcd, &t));
}

static void lcd_cmd1(uint8_t cmd, uint8_t d0)
{
    lcd_cmd(cmd);
    lcd_data(&d0, 1);
}

static void backlight_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    /* vendor: duty = percent * 10 with 10-bit resolution (max 1023) */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)percent * 10);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void lcd_init(void)
{
    /* backlight PWM: LEDC 1kHz / 10bit on GPIO3 (vendor Backlight_Init) */
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t ch = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_LCD_DC) | (1ULL << PIN_LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * 2 + 16,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_LCD_CS,
        .queue_size = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_lcd));

    /* hardware reset (vendor LCD_Reset timing) */
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* --- vendor init sequence (Display_ST7789.cpp LCD_Init), verbatim --- */
    lcd_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));          /* SLPOUT */
    lcd_cmd1(0x36, 0xA0);                                   /* MADCTL: MY+MV+RGB (0xA8=BGR swaps R/B) */
    { const uint8_t d[] = {0x01, 0x2C, 0x2D}; lcd_cmd(0xB1); lcd_data(d, 3); }
    { const uint8_t d[] = {0x01, 0x2C, 0x2D}; lcd_cmd(0xB2); lcd_data(d, 3); }
    { const uint8_t d[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D}; lcd_cmd(0xB3); lcd_data(d, 6); }
    lcd_cmd1(0xB4, 0x07);                                   /* inversion control */
    lcd_cmd(0x21);                                          /* INVON */
    { const uint8_t d[] = {0xA2, 0x02, 0x84}; lcd_cmd(0xC0); lcd_data(d, 3); }
    lcd_cmd1(0xC1, 0xC5);
    { const uint8_t d[] = {0x0A, 0x00}; lcd_cmd(0xC2); lcd_data(d, 2); }
    { const uint8_t d[] = {0x8A, 0x2A}; lcd_cmd(0xC3); lcd_data(d, 2); }
    { const uint8_t d[] = {0x8A, 0xEE}; lcd_cmd(0xC4); lcd_data(d, 2); }
    lcd_cmd1(0xC5, 0x0E);                                   /* VCOM */
    { const uint8_t d[] = {0x0F, 0x1A, 0x0F, 0x18, 0x2F, 0x28, 0x20, 0x22,
                           0x1F, 0x1B, 0x23, 0x37, 0x00, 0x07, 0x02, 0x10};
      lcd_cmd(0xE0); lcd_data(d, 16); }                     /* gamma + */
    { const uint8_t d[] = {0x0F, 0x1B, 0x0F, 0x17, 0x33, 0x2C, 0x29, 0x2E,
                           0x30, 0x30, 0x39, 0x3F, 0x00, 0x07, 0x03, 0x10};
      lcd_cmd(0xE1); lcd_data(d, 16); }                     /* gamma - */
    { const uint8_t d[] = {0x00, 0x02, 0x00, 0x82}; lcd_cmd(0x2A); lcd_data(d, 4); }
    { const uint8_t d[] = {0x00, 0x03, 0x00, 0x83}; lcd_cmd(0x2B); lcd_data(d, 4); }
    lcd_cmd1(0xF0, 0x01);                                   /* enable test command */
    lcd_cmd1(0xF6, 0x00);                                   /* disable ram power save */
    lcd_cmd1(0x3A, 0x05);                                   /* COLMOD: 16bit */
    lcd_cmd(0x29);                                          /* DISPON */
    vTaskDelay(pdMS_TO_TICKS(20));

    backlight_set(100);
    ESP_LOGI(TAG, "LCD init done (ST7735 160x80, vendor seq, BL=100%%)");
}

static void lcd_flush(void)
{
    /* vendor LCD_SetCursor: col = x + 1, row = y + 26 */
    uint8_t ca[] = { 0, LCD_X_OFF, 0, (LCD_W - 1) + LCD_X_OFF };
    uint8_t ra[] = { 0, LCD_Y_OFF, 0, (LCD_H - 1) + LCD_Y_OFF };
    lcd_cmd(0x2A); lcd_data(ca, 4);
    lcd_cmd(0x2B); lcd_data(ra, 4);
    lcd_cmd(0x2C);
    /* framebuffer is landscape row-major, big-endian RGB565 on the wire */
    static uint16_t wire[LCD_W * LCD_H];
    for (int i = 0; i < LCD_W * LCD_H; i++) {
        wire[i] = (uint16_t)((s_fb[i] >> 8) | (s_fb[i] << 8));
    }
    gpio_set_level(PIN_LCD_DC, 1);
    spi_transaction_t t = { .length = LCD_W * LCD_H * 16, .tx_buffer = wire };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_lcd, &t));
}

static void fb_fill(uint16_t color)
{
    for (int i = 0; i < LCD_W * LCD_H; i++) s_fb[i] = color;
}

static void fb_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int yy = y; yy < y + h && yy < LCD_H; yy++) {
        if (yy < 0) continue;
        for (int xx = x; xx < x + w && xx < LCD_W; xx++) {
            if (xx < 0) continue;
            s_fb[yy * LCD_W + xx] = color;
        }
    }
}

/* --- tiny 3x5 font (uppercase, digits, basic symbols) --- */
static void glyph(char c, uint8_t out[5])
{
    memset(out, 0, 5);
    switch (c) {
    case '0': { const uint8_t g[5] = {0x7, 0x5, 0x5, 0x5, 0x7}; memcpy(out, g, 5); break; }
    case '1': { const uint8_t g[5] = {0x2, 0x6, 0x2, 0x2, 0x7}; memcpy(out, g, 5); break; }
    case '2': { const uint8_t g[5] = {0x7, 0x1, 0x7, 0x4, 0x7}; memcpy(out, g, 5); break; }
    case '3': { const uint8_t g[5] = {0x7, 0x1, 0x3, 0x1, 0x7}; memcpy(out, g, 5); break; }
    case '4': { const uint8_t g[5] = {0x5, 0x5, 0x7, 0x1, 0x1}; memcpy(out, g, 5); break; }
    case '5': { const uint8_t g[5] = {0x7, 0x4, 0x7, 0x1, 0x7}; memcpy(out, g, 5); break; }
    case '6': { const uint8_t g[5] = {0x7, 0x4, 0x7, 0x5, 0x7}; memcpy(out, g, 5); break; }
    case '7': { const uint8_t g[5] = {0x7, 0x1, 0x1, 0x2, 0x2}; memcpy(out, g, 5); break; }
    case '8': { const uint8_t g[5] = {0x7, 0x5, 0x7, 0x5, 0x7}; memcpy(out, g, 5); break; }
    case '9': { const uint8_t g[5] = {0x7, 0x5, 0x7, 0x1, 0x7}; memcpy(out, g, 5); break; }
    case 'A': { const uint8_t g[5] = {0x2, 0x5, 0x7, 0x5, 0x5}; memcpy(out, g, 5); break; }
    case 'B': { const uint8_t g[5] = {0x6, 0x5, 0x6, 0x5, 0x6}; memcpy(out, g, 5); break; }
    case 'C': { const uint8_t g[5] = {0x3, 0x4, 0x4, 0x4, 0x3}; memcpy(out, g, 5); break; }
    case 'D': { const uint8_t g[5] = {0x6, 0x5, 0x5, 0x5, 0x6}; memcpy(out, g, 5); break; }
    case 'E': { const uint8_t g[5] = {0x7, 0x4, 0x6, 0x4, 0x7}; memcpy(out, g, 5); break; }
    case 'F': { const uint8_t g[5] = {0x7, 0x4, 0x6, 0x4, 0x4}; memcpy(out, g, 5); break; }
    case 'G': { const uint8_t g[5] = {0x3, 0x4, 0x5, 0x5, 0x3}; memcpy(out, g, 5); break; }
    case 'H': { const uint8_t g[5] = {0x5, 0x5, 0x7, 0x5, 0x5}; memcpy(out, g, 5); break; }
    case 'I': { const uint8_t g[5] = {0x7, 0x2, 0x2, 0x2, 0x7}; memcpy(out, g, 5); break; }
    case 'J': { const uint8_t g[5] = {0x1, 0x1, 0x1, 0x5, 0x2}; memcpy(out, g, 5); break; }
    case 'K': { const uint8_t g[5] = {0x5, 0x6, 0x4, 0x6, 0x5}; memcpy(out, g, 5); break; }
    case 'L': { const uint8_t g[5] = {0x4, 0x4, 0x4, 0x4, 0x7}; memcpy(out, g, 5); break; }
    case 'M': { const uint8_t g[5] = {0x5, 0x7, 0x7, 0x5, 0x5}; memcpy(out, g, 5); break; }
    case 'N': { const uint8_t g[5] = {0x5, 0x7, 0x7, 0x7, 0x5}; memcpy(out, g, 5); break; }
    case 'O': { const uint8_t g[5] = {0x2, 0x5, 0x5, 0x5, 0x2}; memcpy(out, g, 5); break; }
    case 'P': { const uint8_t g[5] = {0x6, 0x5, 0x6, 0x4, 0x4}; memcpy(out, g, 5); break; }
    case 'Q': { const uint8_t g[5] = {0x2, 0x5, 0x5, 0x6, 0x3}; memcpy(out, g, 5); break; }
    case 'R': { const uint8_t g[5] = {0x6, 0x5, 0x6, 0x5, 0x5}; memcpy(out, g, 5); break; }
    case 'S': { const uint8_t g[5] = {0x3, 0x4, 0x2, 0x1, 0x6}; memcpy(out, g, 5); break; }
    case 'T': { const uint8_t g[5] = {0x7, 0x2, 0x2, 0x2, 0x2}; memcpy(out, g, 5); break; }
    case 'U': { const uint8_t g[5] = {0x5, 0x5, 0x5, 0x5, 0x7}; memcpy(out, g, 5); break; }
    case 'V': { const uint8_t g[5] = {0x5, 0x5, 0x5, 0x5, 0x2}; memcpy(out, g, 5); break; }
    case 'W': { const uint8_t g[5] = {0x5, 0x5, 0x7, 0x7, 0x5}; memcpy(out, g, 5); break; }
    case 'X': { const uint8_t g[5] = {0x5, 0x5, 0x2, 0x5, 0x5}; memcpy(out, g, 5); break; }
    case 'Y': { const uint8_t g[5] = {0x5, 0x5, 0x2, 0x2, 0x2}; memcpy(out, g, 5); break; }
    case 'Z': { const uint8_t g[5] = {0x7, 0x1, 0x2, 0x4, 0x7}; memcpy(out, g, 5); break; }
    case ':': { const uint8_t g[5] = {0x0, 0x2, 0x0, 0x2, 0x0}; memcpy(out, g, 5); break; }
    case '-': { const uint8_t g[5] = {0x0, 0x0, 0x7, 0x0, 0x0}; memcpy(out, g, 5); break; }
    case '+': { const uint8_t g[5] = {0x0, 0x2, 0x7, 0x2, 0x0}; memcpy(out, g, 5); break; }
    case '/': { const uint8_t g[5] = {0x1, 0x1, 0x2, 0x4, 0x4}; memcpy(out, g, 5); break; }
    case '.': { const uint8_t g[5] = {0x0, 0x0, 0x0, 0x0, 0x2}; memcpy(out, g, 5); break; }
    case '%': { const uint8_t g[5] = {0x5, 0x1, 0x2, 0x4, 0x5}; memcpy(out, g, 5); break; }
    case '!': { const uint8_t g[5] = {0x2, 0x2, 0x2, 0x0, 0x2}; memcpy(out, g, 5); break; }
    case ' ': break;
    default: break;
    }
}

static void fb_text(int x, int y, const char *s, int scale, uint16_t fg, uint16_t bg)
{
    int cx = x;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        uint8_t g[5];
        glyph(c, g);
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 3; col++) {
                uint16_t px = (g[row] >> (2 - col)) & 1 ? fg : bg;
                fb_rect(cx + col * scale, y + row * scale, scale, scale, px);
            }
        }
        cx += 4 * scale;
        if (cx > LCD_W - 4 * scale) break;
    }
}

/* Firmware version, always drawn at the top-left corner of the screen. */
static void draw_version_overlay(void)
{
    fb_text(1, 1, "V" FW_VERSION, 1, COL_WHITE, COL_BLACK);
}


/* ---------------- settings (NVS) ---------------- */
static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open("photoframe", NVS_READONLY, &h) != ESP_OK) return;
    uint16_t v16 = 0;
    uint8_t v8 = 0;
    if (nvs_get_u16(h, "interval", &v16) == ESP_OK && v16 >= 1 && v16 <= 3600) s_settings.interval_s = v16;
    if (nvs_get_u8(h, "order", &v8) == ESP_OK && v8 <= 1) s_settings.order = v8;
    if (nvs_get_u8(h, "scale", &v8) == ESP_OK && v8 <= 2) s_settings.scale = v8;
    nvs_close(h);
    ESP_LOGI(TAG, "settings: interval=%us order=%s scale=%d",
             s_settings.interval_s, s_settings.order ? "random" : "seq", s_settings.scale);
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("photoframe", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "interval", s_settings.interval_s);
    nvs_set_u8(h, "order", s_settings.order);
    nvs_set_u8(h, "scale", s_settings.scale);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------- SD card + photo store ---------------- */
static int cmp_u16(const void *a, const void *b)
{
    uint16_t x = *(const uint16_t *)a, y = *(const uint16_t *)b;
    return (x > y) - (x < y);
}

static void photos_rescan(void)
{
    s_photo_count = 0;
    if (!s_sd_ok) return;
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && s_photo_count < MAX_PHOTOS) {
        int num;
        char ext[8];
        if (strlen(e->d_name) > 20) continue;  /* only PHOTO_xxx.565 names qualify */
        if (sscanf(e->d_name, "PHOTO_%3d.%7s", &num, ext) == 2 &&
            strcasecmp(ext, "565") == 0 && num >= 0 && num < 1000) {
            char path[64];
            snprintf(path, sizeof(path), "%s/PHOTO_%03d.565", SD_MOUNT_POINT, num);
            struct stat st;
            if (stat(path, &st) == 0 && st.st_size == FRAME_BYTES) {
                s_photo_idx[s_photo_count++] = (uint16_t)num;
            }
        }
    }
    closedir(dir);
    qsort(s_photo_idx, s_photo_count, sizeof(uint16_t), cmp_u16);
    if (s_current >= s_photo_count) s_current = 0;
    ESP_LOGI(TAG, "photos on SD: %d", s_photo_count);
}

static bool sd_mount(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SD SPI bus init failed: %s", esp_err_to_name(ret));
        return false;
    }
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = 20000;
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = PIN_SD_CS;
    dev.host_id = SPI3_HOST;
    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &dev, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s (insert a FAT32 card before power-on)", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "SD mounted: %s, %.1f MB", card->cid.name,
             (double)card->csd.capacity * card->csd.sector_size / (1024.0 * 1024.0));
    return true;
}

/* ---------------- VJ frame store ---------------- */
static void vj_rescan(void)
{
    s_vj_count = 0;
    if (!s_sd_ok) return;
    for (int i = 0; i < MAX_VJ_FRAMES; i++) {
        char path[64];
        snprintf(path, sizeof(path), "%s/F%03d.565", VJ_DIR, i);
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size == FRAME_BYTES) {
            s_vj_count++;
        } else {
            break;  /* frames are stored sequentially from 0 */
        }
    }
    ESP_LOGI(TAG, "vj frames on SD: %d", s_vj_count);
}

static void vj_clear_files(void)
{
    for (int i = 0; i < MAX_VJ_FRAMES; i++) {
        char path[64];
        snprintf(path, sizeof(path), "%s/F%03d.565", VJ_DIR, i);
        unlink(path);
    }
    s_vj_count = 0;
    s_vj_rx_idx = 0;
}

/* ---------------- BLE (NimBLE) ---------------- */
#define SVC_UUID  0xFFF0
#define CTRL_UUID 0xFFF1
#define DATA_UUID 0xFFF2
#define STAT_UUID 0xFFF3

static uint16_t s_stat_val_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t  s_own_addr_type;

static uint8_t  s_ble_state = 0;      /* 0 idle, 1 receiving, 2 error */
static FILE    *s_recv_file = NULL;
static uint32_t s_recv_expected = 0;
static uint32_t s_recv_count = 0;

static uint8_t report_state(void)
{
    return s_vj_active ? 4 : s_ble_state;
}

static void status_notify(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    uint8_t p[9];
    p[0] = report_state();
    p[1] = s_photo_count & 0xFF; p[2] = s_photo_count >> 8;
    int cur = s_photo_count > 0 ? s_photo_idx[s_current] : 0;
    p[3] = cur & 0xFF; p[4] = cur >> 8;
    p[5] = s_settings.interval_s & 0xFF; p[6] = s_settings.interval_s >> 8;
    p[7] = s_settings.order;
    p[8] = s_settings.scale;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(p, sizeof(p));
    if (om) ble_gatts_notify_custom(s_conn_handle, s_stat_val_handle, om);
}

static int stat_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t p[9];
        p[0] = report_state();
        p[1] = s_photo_count & 0xFF; p[2] = s_photo_count >> 8;
        int cur = s_photo_count > 0 ? s_photo_idx[s_current] : 0;
        p[3] = cur & 0xFF; p[4] = cur >> 8;
        p[5] = s_settings.interval_s & 0xFF; p[6] = s_settings.interval_s >> 8;
        p[7] = s_settings.order;
        p[8] = s_settings.scale;
        int rc = os_mbuf_append(ctxt->om, p, sizeof(p));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static void recv_abort(void)
{
    if (s_recv_file) {
        fclose(s_recv_file);
        s_recv_file = NULL;
    }
    unlink(TMP_PATH);
    s_recv_count = 0;
    s_recv_expected = 0;
}

static int ctrl_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t buf[16];
    uint16_t len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
    if (rc != 0 || len < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t op = buf[0];
    ESP_LOGI(TAG, "CTRL op=0x%02x len=%d", op, len);

    switch (op) {
    case 0x01: { /* BEGIN */
        if (len < 5) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint32_t total = buf[1] | (buf[2] << 8) | ((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 24);
        xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
        recv_abort();
        if (!s_sd_ok || total != FRAME_BYTES) {
            s_ble_state = 2;
            ESP_LOGW(TAG, "BEGIN rejected: sd_ok=%d total=%lu", s_sd_ok, (unsigned long)total);
        } else {
            s_recv_file = fopen(TMP_PATH, "wb");
            if (s_recv_file) {
                s_recv_expected = total;
                s_recv_count = 0;
                s_ble_state = 1;
                ESP_LOGI(TAG, "BEGIN: expecting %lu bytes", (unsigned long)total);
            } else {
                s_ble_state = 2;
            }
        }
        xSemaphoreGive(s_sd_mutex);
        break;
    }
    case 0x02: { /* COMMIT */
        xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
        if (s_recv_file && s_recv_count == s_recv_expected && s_recv_expected == FRAME_BYTES) {
            fclose(s_recv_file);
            s_recv_file = NULL;
            if (s_vj_rx) {
                char dst[64];
                snprintf(dst, sizeof(dst), "%s/F%03d.565", VJ_DIR, s_vj_rx_idx);
                if (rename(TMP_PATH, dst) == 0) {
                    s_vj_rx_idx++;
                    s_vj_count = s_vj_rx_idx;
                    s_ble_state = 0;
                    ESP_LOGI(TAG, "COMMIT: saved VJ frame %s (%d)", dst, s_vj_count);
                } else {
                    ESP_LOGW(TAG, "COMMIT: VJ rename failed");
                    unlink(TMP_PATH);
                    s_ble_state = 2;
                }
                s_recv_count = 0;
                s_recv_expected = 0;
                xSemaphoreGive(s_sd_mutex);
                break;
            }
            int used[1000 / 8] = {0};
            for (int i = 0; i < s_photo_count; i++) used[s_photo_idx[i] >> 3] |= 1 << (s_photo_idx[i] & 7);
            int num = -1;
            for (int i = 0; i < 1000; i++) {
                if (!(used[i >> 3] & (1 << (i & 7)))) { num = i; break; }
            }
            if (num >= 0) {
                char dst[64];
                snprintf(dst, sizeof(dst), "%s/PHOTO_%03d.565", SD_MOUNT_POINT, num);
                if (rename(TMP_PATH, dst) == 0) {
                    ESP_LOGI(TAG, "COMMIT: saved %s", dst);
                    s_ble_state = 0;
                } else {
                    ESP_LOGW(TAG, "COMMIT: rename failed");
                    unlink(TMP_PATH);
                    s_ble_state = 2;
                }
            } else {
                unlink(TMP_PATH);
                s_ble_state = 2;
            }
            photos_rescan();
            xEventGroupSetBits(s_events, EV_CHANGED);
        } else {
            ESP_LOGW(TAG, "COMMIT rejected: count=%lu expected=%lu",
                     (unsigned long)s_recv_count, (unsigned long)s_recv_expected);
            recv_abort();
            s_ble_state = 2;
        }
        s_recv_count = 0;
        s_recv_expected = 0;
        xSemaphoreGive(s_sd_mutex);
        break;
    }
    case 0x03: { /* SETTINGS */
        if (len < 6) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint16_t iv = buf[1] | (buf[2] << 8);
        if (iv < 1) iv = 1;
        if (iv > 3600) iv = 3600;
        s_settings.interval_s = iv;
        s_settings.order = buf[3] <= 1 ? buf[3] : 0;
        s_settings.scale = buf[4] <= 2 ? buf[4] : 0;
        settings_save();
        ESP_LOGI(TAG, "SETTINGS: interval=%u order=%u scale=%u", iv, buf[3], buf[4]);
        xEventGroupSetBits(s_events, EV_CHANGED);
        break;
    }
    case 0x04: { /* CLEAR */
        xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
        recv_abort();
        if (s_sd_ok) {
            for (int i = 0; i < s_photo_count; i++) {
                char path[64];
                snprintf(path, sizeof(path), "%s/PHOTO_%03d.565", SD_MOUNT_POINT, s_photo_idx[i]);
                unlink(path);
            }
        }
        photos_rescan();
        s_current = 0;
        s_ble_state = 0;
        xSemaphoreGive(s_sd_mutex);
        xEventGroupSetBits(s_events, EV_CHANGED);
        ESP_LOGI(TAG, "CLEAR done");
        break;
    }
    case 0x05: /* NEXT */
        xEventGroupSetBits(s_events, EV_NEXT);
        break;
    case 0x06: /* VJ_BEGIN */
        xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
        recv_abort();
        s_vj_active = false;
        if (s_sd_ok) vj_clear_files();
        s_vj_rx = true;
        s_ble_state = 0;
        xSemaphoreGive(s_sd_mutex);
        ESP_LOGI(TAG, "VJ_BEGIN: waiting for frames");
        break;
    case 0x07: /* VJ_PLAY */
        s_vj_rx = false;
        xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
        vj_rescan();
        xSemaphoreGive(s_sd_mutex);
        if (s_vj_count > 0) {
            s_vj_active = true;
            s_vj_frame = 0;
            xEventGroupSetBits(s_events, EV_VJ);
        }
        ESP_LOGI(TAG, "VJ_PLAY: %d frames, active=%d", s_vj_count, s_vj_active);
        break;
    case 0x08: /* VJ_STOP */
        s_vj_rx = false;
        s_vj_active = false;
        xEventGroupSetBits(s_events, EV_CHANGED);
        ESP_LOGI(TAG, "VJ_STOP");
        break;
    case 0x09: /* VJ_RESUME */
        if (s_vj_count > 0) {
            s_vj_active = true;
            s_vj_frame = 0;
            xEventGroupSetBits(s_events, EV_VJ);
        }
        ESP_LOGI(TAG, "VJ_RESUME: %d frames", s_vj_count);
        break;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
    status_notify();
    return 0;
}

static int data_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    static uint8_t buf[1024];
    uint16_t len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
    if (rc != 0) return BLE_ATT_ERR_INSUFFICIENT_RES;
    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    if (s_recv_file && s_ble_state == 1) {
        size_t w = fwrite(buf, 1, len, s_recv_file);
        s_recv_count += w;
    }
    xSemaphoreGive(s_sd_mutex);
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(CTRL_UUID),
                .access_cb = ctrl_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = BLE_UUID16_DECLARE(DATA_UUID),
                .access_cb = data_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = BLE_UUID16_DECLARE(STAT_UUID),
                .access_cb = stat_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_stat_val_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE connected");
            status_notify();
        } else {
            extern void adv_start(void);
            adv_start();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected");
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (s_ble_state == 1) {  /* transfer interrupted */
            xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
            recv_abort();
            s_ble_state = 0;
            s_vj_rx = false;
            xSemaphoreGive(s_sd_mutex);
        }
        extern void adv_start(void);
        adv_start();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        extern void adv_start(void);
        adv_start();
        return 0;
    default:
        return 0;
    }
}

void adv_start(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids16 = (ble_uuid16_t[]) { BLE_UUID16_INIT(SVC_UUID) };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.name = (uint8_t *)ble_svc_gap_device_name();
    fields.name_len = strlen(ble_svc_gap_device_name());
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv fields rc=%d", rc); return; }
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "adv start rc=%d", rc);
    else ESP_LOGI(TAG, "advertising as PHOTO-DONGLE");
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer addr rc=%d", rc); return; }
    adv_start();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason %d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_init(void)
{
    int rc = nimble_port_init();
    if (rc != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init failed"); return; }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts count rc=%d", rc); return; }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts add rc=%d", rc); return; }
    rc = ble_svc_gap_device_name_set("PHOTO-DONGLE");
    if (rc != 0) ESP_LOGE(TAG, "gap name rc=%d", rc);
    nimble_port_freertos_init(host_task);
}

/* ---------------- slideshow + button ---------------- */
static void IRAM_ATTR button_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    xEventGroupSetBitsFromISR(s_events, EV_NEXT, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static void draw_placeholder(void)
{
    fb_fill(COL_BLACK);
    fb_text(10, 8, "PHOTO DONGLE", 2, COL_WHITE, COL_BLACK);
    if (!s_sd_ok) {
        fb_text(10, 32, "NO SD CARD", 2, COL_RED, COL_BLACK);
        fb_text(10, 52, "INSERT FAT32 SD", 1, COL_GRAY, COL_BLACK);
    } else if (s_photo_count == 0) {
        fb_text(10, 32, "NO PHOTOS", 2, COL_YELLOW, COL_BLACK);
        fb_text(10, 52, "SEND FROM IPHONE APP", 1, COL_GRAY, COL_BLACK);
    }
    fb_text(10, 68, "BLE: PHOTO-DONGLE", 1, COL_CYAN, COL_BLACK);
    draw_version_overlay();
    lcd_flush();
}

static bool show_current_photo(void)
{
    if (!s_sd_ok || s_photo_count == 0) return false;
    char path[64];
    snprintf(path, sizeof(path), "%s/PHOTO_%03d.565", SD_MOUNT_POINT, s_photo_idx[s_current]);
    bool ok = false;
    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(s_fb, 1, FRAME_BYTES, f);
        fclose(f);
        ok = (n == FRAME_BYTES);
    }
    xSemaphoreGive(s_sd_mutex);
    if (ok) {
        draw_version_overlay();
        lcd_flush();
        ESP_LOGI(TAG, "show %s (%d/%d)", path, s_current + 1, s_photo_count);
    }
    return ok;
}

static void advance_index(void)
{
    if (s_photo_count <= 1) { s_current = 0; return; }
    if (s_settings.order == 1) {
        int next;
        do { next = (int)(esp_random() % (uint32_t)s_photo_count); } while (next == s_current);
        s_current = next;
    } else {
        s_current = (s_current + 1) % s_photo_count;
    }
}

static bool show_vj_frame(int idx)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/F%03d.565", VJ_DIR, idx);
    bool ok = false;
    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(s_fb, 1, FRAME_BYTES, f);
        fclose(f);
        ok = (n == FRAME_BYTES);
    }
    xSemaphoreGive(s_sd_mutex);
    if (ok) {
        draw_version_overlay();
        lcd_flush();
    }
    return ok;
}

static void slideshow_task(void *arg)
{
    if (s_photo_count > 0) {
        show_current_photo();
    } else {
        draw_placeholder();
    }
    TickType_t wait = pdMS_TO_TICKS(s_settings.interval_s * 1000);
    float bl_phase = 0;
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(s_events, EV_NEXT | EV_CHANGED | EV_VJ,
                                               pdTRUE, pdFALSE, wait);
        if (s_vj_active && s_vj_count > 0) {
            if (bits & EV_NEXT) {
                /* BOOT button or app NEXT: leave VJ mode */
                s_vj_active = false;
                backlight_set(100);
                if (s_photo_count > 0) {
                    show_current_photo();
                } else {
                    draw_placeholder();
                }
                status_notify();
                wait = pdMS_TO_TICKS(s_settings.interval_s * 1000);
                continue;
            }
            show_vj_frame(s_vj_frame);
            s_vj_frame = (s_vj_frame + 1) % s_vj_count;
            bl_phase += 0.45f;
            backlight_set(82 + (int)(18.0f * sinf(bl_phase)));  /* breathing backlight */
            wait = pdMS_TO_TICKS(VJ_FRAME_MS);
            continue;
        }
        if (bits & EV_CHANGED) {
            backlight_set(100);
            if (s_photo_count > 0) {
                show_current_photo();
            } else {
                draw_placeholder();
            }
            wait = pdMS_TO_TICKS(s_settings.interval_s * 1000);
            continue;
        }
        if (s_photo_count == 0) {
            wait = pdMS_TO_TICKS(1000);
            continue;
        }
        /* timeout (interval elapsed) or NEXT: advance */
        advance_index();
        show_current_photo();
        status_notify();
        wait = pdMS_TO_TICKS(s_settings.interval_s * 1000);
    }
}

/* ---------------- main ---------------- */
void app_main(void)
{
    ESP_LOGI(TAG, "=== BLE Photo Dongle start ===");
    uint32_t flash_bytes = 0;
    esp_flash_get_size(NULL, &flash_bytes);
    ESP_LOGI(TAG, "flash %luMB, free heap %lu",
             (unsigned long)(flash_bytes / (1024 * 1024)),
             (unsigned long)esp_get_free_heap_size());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    settings_load();

    s_sd_mutex = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();

    lcd_init();
    fb_fill(COL_BLACK);
    fb_text(10, 30, "PHOTO DONGLE", 2, COL_WHITE, COL_BLACK);
    draw_version_overlay();
    lcd_flush();

    s_sd_ok = sd_mount();
    if (s_sd_ok) {
        mkdir(VJ_DIR, 0777);
        photos_rescan();
        vj_rescan();
    }

    /* BOOT button (GPIO9): falling-edge interrupt -> next photo */
    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << PIN_BTN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&btn);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_BTN, button_isr, NULL);

    xTaskCreate(slideshow_task, "slideshow", 4096, NULL, 5, NULL);
    ble_init();
    ESP_LOGI(TAG, "init done: %d photos, interval %us", s_photo_count, s_settings.interval_s);
}
