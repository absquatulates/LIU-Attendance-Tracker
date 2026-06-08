#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "esp_cpu.h"
//#include "esp_clk.h"


static const char *TAG = "RFID";

// Pin definitions 
#define PIN_MOSI  GPIO_NUM_3
#define PIN_MISO  GPIO_NUM_5
#define PIN_SCK   GPIO_NUM_4
#define PIN_CS    GPIO_NUM_7
#define PIN_RST   GPIO_NUM_0

// MFRC522 registers
#define MFRC522_REG_COMMAND        0x01
#define MFRC522_REG_COM_I_EN       0x02
#define MFRC522_REG_COM_IRQ        0x04
#define MFRC522_REG_ERROR          0x06
#define MFRC522_REG_FIFO_DATA      0x09
#define MFRC522_REG_FIFO_LEVEL     0x0A
#define MFRC522_REG_CONTROL        0x0C
#define MFRC522_REG_BIT_FRAMING    0x0D
#define MFRC522_REG_MODE           0x11
#define MFRC522_REG_TX_CONTROL     0x14
#define MFRC522_REG_TX_ASK         0x15
#define MFRC522_REG_CRC_RESULT_H   0x21
#define MFRC522_REG_CRC_RESULT_L   0x22
#define MFRC522_REG_VERSION        0x37

// MFRC522 commands
#define MFRC522_CMD_IDLE           0x00
#define MFRC522_CMD_TRANSCEIVE     0x0C
#define MFRC522_CMD_SOFT_RESET     0x0F

// PICC commands
#define PICC_CMD_REQA              0x26
#define PICC_CMD_ANTICOLL          0x93

// Settings

#define MAX_UID_LEN      10
#define CARD_COOLDOWN_MS 200


#define UID_FILTER_LEN   4



// Status type 

typedef enum {
    STATUS_OK,
    STATUS_ERROR,
    STATUS_TIMEOUT,
    STATUS_COLLISION,
} status_t;

// Card registry

#define MAX_CARDS 512

typedef struct {
    uint8_t  uid[MAX_UID_LEN];
    uint8_t  uid_len;
    int      id;
    int      count;
    int64_t  last_seen_ms;
} card_entry_t;

static card_entry_t card_registry[MAX_CARDS];
static int          card_registry_size = 0;

static uint8_t parse_uid(const char *str, uint8_t *out, uint8_t max_len)
{
    uint8_t count = 0;
    const char *p = str;
    while (*p && count < max_len) {
        while (*p == ' ') p++;
        if (!*p) break;
        out[count++] = (uint8_t)strtol(p, (char **)&p, 16);
    }
    return count;
}

static card_entry_t *registry_get_or_add(const uint8_t *uid, uint8_t uid_len)
{
    int64_t now_ms = esp_timer_get_time() / 1000;

    for (int i = 0; i < card_registry_size; i++) {
        if (card_registry[i].uid_len == uid_len &&
            memcmp(card_registry[i].uid, uid, uid_len) == 0) {

            int64_t elapsed = now_ms - card_registry[i].last_seen_ms;
            if (elapsed < CARD_COOLDOWN_MS) {
                return NULL;
            }

            card_registry[i].count++;
            card_registry[i].last_seen_ms = now_ms;
            return &card_registry[i];
        }
    }

    if (card_registry_size >= MAX_CARDS) {
        ESP_LOGW(TAG, "Card registry full");
        return NULL;
    }

    card_entry_t *entry = &card_registry[card_registry_size++];
    memcpy(entry->uid, uid, uid_len);
    entry->uid_len      = uid_len;
    entry->id           = card_registry_size;
    entry->count        = 1;
    entry->last_seen_ms = now_ms;
    return entry;
}

static spi_device_handle_t spi;

static uint8_t mfrc522_read_reg(uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0x00 };
    uint8_t rx[2] = { 0 };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_transmit(spi, &t);
    return rx[1];
}

static void mfrc522_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)((reg << 1) & 0x7E), val };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx };
    spi_device_transmit(spi, &t);
}

static void mfrc522_set_bits(uint8_t reg, uint8_t mask)
{
    mfrc522_write_reg(reg, mfrc522_read_reg(reg) | mask);
}

static void mfrc522_clear_bits(uint8_t reg, uint8_t mask)
{
    mfrc522_write_reg(reg, mfrc522_read_reg(reg) & (~mask));
}

