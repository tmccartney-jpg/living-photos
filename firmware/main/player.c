// Scene player (step 7b). See player.h.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "esp_jpeg_dec.h"
#include "sdcard.h"
#include "presence.h"
#include "player.h"

static const char *TAG = "player";

static lv_obj_t *scr_scene, *scr_back, *img, *lbl_info;
static lv_img_dsc_t dsc[2];

static void scene_click_cb(lv_event_t *e)
{
    (void)e;
    if (scr_back) lv_scr_load(scr_back);
}

void player_show(void)
{
    if (scr_scene) lv_scr_load(scr_scene);
}

static void build_screen(void)
{
    scr_back = lv_scr_act();
    scr_scene = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_scene, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr_scene, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_scene, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_scene, scene_click_cb, LV_EVENT_CLICKED, NULL);

    img = lv_img_create(scr_scene);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);

    lbl_info = lv_label_create(scr_scene);
    lv_obj_set_style_text_font(lbl_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_info, lv_color_white(), 0);
    lv_obj_set_style_bg_color(lbl_info, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lbl_info, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(lbl_info, 4, 0);
    lv_obj_align(lbl_info, LV_ALIGN_BOTTOM_LEFT, 6, -6);
    lv_label_set_text(lbl_info, "Loading scene...");
}

static void set_info(const char *text)
{
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_label_set_text(lbl_info, text);
        esp_lv_adapter_unlock();
    }
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static void player_task(void *arg)
{
    (void)arg;
    char msg[96];

    // wait for the SD card
    sd_state_t sd;
    do {
        vTaskDelay(pdMS_TO_TICKS(200));
        sdcard_get(&sd);
    } while (!sd.done);
    if (!sd.mounted) {
        set_info("No SD card - scene unavailable");
        vTaskDelete(NULL);
    }

    const char *path = PLAYER_CLIP_PATH;
    FILE *f = fopen(path, "rb");
    if (!f) {
        path = PLAYER_CLIP_FALLBACK;
        f = fopen(path, "rb");
    }
    if (!f) {
        snprintf(msg, sizeof(msg), "Clip not found:\n%s", PLAYER_CLIP_PATH);
        set_info(msg);
        ESP_LOGW(TAG, "%s", msg);
        vTaskDelete(NULL);
    }
    uint8_t hdr[32];
    if (fread(hdr, 1, 32, f) != 32 || memcmp(hdr, "LPV1", 4)) {
        set_info("Clip file is not .lpv format");
        fclose(f);
        vTaskDelete(NULL);
    }
    int w = rd16(hdr + 4), h = rd16(hdr + 6), fps = rd16(hdr + 8);
    uint32_t nframes = rd32(hdr + 12), maxjpg = rd32(hdr + 16);
    if (fps < 1 || fps > 60) fps = 15;
    ESP_LOGI(TAG, "playing %s", path);
    ESP_LOGI(TAG, "clip %dx%d @ %d fps, %lu frames, max frame %lu bytes",
             w, h, fps, (unsigned long)nframes, (unsigned long)maxjpg);

    size_t pix = (size_t)w * h * 2;
    uint8_t *jpg = heap_caps_malloc(maxjpg + 16, MALLOC_CAP_SPIRAM);
    uint8_t *buf[2] = {
        heap_caps_aligned_alloc(16, pix, MALLOC_CAP_SPIRAM),
        heap_caps_aligned_alloc(16, pix, MALLOC_CAP_SPIRAM),
    };
    if (!jpg || !buf[0] || !buf[1]) {
        set_info("Out of memory for video");
        fclose(f);
        vTaskDelete(NULL);
    }
    setvbuf(f, NULL, _IOFBF, 32 * 1024);

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;   // matches LVGL (no 16-bit swap)
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        set_info("JPEG decoder failed to start");
        fclose(f);
        vTaskDelete(NULL);
    }

    for (int i = 0; i < 2; i++) {
        memset(&dsc[i], 0, sizeof(dsc[i]));
        dsc[i].header.cf = LV_IMG_CF_TRUE_COLOR;
        dsc[i].header.w = w;
        dsc[i].header.h = h;
        dsc[i].data_size = pix;
        dsc[i].data = buf[i];
    }

    const TickType_t period = pdMS_TO_TICKS(1000 / fps);
    TickType_t wake = xTaskGetTickCount();
    uint32_t frame = 0, shown = 0;
    int back = 0;
    bool have_frame = false;
    int64_t stat_t0 = esp_timer_get_time();
    uint32_t stat_frames = 0, dec_us_sum = 0;
    float fps_now = 0, dec_ms = 0;

    while (1) {
        presence_t p;
        presence_get(&p);
        bool moving = !p.present;

        if (moving || !have_frame) {
            uint8_t lenb[4];
            if (frame >= nframes || fread(lenb, 1, 4, f) != 4) {
                fseek(f, 32, SEEK_SET);   // loop
                frame = 0;
                continue;
            }
            uint32_t len = rd32(lenb);
            if (len == 0 || len > maxjpg || fread(jpg, 1, len, f) != len) {
                ESP_LOGW(TAG, "bad frame %lu, restarting clip", (unsigned long)frame);
                fseek(f, 32, SEEK_SET);
                frame = 0;
                continue;
            }
            frame++;

            int64_t t0 = esp_timer_get_time();
            jpeg_dec_io_t io = { .inbuf = jpg, .inbuf_len = (int)len, .outbuf = buf[back] };
            jpeg_dec_header_info_t info;
            jpeg_error_t je = jpeg_dec_parse_header(dec, &io, &info);
            if (je == JPEG_ERR_OK) je = jpeg_dec_process(dec, &io);
            dec_us_sum += (uint32_t)(esp_timer_get_time() - t0);
            if (je != JPEG_ERR_OK) {
                ESP_LOGW(TAG, "decode error %d on frame %lu", je, (unsigned long)frame);
                continue;
            }

            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                lv_img_cache_invalidate_src(&dsc[back]);
                lv_img_set_src(img, &dsc[back]);
                esp_lv_adapter_unlock();
            }
            back ^= 1;
            have_frame = true;
            shown++;
            stat_frames++;
        }

        int64_t now = esp_timer_get_time();
        if (now - stat_t0 >= 1000000) {
            fps_now = stat_frames * 1e6f / (float)(now - stat_t0);
            dec_ms = stat_frames ? dec_us_sum / 1000.0f / stat_frames : 0;
            snprintf(msg, sizeof(msg), "%s   %.1f fps   decode %.0f ms   frame %lu/%lu   tap for diagnostics",
                     moving ? "AWAY - moving" : "PRESENT - frozen", fps_now, dec_ms,
                     (unsigned long)frame, (unsigned long)nframes);
            set_info(msg);
            stat_t0 = now;
            stat_frames = 0;
            dec_us_sum = 0;
        }

        vTaskDelayUntil(&wake, period);
        if ((int32_t)(xTaskGetTickCount() - wake) > (int32_t)period) {
            wake = xTaskGetTickCount();   // fell behind: don't try to catch up
        }
    }
}

void player_start(void)
{
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        build_screen();
        esp_lv_adapter_unlock();
    }
    xTaskCreatePinnedToCore(player_task, "player", 6144, NULL, 3, NULL, 0);
}
