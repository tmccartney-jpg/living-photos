// microSD mount + root listing (step 7a).
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "sdcard.h"

#define SD_PIN_MOSI 11
#define SD_PIN_MISO 13
#define SD_PIN_CLK  12

// CH422G output byte: bit1 touch reset, bit2 backlight, bit3 LCD reset (all high),
// bit4 SD CS (LOW = card selected), bit5 USB_SEL (must stay LOW for native USB).
#define CH422G_I2C_PORT  0
#define CH422G_OUT_ADDR  0x38
#define CH422G_OUT_SD_ON 0x0E

static const char *TAG = "sdcard";
static sd_state_t sd;
static portMUX_TYPE sd_lock = portMUX_INITIALIZER_UNLOCKED;
static sdmmc_card_t *card;

void sdcard_get(sd_state_t *out)
{
    portENTER_CRITICAL(&sd_lock);
    *out = sd;
    portEXIT_CRITICAL(&sd_lock);
}

static void fail(const char *why)
{
    ESP_LOGW(TAG, "%s", why);
    portENTER_CRITICAL(&sd_lock);
    snprintf(sd.error, sizeof(sd.error), "%s", why);
    sd.done = true;
    portEXIT_CRITICAL(&sd_lock);
}

static void sd_task(void *arg)
{
    (void)arg;
    sd_state_t r = {0};

    // Select the card: CH422G EXIO4 low (keeps backlight on, USB_SEL low).
    uint8_t out = CH422G_OUT_SD_ON;
    if (i2c_master_write_to_device(CH422G_I2C_PORT, CH422G_OUT_ADDR, &out, 1, pdMS_TO_TICKS(1000)) != ESP_OK) {
        fail("CH422G write failed");
        vTaskDelete(NULL);
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32 * 1024,
    };
    if (spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA) != ESP_OK) {
        fail("SPI bus init failed");
        vTaskDelete(NULL);
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = -1;
    slot.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,   // never wipe the card
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &card);
    if (err != ESP_OK) {
        fail(err == ESP_FAIL ? "no FAT32 filesystem" : "no card / card not responding");
        vTaskDelete(NULL);
    }

    snprintf(r.card_name, sizeof(r.card_name), "%s", card->cid.name);
    r.size_mb = (uint32_t)(((uint64_t)card->csd.capacity * card->csd.sector_size) / (1024 * 1024));
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &freeb) == ESP_OK) {
        r.free_mb = (uint32_t)(freeb / (1024 * 1024));
    }

    DIR *d = opendir(SD_MOUNT_POINT);
    if (d) {
        struct dirent *e;
        int n = 0;
        size_t used = 0;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            if (!strcmp(e->d_name, "System Volume Information")) continue;
            n++;
            ESP_LOGI(TAG, "  %s%s", e->d_name, e->d_type == DT_DIR ? "/" : "");
            if (used < sizeof(r.listing) - 20) {
                int w = snprintf(r.listing + used, sizeof(r.listing) - used, "%s%s%s",
                                 used ? ", " : "", e->d_name, e->d_type == DT_DIR ? "/" : "");
                if (w > 0) used += (size_t)w;
                if (used >= sizeof(r.listing)) used = sizeof(r.listing) - 1;
            }
        }
        closedir(d);
        r.file_count = n;
        if (used >= sizeof(r.listing) - 20 && n > 0) {
            snprintf(r.listing + sizeof(r.listing) - 5, 5, "...");
        }
    }

    r.mounted = true;
    r.done = true;
    ESP_LOGI(TAG, "mounted: %s, %lu MB, %lu MB free, %d entries",
             r.card_name, (unsigned long)r.size_mb, (unsigned long)r.free_mb, r.file_count);
    portENTER_CRITICAL(&sd_lock);
    sd = r;
    portEXIT_CRITICAL(&sd_lock);
    vTaskDelete(NULL);
}

void sdcard_start(void)
{
    xTaskCreatePinnedToCore(sd_task, "sdcard", 4096, NULL, 3, NULL, 0);
}