static void mfrc522_read_fifo_burst(uint8_t *buf, uint8_t count)
{
    if (count == 0) return;
    uint8_t tx[17] = {0};
    uint8_t rx[17] = {0};
    tx[0] = (uint8_t)(((MFRC522_REG_FIFO_DATA << 1) & 0x7E) | 0x80);
    spi_transaction_t t = {
        .length    = (count + 1) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_transmit(spi, &t);
    memcpy(buf, &rx[1], count);
}

static void mfrc522_reset(void)
{
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    mfrc522_write_reg(MFRC522_REG_COMMAND, MFRC522_CMD_SOFT_RESET);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void mfrc522_init(void)
{
    mfrc522_reset();
    mfrc522_write_reg(MFRC522_REG_TX_ASK, 0x40);
    mfrc522_write_reg(MFRC522_REG_MODE,   0x3D);
    mfrc522_set_bits(MFRC522_REG_TX_CONTROL, 0x03);
}

static status_t mfrc522_transceive(uint8_t *tx_data, uint8_t tx_len, uint8_t *rx_data, uint8_t *rx_len, uint8_t bit_framing)
{
    mfrc522_write_reg(MFRC522_REG_COMMAND,    MFRC522_CMD_IDLE);
    mfrc522_write_reg(MFRC522_REG_COM_IRQ,    0x7F);
    mfrc522_write_reg(MFRC522_REG_FIFO_LEVEL, 0x80);

    for (uint8_t i = 0; i < tx_len; i++)
        mfrc522_write_reg(MFRC522_REG_FIFO_DATA, tx_data[i]);

    mfrc522_write_reg(MFRC522_REG_BIT_FRAMING, bit_framing);
    mfrc522_write_reg(MFRC522_REG_COMMAND,     MFRC522_CMD_TRANSCEIVE);
    mfrc522_set_bits(MFRC522_REG_BIT_FRAMING,  0x80); // StartSend

    uint16_t timeout = 2000;
    uint8_t irq;
    do {
        irq = mfrc522_read_reg(MFRC522_REG_COM_IRQ);
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (--timeout && !(irq & 0x31));

    mfrc522_clear_bits(MFRC522_REG_BIT_FRAMING, 0x80);

    if (!timeout)   return STATUS_TIMEOUT;
    if (irq & 0x01) return STATUS_ERROR;

    uint8_t err = mfrc522_read_reg(MFRC522_REG_ERROR);
    if (err & 0x13) return STATUS_COLLISION;

    uint8_t n = mfrc522_read_reg(MFRC522_REG_FIFO_LEVEL);
    if (n > 16) n = 16;
    if (rx_len) *rx_len = n;

    // Burst read instead of byte-by-byte fixes Wokwi FIFO corruption
    mfrc522_read_fifo_burst(rx_data, n);

    return STATUS_OK;
}

static bool mfrc522_detect_card(void)
{
    uint8_t buf[2];
    uint8_t len;
    uint8_t req = PICC_CMD_REQA;
    mfrc522_write_reg(MFRC522_REG_BIT_FRAMING, 0x07);
    status_t s = mfrc522_transceive(&req, 1, buf, &len, 0x07);
    return (s == STATUS_OK && len == 2);
}

/*
static uint8_t mfrc522_compute_bcc(uint8_t *data, uint8_t len)
{
    uint8_t bcc = 0;
    for (uint8_t i = 0; i < len; i++) bcc ^= data[i];
    return bcc;
}

static void compute_crc_a(uint8_t *data, uint8_t len, uint8_t *crc_l, uint8_t *crc_h)
{
    uint32_t crc = 0x6363; // ISO 14443-3 CRC-A preset
    for (uint8_t i = 0; i < len; i++) {
        uint8_t b = data[i] ^ (uint8_t)(crc & 0xFF);
        b = b ^ (b << 4);
        crc = (crc >> 8) ^ ((uint32_t)b << 8) ^ ((uint32_t)b << 3) ^ ((uint32_t)b >> 4);
    }
    *crc_l = (uint8_t)(crc & 0xFF);
    *crc_h = (uint8_t)((crc >> 8) & 0xFF);
}
*/

static status_t mfrc522_get_uid(uint8_t *uid, uint8_t *uid_len)
{
    uint8_t cascade_levels[] = { 0x93, 0x95, 0x97 };
    uint8_t full_uid[10];
    uint8_t full_len = 0;

    for (int level = 0; level < 3; level++) {
        uint8_t rx[16];
        uint8_t rx_len = 0;

        uint8_t anticoll_tx[2] = { cascade_levels[level], 0x20 };
        status_t s = mfrc522_transceive(anticoll_tx, 2, rx, &rx_len, 0);
        if (s != STATUS_OK) {
            ESP_LOGE(TAG, "Anticoll failed at level %d", level);
            return s;
        }

        bool has_ct = (rx[0] == 0x88);
        uint8_t *src  = has_ct ? &rx[1] : &rx[0];
        uint8_t  copy = has_ct ? 3 : 4;
        memcpy(&full_uid[full_len], src, copy);
        full_len += copy;


        if (!has_ct) break;   

    }

    memcpy(uid, full_uid, full_len);
    *uid_len = full_len;
    return STATUS_OK;
}

//  SPIFFS / attendance file

#define ATTENDANCE_FILE "/spiffs/attendance.txt"

static bool spiffs_mounted = false;

static void attendance_load(void); // forward declaration

static void spiffs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = NULL,
        .max_files              = 4,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    switch (ret) {
        case ESP_OK:
            break;
        case ESP_FAIL:
            ESP_LOGE(TAG, "SPIFFS: failed to mount or format the filesystem.");
            return;
        case ESP_ERR_NOT_FOUND:
            ESP_LOGE(TAG, "SPIFFS: partition not found. "
                          "Check partitions.csv has a 'spiffs' data partition.");
            return;
        default:
            ESP_LOGE(TAG, "SPIFFS: unexpected error (%s).", esp_err_to_name(ret));
            return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS: could not read partition info (%s).", esp_err_to_name(ret));
        return;
    }

    FILE *f = fopen(ATTENDANCE_FILE, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "SPIFFS: mounted but cannot open %s (errno %d: %s). "
                      "Try: idf.py erase-flash",
                 ATTENDANCE_FILE, errno, strerror(errno));
        return;
    }
    fclose(f);

    spiffs_mounted = true;
    attendance_load();
}

static void attendance_load(void)
{
    FILE *f = fopen(ATTENDANCE_FILE, "r");
    if (f == NULL) {
        ESP_LOGI(TAG, "No existing attendance file.");
        return;
    }

    char line[128];
    int restored = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        int id, count;
        char uid_str[64] = {0};
        if (sscanf(line, "ID:%d COUNT:%d UID:[%63[^]]]", &id, &count, uid_str) != 3)
            continue;
        if (card_registry_size >= MAX_CARDS) {
            ESP_LOGW(TAG, "Registry full during restore");
            break;
        }
        card_entry_t *entry = &card_registry[card_registry_size++];
        entry->uid_len      = parse_uid(uid_str, entry->uid, MAX_UID_LEN);
        entry->id           = id;
        entry->count        = count;
        entry->last_seen_ms = 0; // cooldown not carried over
        restored++;
    }
    fclose(f);
}

