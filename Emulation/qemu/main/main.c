#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "esp_cpu.h"

// ── Emulation switch ──────────────────────────────────────────────────────────
//  1 = QEMU / no real hardware   |   0 = real MFRC522 over SPI
#define QEMU_SIM 1

static const char *TAG = "RFID";

// Pin definitions (compiled but unused when QEMU_SIM == 1)
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

// ── Settings ──────────────────────────────────────────────────────────────────

#define MAX_UID_LEN      10
#define CARD_COOLDOWN_MS 4000
#define UID_FILTER_LEN   7     // 0 = accept all lengths

static const uint8_t MASTER_UID[]   = { 0x04, 0x8E, 0x11, 0x32, 0xB7, 0x73, 0x84 };
static const uint8_t MASTER_UID_LEN = 7;

// ── Status type (shared by real and mock paths) ───────────────────────────────

typedef enum {
    STATUS_OK,
    STATUS_ERROR,
    STATUS_TIMEOUT,
    STATUS_COLLISION,
} status_t;

// ── Card registry ─────────────────────────────────────────────────────────────

#define MAX_CARDS 16

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
    entry->uid_len      = uid_len;
    entry->id           = card_registry_size;
    entry->count        = 1;
    entry->last_seen_ms = now_ms;
    return entry;
}

// ═════════════════════════════════════════════════════════════════════════════
// REAL HARDWARE PATH  (compiled only when QEMU_SIM == 0)
// ═════════════════════════════════════════════════════════════════════════════
#if !QEMU_SIM

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
    uint8_t ver = mfrc522_read_reg(MFRC522_REG_VERSION);
    ESP_LOGI(TAG, "MFRC522 version: 0x%02X (%s)",
             ver,
             ver == 0x91 ? "v1.0" : ver == 0x92 ? "v2.0" : "unknown");
}

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
        ESP_LOGD(TAG, "Level %d SAK: 0x%02X", level, sak);
        if (!(sak & 0x04)) break;
    }

    memcpy(uid, full_uid, full_len);
    *uid_len = full_len;
    return STATUS_OK;
}

#endif // !QEMU_SIM

// ═════════════════════════════════════════════════════════════════════════════
// QEMU MOCK PATH  (compiled only when QEMU_SIM == 1)
//
// A periodic esp_timer fires every MOCK_INTERVAL_US microseconds and pushes
// a card descriptor onto a FreeRTOS queue.  The main loop's detect/get_uid
// calls drain that queue — no SPI, no GPIO, no real chip needed.
// ═════════════════════════════════════════════════════════════════════════════
#if QEMU_SIM

#define MOCK_INTERVAL_US  (1000000ULL)   // 1 s between injected scans

typedef struct {
    uint8_t uid[MAX_UID_LEN];
    uint8_t len;
} mock_card_t;

// ── Cards that will be injected in round-robin order ─────────────────────────
// Add, remove or modify entries freely.
static const mock_card_t TEST_CARDS[] = {
    { .uid = { 0x04, 0x8E, 0x11, 0x32, 0xB7, 0x73, 0x84 }, .len = 7 }, // MASTER
    { .uid = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11 }, .len = 7 }, // card 2
    { .uid = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 }, .len = 7 }, // card 3
    { .uid = { 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07 }, .len = 7 }, // card 4
    { .uid = { 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87 }, .len = 7 }, // card 5
    { .uid = { 0xB1, 0xC2, 0xD3, 0xE4, 0xF5, 0x06, 0x17 }, .len = 7 }, // card 6
    { .uid = { 0x31, 0x42, 0x53, 0x64, 0x75, 0x86, 0x97 }, .len = 7 }, // card 7
    { .uid = { 0xC1, 0xD2, 0xE3, 0xF4, 0x05, 0x16, 0x27 }, .len = 7 }, // card 8
    { .uid = { 0x41, 0x52, 0x63, 0x74, 0x85, 0x96, 0xA7 }, .len = 7 }, // card 9
    { .uid = { 0xD1, 0xE2, 0xF3, 0x04, 0x15, 0x26, 0x37 }, .len = 7 }, // card 10
    { .uid = { 0x51, 0x62, 0x73, 0x84, 0x95, 0xA6, 0xB7 }, .len = 7 }, // card 11
    { .uid = { 0xE1, 0xF2, 0x03, 0x14, 0x25, 0x36, 0x47 }, .len = 7 }, // card 12
    { .uid = { 0x61, 0x72, 0x83, 0x94, 0xA5, 0xB6, 0xC7 }, .len = 7 }, // card 13
};
#define NUM_TEST_CARDS  (sizeof(TEST_CARDS) / sizeof(TEST_CARDS[0]))

