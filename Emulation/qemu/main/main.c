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
#include <time.h>

static const char *TAG = "RFID";

#define PIN_MOSI  GPIO_NUM_3
#define PIN_MISO  GPIO_NUM_5
#define PIN_SCK   GPIO_NUM_4
#define PIN_CS    GPIO_NUM_7
#define PIN_RST   GPIO_NUM_0


// PICC commands
#define PICC_CMD_REQA              0x26
#define PICC_CMD_ANTICOLL          0x93

//  Settings 

#define MAX_UID_LEN      10
#define CARD_COOLDOWN_MS 40
#define UID_FILTER_LEN   7     // 0 = accept all lengths


// Status type (shared by real and mock paths)

typedef enum {
    STATUS_OK,
    STATUS_ERROR,
    STATUS_TIMEOUT,
    STATUS_COLLISION,
} status_t;

// ── Card registry ─────────────────────────────────────────────────────────────

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


#define MOCK_INTERVAL_US  (500000ULL)  

typedef struct {
    uint8_t uid[MAX_UID_LEN];
    uint8_t len;
} mock_card_t;

static const mock_card_t TEST_CARDS[] = {
    { .uid = { 0x04, 0x8E, 0x11, 0x32, 0xB7, 0x73, 0x84 }, .len = 7 }, 
    { .uid = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11 }, .len = 7 }, 
    { .uid = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 }, .len = 7 }, 
    { .uid = { 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07 }, .len = 7 }, 
    { .uid = { 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87 }, .len = 7 }, 
    { .uid = { 0xB1, 0xC2, 0xD3, 0xE4, 0xF5, 0x06, 0x17 }, .len = 7 }, 
    { .uid = { 0x31, 0x42, 0x53, 0x64, 0x75, 0x86, 0x97 }, .len = 7 }, 
    { .uid = { 0xC1, 0xD2, 0xE3, 0xF4, 0x05, 0x16, 0x27 }, .len = 7 }, 
    { .uid = { 0x41, 0x52, 0x63, 0x74, 0x85, 0x96, 0xA7 }, .len = 7 }, 
    { .uid = { 0xD1, 0xE2, 0xF3, 0x04, 0x15, 0x26, 0x37 }, .len = 7 }, 
};
#define NUM_TEST_CARDS  (sizeof(TEST_CARDS) / sizeof(TEST_CARDS[0]))

static QueueHandle_t mock_card_queue;
static int           mock_card_idx = 0;

static void mock_inject_timer_cb(void *arg)
{
    const mock_card_t *card = &TEST_CARDS[mock_card_idx];

    xQueueSend(mock_card_queue, card, 0);

    mock_card_idx = (mock_card_idx + 1) % NUM_TEST_CARDS;
}

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

// SPIFFS / attendance file

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
        ESP_LOGE(TAG, "SPIFFS: mounted but cannot open %s (errno %d: %s). ",
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
        ESP_LOGI(TAG, "No existing attendance file");
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
        entry->last_seen_ms = 0; 
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

//     f = fopen(ATTENDANCE_FILE, "r");
//     if (f == NULL) {
//         ESP_LOGE(TAG, "Failed to open %s for reading (errno %d: %s)",
//                  ATTENDANCE_FILE, errno, strerror(errno));
//         return;
//     }
//     printf("\n--- attendance.txt ---\n");
//     char line[128];
//     while (fgets(line, sizeof(line), f) != NULL)
//         printf("%s", line);
//     printf("----------------------\n\n");
//     fclose(f);
}

// Main 

void app_main(void)
{
    
    spiffs_init();

    mock_card_queue = xQueueCreate(4, sizeof(mock_card_t));
    assert(mock_card_queue != NULL);

    esp_timer_handle_t mock_timer;
    const esp_timer_create_args_t timer_args = {
        .callback = mock_inject_timer_cb,
        .name     = "mock_rfid",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &mock_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(mock_timer, MOCK_INTERVAL_US));

    ESP_LOGI(TAG, "Waiting for RFID card...");

    uint8_t last_uid[MAX_UID_LEN] = {0};
    uint8_t last_uid_len = 0;
    uint32_t average = 0;
    uint16_t cnt = 0;
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
                    if (card != NULL) {
                        attendance_log();
                        uint32_t elapsed = esp_cpu_get_cycle_count() - start;
                        printf("Elapsed time: %lu\n", elapsed);
                        cnt++;

                        clock_gettime(CLOCK_MONOTONIC, &end);
                        double elapsed_t = (end.tv_sec - start_t.tv_sec) +
                                        (end.tv_nsec - start_t.tv_nsec) / 1e9;
                        printf("Elapsed: %.6f seconds\n", elapsed_t);

                        printf("cnt: %u\n", cnt);
                        vTaskDelay(pdMS_TO_TICKS(10));  

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