static void attendance_log(void)
{
    if (!spiffs_mounted) {
        ESP_LOGW(TAG, "SPIFFS not mounted skipping attendance write.");
        return;
    }

    FILE *f = fopen(ATTENDANCE_FILE, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for writing (errno %d: %s)",
                 ATTENDANCE_FILE, errno, strerror(errno));
        return;
    }

    for (int i = 0; i < card_registry_size; i++) {
        const card_entry_t *card = &card_registry[i];

        char uid_str[MAX_UID_LEN * 3 + 1] = {0};
        for (uint8_t b = 0; b < card->uid_len; b++) {
            char byte_str[4];
            snprintf(byte_str, sizeof(byte_str), "%02X ", card->uid[b]);
            strcat(uid_str, byte_str);
        }
        size_t len = strlen(uid_str);
        if (len > 0) uid_str[len - 1] = '\0';

        fprintf(f, "ID:%-3d  COUNT:%-4d  UID:[%s]\n",
                card->id, card->count, uid_str);
    }
    fclose(f);

    // f = fopen(ATTENDANCE_FILE, "r");
    // if (f == NULL) {
    //     ESP_LOGE(TAG, "Failed to open %s for reading (errno %d: %s)",
    //              ATTENDANCE_FILE, errno, strerror(errno));
    //     return;
    // }
    // printf("\n--- attendance.txt ---\n");
    // char line[128];
    // while (fgets(line, sizeof(line), f) != NULL)
    //     printf("%s", line);
    // printf("----------------------\n\n");
    // fclose(f);
}