static QueueHandle_t mock_card_queue;
static int           mock_card_idx = 0;

static void mock_inject_timer_cb(void *arg)
{
    const mock_card_t *card = &TEST_CARDS[mock_card_idx];

    // ESP_LOGI(TAG, "[QEMU] Injecting card %d/%d  UID: "
    //          "%02X %02X %02X %02X %02X %02X %02X",
    //          (int)(mock_card_idx + 1), (int)NUM_TEST_CARDS,
    //          card->uid[0], card->uid[1], card->uid[2], card->uid[3],
    //          card->uid[4], card->uid[5], card->uid[6]);

    // Non-blocking — if the queue is full the scan is silently dropped and
    // will appear again on the next timer tick.
    xQueueSend(mock_card_queue, card, 0);

    mock_card_idx = (mock_card_idx + 1) % NUM_TEST_CARDS;
}

// These two functions shadow the real ones; same signatures, zero hardware.
static bool mfrc522_detect_card(void)
{
    return uxQueueMessagesWaiting(mock_card_queue) > 0;
}

static status_t mfrc522_get_uid(uint8_t *uid, uint8_t *uid_len)
{
    mock_card_t card;
    if (xQueueReceive(mock_card_queue, &card, 0) == pdTRUE) {
        memcpy(uid, card.uid, card.len);
        *uid_len = card.len;
        return STATUS_OK;
    }
    return STATUS_ERROR;
}

#endif // QEMU_SIM

// ── SPIFFS / attendance file ──────────────────────────────────────────────────

#define ATTENDANCE_FILE "/spiffs/attendance.txt"
#define MASTER_FILE     "/spiffs/master.txt"

static bool spiffs_mounted = false;

static void attendance_load(void); // forward declaration

static void master_write(void)
{
    FILE *f = fopen(MASTER_FILE, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for writing (errno %d: %s)",
                 MASTER_FILE, errno, strerror(errno));
        return;
    }

    char uid_str[MAX_UID_LEN * 3 + 1] = {0};
    for (uint8_t i = 0; i < MASTER_UID_LEN; i++) {
        char byte_str[4];
        snprintf(byte_str, sizeof(byte_str), "%02X ", MASTER_UID[i]);
        strcat(uid_str, byte_str);
    }
    size_t len = strlen(uid_str);
    if (len > 0) uid_str[len - 1] = '\0';

    fprintf(f, "MASTER UID:[%s]\n", uid_str);
    fclose(f);
    ESP_LOGI(TAG, "Master UID written to %s", MASTER_FILE);
}

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
    ESP_LOGI(TAG, "SPIFFS mounted OK — %zu KB total, %zu KB used.", total / 1024, used / 1024);

    FILE *f = fopen(ATTENDANCE_FILE, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "SPIFFS: mounted but cannot open %s (errno %d: %s). "
                      "Try: idf.py erase-flash",
                 ATTENDANCE_FILE, errno, strerror(errno));
        return;
    }
    fclose(f);

    spiffs_mounted = true;
    ESP_LOGI(TAG, "Attendance file ready: %s", ATTENDANCE_FILE);

    master_write();
    attendance_load();
}

