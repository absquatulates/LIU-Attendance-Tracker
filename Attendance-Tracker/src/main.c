#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_spiffs.h"

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

static spi_device_handle_t spi;

//  Settings

#define MAX_UID_LEN      10
#define CARD_COOLDOWN_MS 4000  // ignore re-scans of the same card within this window
#define UID_FILTER_LEN   7     // only accept UIDs of this byte length (0 = accept all)


/**
 * Parse a hex UID string like "04 8E 11 32 B7 73 84" into a byte array.
 * Returns the number of bytes parsed.
 */
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

// Card registry

#define MAX_CARDS 512 //16

typedef struct {
    uint8_t  uid[MAX_UID_LEN];
    uint8_t  uid_len;
    int      id;            
    int      count;         
    int64_t  last_seen_ms;  // timestamp of last accepted scan (ms)
} card_entry_t;

static card_entry_t card_registry[MAX_CARDS];
static int          card_registry_size = 0;

static card_entry_t *registry_get_or_add(const uint8_t *uid, uint8_t uid_len)
{
    int64_t now_ms = esp_timer_get_time() / 1000;

    for (int i = 0; i < card_registry_size; i++) {
        if (card_registry[i].uid_len == uid_len &&
            memcmp(card_registry[i].uid, uid, uid_len) == 0) {

            int64_t elapsed = now_ms - card_registry[i].last_seen_ms;
            if (elapsed < CARD_COOLDOWN_MS) {
                ESP_LOGD(TAG, "Card ID=%d suppressed (cooldown, %lld ms remaining)",
                         card_registry[i].id,
                         (long long)(CARD_COOLDOWN_MS - elapsed));
                return NULL; 
            }

            card_registry[i].count++;
            card_registry[i].last_seen_ms = now_ms;
            return &card_registry[i];
        }
    }
    if (card_registry_size >= MAX_CARDS) {
        ESP_LOGW(TAG, "Card registry full! Cannot register new card.");
        return NULL;
    }

    card_entry_t *entry = &card_registry[card_registry_size++];
    memcpy(entry->uid, uid, uid_len);
    entry->uid_len    = uid_len;
    entry->id         = card_registry_size;
    entry->count      = 1;
    entry->last_seen_ms = now_ms;
    return entry;
}

static uint8_t mfrc522_read_reg(uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0x00 };
    uint8_t rx[2] = { 0 };
    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_transmit(spi, &t);
    return rx[1];
}

static void mfrc522_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)((reg << 1) & 0x7E), val };
    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = tx,
    };
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

// Init 
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

    mfrc522_write_reg(MFRC522_REG_TX_ASK, 0x40); // 100% ASK modulation
    mfrc522_write_reg(MFRC522_REG_MODE,   0x3D); // CRC preset 0x6363

    mfrc522_set_bits(MFRC522_REG_TX_CONTROL, 0x03);

}

// Transceive helper

typedef enum {
    STATUS_OK,
    STATUS_ERROR,
    STATUS_TIMEOUT,
    STATUS_COLLISION,
} status_t;

static status_t mfrc522_transceive(
    uint8_t *tx_data, uint8_t tx_len,
    uint8_t *rx_data, uint8_t *rx_len,
    uint8_t bit_framing)
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
    if (rx_len) *rx_len = n;
    for (uint8_t i = 0; i < n && i < 16; i++)
        rx_data[i] = mfrc522_read_reg(MFRC522_REG_FIFO_DATA);

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

static uint8_t mfrc522_compute_bcc(uint8_t *data, uint8_t len)
{
    uint8_t bcc = 0;
    for (uint8_t i = 0; i < len; i++) bcc ^= data[i];
    return bcc;
}

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
        uint8_t *src = has_ct ? &rx[1] : &rx[0];
        uint8_t  copy = has_ct ? 3 : 4;
        memcpy(&full_uid[full_len], src, copy);
        full_len += copy;

        uint8_t select_tx[9];
        select_tx[0] = cascade_levels[level];
        select_tx[1] = 0x70;
        memcpy(&select_tx[2], rx, 4);
        select_tx[6] = mfrc522_compute_bcc(rx, 4);

        mfrc522_write_reg(0x22, 0x80);
        mfrc522_write_reg(MFRC522_REG_COMMAND,    MFRC522_CMD_IDLE);
        mfrc522_write_reg(MFRC522_REG_FIFO_LEVEL, 0x80);
        for (int i = 0; i < 7; i++)
            mfrc522_write_reg(MFRC522_REG_FIFO_DATA, select_tx[i]);
        mfrc522_write_reg(MFRC522_REG_COMMAND, 0x03); // CalcCRC
        vTaskDelay(pdMS_TO_TICKS(5));
        select_tx[7] = mfrc522_read_reg(MFRC522_REG_CRC_RESULT_L);
        select_tx[8] = mfrc522_read_reg(MFRC522_REG_CRC_RESULT_H);
        mfrc522_write_reg(MFRC522_REG_COMMAND, MFRC522_CMD_IDLE);

        uint8_t sak_buf[4];
        uint8_t sak_len = 0;
        s = mfrc522_transceive(select_tx, 9, sak_buf, &sak_len, 0);
        if (s != STATUS_OK) {
            ESP_LOGE(TAG, "Select failed at level %d", level);
            return s;
        }

        uint8_t sak = sak_buf[0];

        if (!(sak & 0x04)) break;
    }

    memcpy(uid, full_uid, full_len);
    *uid_len = full_len;
    return STATUS_OK;
}