static void registry_preload(void)
{
    // 4-byte UIDs to match UID_FILTER_LEN == 4
    static const struct { uint8_t uid[MAX_UID_LEN]; uint8_t len; } PRELOAD[] = {
        { { 0xAA, 0xBB, 0xCC, 0xDD }, 4 },
        { { 0x11, 0x22, 0x33, 0x44 }, 4 },
        { { 0xDE, 0xAD, 0xBE, 0xEF }, 4 },
        { { 0xCA, 0xFE, 0xBA, 0xBE }, 4 },
        { { 0xCA, 0xFE, 0xBA, 0xB4 }, 4 },
        { { 0xCA, 0xFE, 0xBA, 0xB5 }, 4 },
        { { 0xCA, 0xFE, 0xBA, 0xB3 }, 4 },
        { { 0xCA, 0xFE, 0xBA, 0xB2 }, 4 },
        { { 0xCA, 0xFE, 0xBA, 0xB1 }, 4 },
    };

    for (int i = 0; i < 9; i++) {
        bool found = false;
        for (int j = 0; j < card_registry_size; j++) {
            if (card_registry[j].uid_len == PRELOAD[i].len &&
                memcmp(card_registry[j].uid, PRELOAD[i].uid, PRELOAD[i].len) == 0) {
                found = true;
                break;
            }
        }
        if (found) continue;

        if (card_registry_size >= MAX_CARDS) {
            ESP_LOGW(TAG, "Registry full");
            break;
        }

        card_entry_t *e = &card_registry[card_registry_size++];
        memcpy(e->uid, PRELOAD[i].uid, PRELOAD[i].len);
        e->uid_len      = PRELOAD[i].len;
        e->id           = card_registry_size;   // 1-based
        e->count        = 0;
        e->last_seen_ms = 0;

        char uid_str[MAX_UID_LEN * 3 + 1] = {0};
        for (uint8_t b = 0; b < e->uid_len; b++) {
            char tmp[4];
            snprintf(tmp, sizeof(tmp), "%02X ", e->uid[b]);
            strcat(uid_str, tmp);
        }
    }
}

// Main

void app_main(void)
{
    spiffs_init();
    registry_preload();


    gpio_config_t rst_conf = {
        .pin_bit_mask = (1ULL << PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_conf);
    gpio_set_level(PIN_RST, 1);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = PIN_MOSI,
        .miso_io_num   = PIN_MISO,
        .sclk_io_num   = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 5 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = PIN_CS,
        .queue_size     = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &spi));

    mfrc522_init();


    ESP_LOGI(TAG, "Waiting for RFID card...");

    uint8_t last_uid[MAX_UID_LEN] = {0};
    uint8_t last_uid_len = 0;
    uint32_t cnt = 0;
    struct timespec start_t, end;

    while (cnt < 1000) {
        if (mfrc522_detect_card()) {
            uint8_t uid[MAX_UID_LEN];
            uint8_t uid_len = 0;
            clock_gettime(CLOCK_MONOTONIC, &start_t);
            uint32_t start = esp_cpu_get_cycle_count();

            if (mfrc522_get_uid(uid, &uid_len) == STATUS_OK) {
                if (uid_len != last_uid_len || memcmp(uid, last_uid, uid_len) != 0) {

                    memcpy(last_uid, uid, uid_len);
                    last_uid_len = uid_len;
                    if (UID_FILTER_LEN != 0 && uid_len != UID_FILTER_LEN) {
                        ESP_LOGD(TAG, "Ignoring %d-byte UID (filter=%d)", uid_len, UID_FILTER_LEN);
                        continue;
                    }

                    card_entry_t *card = registry_get_or_add(uid, uid_len);

                    memset(last_uid, 0, sizeof(last_uid));
                    last_uid_len = 0;

                    if (card != NULL) {
                        attendance_log();
                        uint32_t elapsed = esp_cpu_get_cycle_count() - start;

                        clock_gettime(CLOCK_MONOTONIC, &end);
                        double elapsed_t = (end.tv_sec - start_t.tv_sec) +
                                        (end.tv_nsec - start_t.tv_nsec) / 1e9;
                        printf("Elapsed: %.6f seconds\n", elapsed_t);

                        printf("Elapsed time: %lu\n", elapsed);
                        cnt++;
                    }
                }
            }
        } else {
            memset(last_uid, 0, sizeof(last_uid));
            last_uid_len = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}