static void attendance_load(void)
{
    FILE *f = fopen(ATTENDANCE_FILE, "r");
    if (f == NULL) {
        ESP_LOGI(TAG, "No existing attendance file — starting fresh.");
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
            ESP_LOGW(TAG, "Registry full during restore — stopping at %d cards.", restored);
            break;
        }
        card_entry_t *entry = &card_registry[card_registry_size++];
        entry->uid_len      = parse_uid(uid_str, entry->uid, MAX_UID_LEN);
        entry->id           = id;
        entry->count        = count;
        entry->last_seen_ms = 0; // cooldown not carried over
        restored++;
        ESP_LOGI(TAG, "Restored card ID=%d COUNT=%d", id, count);
    }
    fclose(f);
    ESP_LOGI(TAG, "Attendance restored: %d card(s) loaded from %s", restored, ATTENDANCE_FILE);
}

static void attendance_log(void)
{
    if (!spiffs_mounted) {
        ESP_LOGW(TAG, "SPIFFS not mounted — skipping attendance write.");
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

    // Read back and echo to serial
    f = fopen(ATTENDANCE_FILE, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for reading (errno %d: %s)",
                 ATTENDANCE_FILE, errno, strerror(errno));
        return;
    }
    printf("\n--- attendance.txt ---\n");
    char line[128];
    while (fgets(line, sizeof(line), f) != NULL)
        printf("%s", line);
    printf("----------------------\n\n");
    fclose(f);
}

// ── Main ──────────────────────────────────────────────────────────────────────

void app_main(void)
{
    // SPIFFS must be mounted before any file I/O
    spiffs_init();

#if !QEMU_SIM
    // ── Real hardware: GPIO + SPI bus + MFRC522 ───────────────────────────────
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

#else
    // ── QEMU mock: queue + periodic timer ────────────────────────────────────
    //
    // Queue depth 4: if the main loop is busy the timer can queue up to 4
    // extra scans before dropping (non-blocking xQueueSend in the callback).
    mock_card_queue = xQueueCreate(4, sizeof(mock_card_t));
    assert(mock_card_queue != NULL);

    esp_timer_handle_t mock_timer;
    const esp_timer_create_args_t timer_args = {
        .callback = mock_inject_timer_cb,
        .name     = "mock_rfid",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &mock_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(mock_timer, MOCK_INTERVAL_US));

    ESP_LOGI(TAG, "[QEMU] Mock MFRC522 active — injecting a card every %llu s",
             MOCK_INTERVAL_US / 1000000ULL);
#endif

    ESP_LOGI(TAG, "Waiting for RFID card...");

    uint8_t last_uid[MAX_UID_LEN] = {0};
    uint8_t last_uid_len = 0;
    uint32_t average = 0;
    uint8_t cnt = 0;

    while (cnt < 100) {
        if (mfrc522_detect_card()) {
            uint8_t uid[MAX_UID_LEN];
            uint8_t uid_len = 0;

            if (mfrc522_get_uid(uid, &uid_len) == STATUS_OK) {
                // Debounce: only process if UID changed since last scan
                if (uid_len != last_uid_len || memcmp(uid, last_uid, uid_len) != 0) {
                    uint32_t start = esp_cpu_get_cycle_count();
                    memcpy(last_uid, uid, uid_len);
                    last_uid_len = uid_len;
                    if (UID_FILTER_LEN != 0 && uid_len != UID_FILTER_LEN) {
                        ESP_LOGD(TAG, "Ignoring %d-byte UID (filter=%d)", uid_len, UID_FILTER_LEN);
                        continue;
                    }

                    card_entry_t *card = registry_get_or_add(uid, uid_len);
                    if (card != NULL) {
                        attendance_log();
                        uint32_t elapsed = esp_cpu_get_cycle_count() - start;
                        printf("Elapsed time: %lu\n", elapsed);
                        cnt++;
                        average = (average*(cnt-1) + elapsed)/cnt;
                        printf("Average time: %lu\n", average);
                    }
                }
            }
        } else {
            // Card absent — reset debounce so the next scan is always accepted
            memset(last_uid, 0, sizeof(last_uid));
            last_uid_len = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}