#define ATTENDANCE_FILE "/spiffs/attendance.txt"

static bool spiffs_mounted = false;

static void attendance_load(void); 

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
                          "Check your partitions.csv has a 'spiffs' data partition.");
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

    spiffs_mounted = true;
    attendance_load();
}


static void attendance_load(void)
{
    FILE *f = fopen(ATTENDANCE_FILE, "r");
    if (f == NULL) {
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
            break;
        }

        card_entry_t *entry = &card_registry[card_registry_size++];
        entry->uid_len = parse_uid(uid_str, entry->uid, MAX_UID_LEN);
        entry->id      = id;
        entry->count   = count;
        entry->last_seen_ms = 0; // cooldown not carried over card can be scanned immediately
        restored++;
    }

    fclose(f);
}


static void attendance_log(void)
{
    if (!spiffs_mounted) {
        ESP_LOGW(TAG, "SPIFFS not mounted");
        return;
    }

    // Overwrite ("w") so there is always exactly one line per card
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
        // Trim trailing space
        size_t len = strlen(uid_str);
        if (len > 0) uid_str[len - 1] = '\0';

        fprintf(f, "ID:%-3d  COUNT:%-4d  UID:[%s]\n",
                card->id, card->count, uid_str);
    }

    fclose(f);

    //To see whats in the ATTENDANCE_FILE 
    /*
    f = fopen(ATTENDANCE_FILE, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for reading (errno %d: %s)",
                 ATTENDANCE_FILE, errno, strerror(errno));
        return;
    }

    printf("\n--- attendance.txt ---\n");
    char line[128];
    while (fgets(line, sizeof(line), f) != NULL) {
        printf("%s", line);
    }
    printf("----------------------\n\n");

    fclose(f);
    */
}


// Preloaded cards

typedef struct {
    uint8_t uid[MAX_UID_LEN];
    uint8_t uid_len;
} preloaded_card_t;

static const preloaded_card_t PRELOADED_CARDS[] = {
    { { 0x04, 0x8E, 0x11, 0x11, 0xB7, 0x73, 0x12 }, 7 },
    { { 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67 }, 7 },
    { { 0x11, 0x22, 0x33, 0x44, 0x11, 0x11, 0x11 }, 7 },
    { { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0x00 }, 7 },
    { { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0x05 }, 7 },
    { { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0x04 }, 7 },
    { { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0x03 }, 7 },
    { { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0x02 }, 7 },
    { { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0x01 }, 7 },
};
#define PRELOADED_CARDS_COUNT (sizeof(PRELOADED_CARDS) / sizeof(PRELOADED_CARDS[0]))

static void registry_preload(void)
{
    for (int i = 0; i < PRELOADED_CARDS_COUNT; i++) {
        if (card_registry_size >= MAX_CARDS) {
            ESP_LOGW(TAG, "Registry full during preload");
            break;
        }

        const preloaded_card_t *p = &PRELOADED_CARDS[i];
        bool already_exists = false;
        for (int j = 0; j < card_registry_size; j++) {
            if (card_registry[j].uid_len == p->uid_len &&
                memcmp(card_registry[j].uid, p->uid, p->uid_len) == 0) {
                already_exists = true;
                break;
            }
        }
        if (already_exists) {
            continue;
        }

        card_entry_t *entry = &card_registry[card_registry_size++];
        memcpy(entry->uid, p->uid, p->uid_len);
        entry->uid_len      = p->uid_len;
        entry->id           = card_registry_size; 
        entry->count        = 0;                  
        entry->last_seen_ms = 0;
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

    uint8_t last_uid[MAX_UID_LEN] = {0};
    uint8_t last_uid_len = 0;
    struct timespec start_t, end;
    int cnt = 0;

    while (1) {
        if (mfrc522_detect_card()) {
            uint8_t uid[MAX_UID_LEN];
            uint8_t uid_len = 0;
            
            uint32_t start = esp_cpu_get_cycle_count();
            clock_gettime(CLOCK_MONOTONIC, &start_t);

            if (mfrc522_get_uid(uid, &uid_len) == STATUS_OK) {
                if (uid_len != last_uid_len || memcmp(uid, last_uid, uid_len) != 0) {
                    
                    memcpy(last_uid, uid, uid_len);
                    last_uid_len = uid_len;

                    if (UID_FILTER_LEN != 0 && uid_len != UID_FILTER_LEN) {
                        ESP_LOGD(TAG, "Ignoring %d-byte UID (filter=%d)", uid_len, UID_FILTER_LEN);
                        continue;
                    }

                    card_entry_t *card = registry_get_or_add(uid, uid_len);
                    if (card != NULL) { 

                        attendance_log();
                        cnt++;
                        uint32_t elapsed = esp_cpu_get_cycle_count() - start;
                        printf("Elapsed time: %lu\n", elapsed);

                        clock_gettime(CLOCK_MONOTONIC, &end);
                        double elapsed_t = (end.tv_sec - start_t.tv_sec) +
                                        (end.tv_nsec - start_t.tv_nsec) / 1e9;
                        printf("Elapsed: %.6f seconds\n", elapsed_t);

                        printf("cnt: %u\n", cnt);
                    }
                }
            }
        } else {
            memset(last_uid, 0, sizeof(last_uid));
            last_uid_len = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}