#include <dirent.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "dev_audio_codec.h"
#include "dev_display_lcd.h"
#include "dev_lcd_touch.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_codec_dev.h"

#include <mgba-util/audio-buffer.h>
#include <mgba-util/audio-resampler.h>
#include <mgba-util/image.h>
#include <mgba-util/vfs.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/input.h>

#include "rom_cache.h"

#include "miniz.h"

#define GBA_WIDTH          240
#define GBA_HEIGHT         160
#define GBA_SCALE          2
#define GBA_FPS            60
#define DEFAULT_FRAMESKIP  0
#define MAX_FRAMESKIP      5
#define DISPLAY_FRAME_DIV  1
#define ROM_DIR            "/sdcard"
#define SETTINGS_PATH      ROM_DIR "/esp-mgba.cfg"
#define ROM_PATH_MAX       256
#define ROM_LIST_MAX       64
#define ROM_VISIBLE_MAX    8
#define ZIP_IO_CHUNK       4096
#define ZIP_DICT_SIZE      TINFL_LZ_DICT_SIZE
#define STATE_SLOT_COUNT   5
#define STATE_PATH_MAX     320
#define EMU_TASK_STACK     (16 * 1024)
#define EMU_TASK_PRIORITY  6
#define EMU_TASK_CORE      0
#define RENDER_TASK_STACK  (4 * 1024)
#define RENDER_TASK_PRIORITY 4
#define RENDER_TASK_CORE   1
#define TOUCH_TASK_STACK   (4 * 1024)
#define TOUCH_TASK_PRIORITY 5
#define TOUCH_TASK_CORE    1
#define AUDIO_TASK_STACK   (6 * 1024)
#define AUDIO_TASK_PRIORITY 6
#define AUDIO_TASK_CORE    1
#define AUDIO_CORE_SAMPLE_RATE 32768
#define AUDIO_CODEC_SAMPLE_RATE 48000
#define AUDIO_BUFFER_SAMPLES 2048
#define AUDIO_CHUNK_SAMPLES 256
#define AUDIO_RING_CHUNKS   96
#define AUDIO_MAX_POST_CHUNKS 8
#define AUDIO_RESAMPLE_SOURCE_SAMPLES 4096
#define AUDIO_RESAMPLE_DEST_SAMPLES 4096
#define AUDIO_CODEC_WRITE_SAMPLES 512
#define MEM_MONITOR_CORE   1
#define TOUCH_MAX_POINTS   5
#define TOUCH_POLL_MS      15
#define TOUCH_RELEASE_POLL_MS 20
#define MEM_MONITOR_STACK  (3 * 1024)
#define MEM_MONITOR_PRIORITY 1
#define MEM_MONITOR_INTERVAL_MS 5000
#define AUDIO_PA_GPIO      GPIO_NUM_7
#define SD_POWER_GPIO      GPIO_NUM_39
#define FRAMEBUFFER_ALIGN  64
#define FONT_SCALE         3
#define FONT_W             5
#define FONT_H             7
#define USE_PPA_GAME_SCALE 1

#define RGB565(r, g, b)    (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3))
#define KEY_MASK(key)      (1U << (key))

static const char *TAG = "esp-mgba";

typedef struct {
    struct mAVStream stream;
} esp_audio_stream_t;

typedef struct {
    const char *label;
    uint32_t key;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint16_t color;
} pad_button_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} rect_t;

typedef enum {
    UI_MODE_ROM_SELECT,
    UI_MODE_GAME,
    UI_MODE_GAME_MENU,
} ui_mode_t;

typedef enum {
    EMU_CMD_NONE,
    EMU_CMD_RESUME,
    EMU_CMD_SAVE_STATE,
    EMU_CMD_LOAD_STATE,
    EMU_CMD_DELETE_STATE,
    EMU_CMD_TOGGLE_FAST,
    EMU_CMD_TOGGLE_AUDIO,
    EMU_CMD_VOLUME_DOWN,
    EMU_CMD_VOLUME_UP,
    EMU_CMD_CYCLE_FRAMESKIP,
    EMU_CMD_TOGGLE_SKIP_BIOS,
    EMU_CMD_TOGGLE_AUDIO_SYNC,
    EMU_CMD_TOGGLE_VIDEO_SYNC,
    EMU_CMD_TOGGLE_SHOW_FPS,
    EMU_CMD_TOGGLE_SCALE_LINEAR,
    EMU_CMD_ROM_MENU,
} emu_cmd_t;

typedef struct {
    const char *label;
    emu_cmd_t cmd;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint16_t color;
} menu_button_t;

static const pad_button_t s_buttons[] = {
    { "UP",     KEY_MASK(GBA_KEY_UP),      56,  176, 48, 48, RGB565(40, 54, 62) },
    { "LEFT",   KEY_MASK(GBA_KEY_LEFT),     8,  224, 48, 48, RGB565(40, 54, 62) },
    { "RIGHT",  KEY_MASK(GBA_KEY_RIGHT),  104,  224, 48, 48, RGB565(40, 54, 62) },
    { "DOWN",   KEY_MASK(GBA_KEY_DOWN),    56,  272, 48, 48, RGB565(40, 54, 62) },
    { "B",      KEY_MASK(GBA_KEY_B),      646,  242, 66, 58, RGB565(28, 104, 120) },
    { "A",      KEY_MASK(GBA_KEY_A),      724,  180, 66, 58, RGB565(28, 104, 120) },
    { "L",      KEY_MASK(GBA_KEY_L),       18,   24, 92, 42, RGB565(58, 66, 70) },
    { "R",      KEY_MASK(GBA_KEY_R),      690,   24, 92, 42, RGB565(58, 66, 70) },
    { "SELECT", KEY_MASK(GBA_KEY_SELECT), 264,  426, 108, 38, RGB565(64, 64, 64) },
    { "START",  KEY_MASK(GBA_KEY_START),  428,  426, 108, 38, RGB565(64, 64, 64) },
    { "MENU",   0,                        346,   24, 108, 38, RGB565(72, 72, 82) },
};

static const menu_button_t s_menu_buttons[] = {
    { "RESUME",  EMU_CMD_RESUME,             208,  84, 118, 36, RGB565(44, 72, 74) },
    { "SCALE",   EMU_CMD_TOGGLE_SCALE_LINEAR,340,  84, 118, 36, RGB565(84, 78, 108) },
    { "ROMS",    EMU_CMD_ROM_MENU,           472,  84, 118, 36, RGB565(68, 68, 76) },
    { "SAVE",    EMU_CMD_SAVE_STATE,         208, 156, 118, 36, RGB565(28, 104, 120) },
    { "LOAD",    EMU_CMD_LOAD_STATE,         340, 156, 118, 36, RGB565(58, 76, 116) },
    { "DELETE",  EMU_CMD_DELETE_STATE,       472, 156, 118, 36, RGB565(112, 48, 48) },
    { "FAST",    EMU_CMD_TOGGLE_FAST,        208, 276, 118, 36, RGB565(118, 88, 34) },
    { "AUDIO",   EMU_CMD_TOGGLE_AUDIO,       340, 276, 118, 36, RGB565(52, 92, 116) },
    { "VOL-",    EMU_CMD_VOLUME_DOWN,        472, 276, 118, 36, RGB565(70, 70, 84) },
    { "VOL+",    EMU_CMD_VOLUME_UP,          208, 322, 118, 36, RGB565(70, 70, 84) },
    { "FSKIP",   EMU_CMD_CYCLE_FRAMESKIP,    340, 322, 118, 36, RGB565(82, 74, 120) },
    { "FPS",     EMU_CMD_TOGGLE_SHOW_FPS,    472, 322, 118, 36, RGB565(82, 92, 74) },
    { "SKIP",    EMU_CMD_TOGGLE_SKIP_BIOS,   208, 368, 118, 36, RGB565(82, 86, 68) },
    { "ASYNC",   EMU_CMD_TOGGLE_AUDIO_SYNC,  340, 368, 118, 36, RGB565(76, 86, 112) },
    { "VSYNC",   EMU_CMD_TOGGLE_VIDEO_SYNC,  472, 368, 118, 36, RGB565(76, 96, 94) },
};

static _Atomic uint32_t s_keys;
static _Atomic ui_mode_t s_ui_mode;
static _Atomic bool s_fast_forward;
static _Atomic bool s_audio_enabled;
static _Atomic bool s_audio_paused;
static _Atomic uint8_t s_audio_volume;
static _Atomic uint8_t s_frameskip;
static _Atomic bool s_skip_bios;
static _Atomic bool s_audio_sync;
static _Atomic bool s_video_sync;
static _Atomic bool s_show_fps;
static _Atomic bool s_scale_linear = true;
static _Atomic size_t s_rom_list_offset;
static _Atomic uint8_t s_state_slot;
static SemaphoreHandle_t s_ui_lock;
static QueueHandle_t s_rom_queue;
static QueueHandle_t s_emu_cmd_queue;
static mColor *s_framebuffers[2];
static mColor *s_active_framebuffer;
static mColor *s_render_framebuffer;
static SemaphoreHandle_t s_frame_lock;
static TaskHandle_t s_render_task_handle;
static uint16_t *s_lcd_fb;
static uint16_t s_lcd_width;
static uint16_t s_lcd_height;
static esp_lcd_touch_handle_t s_touch_handle;
static TaskHandle_t s_touch_task_handle;
static bool s_touch_irq_mode;
static esp_codec_dev_handle_t s_audio_codec;
static RingbufHandle_t s_audio_ringbuf;
static esp_audio_stream_t s_audio_stream;
static _Atomic uint32_t s_audio_source_rate;
static _Atomic uint32_t s_audio_post_count;
static _Atomic uint32_t s_audio_write_count;
static _Atomic uint32_t s_audio_drop_count;
static bool s_audio_codec_opened;
static _Atomic bool s_audio_codec_wanted;
static bool s_audio_pa_enabled;
static uint8_t s_audio_codec_volume_applied;
static struct mAudioBuffer s_audio_resample_source;
static struct mAudioBuffer s_audio_resample_dest;
static struct mAudioResampler s_audio_resampler;
static int16_t s_audio_write_buf[AUDIO_CODEC_WRITE_SAMPLES * 2];
static const int16_t s_audio_silence_chunk[AUDIO_CHUNK_SAMPLES * 2];
static ppa_client_handle_t s_ppa_srm;
static SemaphoreHandle_t s_ppa_done_sem;
static void *s_rom_data;
static size_t s_rom_size;
static char s_loaded_rom_path[ROM_PATH_MAX];
static char s_rom_paths[ROM_LIST_MAX][ROM_PATH_MAX];
static size_t s_rom_sizes[ROM_LIST_MAX];
static size_t s_rom_count;
static bool s_game_running;
static rect_t s_game_rect;
static char s_state_path[STATE_PATH_MAX];

static void fill_rect(int x, int y, int w, int h, uint16_t color);
static void redraw_game_screen(void);
static void draw_game_menu(void);
static void make_state_slot_path(uint8_t slot, char *out, size_t out_size);
static bool ppa_trans_done_cb(ppa_client_handle_t ppa_client, ppa_event_data_t *event_data, void *user_data);
static void audio_rate_changed(struct mAVStream *stream, unsigned rate);
static void audio_post_buffer(struct mAVStream *stream, struct mAudioBuffer *buffer);

static void ui_lock(void)
{
    if (s_ui_lock) {
        xSemaphoreTake(s_ui_lock, portMAX_DELAY);
    }
}

static void ui_unlock(void)
{
    if (s_ui_lock) {
        xSemaphoreGive(s_ui_lock);
    }
}

typedef struct {
    char ch;
    uint8_t rows[FONT_H];
} glyph_t;

static const glyph_t s_font[] = {
    { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c } },
    { '+', { 0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00 } },
    { '-', { 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00 } },
    { '_', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f } },
    { '/', { 0x01, 0x02, 0x04, 0x04, 0x08, 0x10, 0x00 } },
    { ':', { 0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00 } },
    { '0', { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e } },
    { '1', { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e } },
    { '2', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f } },
    { '3', { 0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e } },
    { '4', { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 } },
    { '5', { 0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e } },
    { '6', { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e } },
    { '7', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e } },
    { '9', { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c } },
    { 'A', { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e } },
    { 'C', { 0x0f, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0f } },
    { 'D', { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e } },
    { 'E', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f } },
    { 'F', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0f, 0x10, 0x10, 0x13, 0x11, 0x11, 0x0f } },
    { 'H', { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e } },
    { 'J', { 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0e } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } },
    { 'M', { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
    { 'O', { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
    { 'P', { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d } },
    { 'R', { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e } },
    { 'T', { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a } },
    { 'X', { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04 } },
    { 'Z', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f } },
};

static const uint8_t *glyph_rows(char ch)
{
    static const uint8_t missing[FONT_H] = { 0x1f, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1f };

    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }

    for (size_t i = 0; i < sizeof(s_font) / sizeof(s_font[0]); ++i) {
        if (s_font[i].ch == ch) {
            return s_font[i].rows;
        }
    }
    return missing;
}

static bool IRAM_ATTR ppa_trans_done_cb(ppa_client_handle_t ppa_client, ppa_event_data_t *event_data, void *user_data)
{
    (void)ppa_client;
    (void)event_data;
    (void)user_data;
    BaseType_t higher = pdFALSE;
    if (s_ppa_done_sem) {
        xSemaphoreGiveFromISR(s_ppa_done_sem, &higher);
    }
    return higher == pdTRUE;
}

static void flush_lcd_rect(int x, int y, int w, int h)
{
    if (!s_lcd_fb || !s_lcd_width || !s_lcd_height || w <= 0 || h <= 0) {
        return;
    }

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > s_lcd_width) {
        w = s_lcd_width - x;
    }
    if (y + h > s_lcd_height) {
        h = s_lcd_height - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    const size_t line_bytes = (size_t)s_lcd_width * sizeof(uint16_t);
    uintptr_t start = (uintptr_t)s_lcd_fb + (size_t)y * line_bytes + (size_t)x * sizeof(uint16_t);
    uintptr_t end = (uintptr_t)s_lcd_fb + (size_t)(y + h - 1) * line_bytes + (size_t)(x + w) * sizeof(uint16_t);
    uintptr_t aligned_start = start & ~(uintptr_t)(FRAMEBUFFER_ALIGN - 1);
    uintptr_t aligned_end = (end + FRAMEBUFFER_ALIGN - 1) & ~(uintptr_t)(FRAMEBUFFER_ALIGN - 1);

    esp_err_t ret = esp_cache_msync((void *)aligned_start, aligned_end - aligned_start,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LCD cache flush failed: %s", esp_err_to_name(ret));
    }
}

static void flush_lcd_fb(void)
{
    flush_lcd_rect(0, 0, s_lcd_width, s_lcd_height);
}

static void draw_char(int x, int y, char ch, uint16_t color, int scale)
{
    const uint8_t *rows = glyph_rows(ch);
    for (int row = 0; row < FONT_H; ++row) {
        for (int col = 0; col < FONT_W; ++col) {
            if (rows[row] & (1U << (FONT_W - 1 - col))) {
                fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(int x, int y, const char *text, uint16_t color, int scale, int max_px)
{
    int cursor = x;
    int step = (FONT_W + 1) * scale;
    while (*text && cursor + FONT_W * scale <= x + max_px) {
        draw_char(cursor, y, *text++, color, scale);
        cursor += step;
    }
}

static void *aligned_calloc_caps(size_t alignment, size_t count, size_t size, uint32_t caps)
{
    void *ptr = heap_caps_aligned_calloc(alignment, count, size, caps);
    if (ptr) {
        return ptr;
    }

    ptr = heap_caps_aligned_alloc(alignment, count * size, caps);
    if (ptr) {
        memset(ptr, 0, count * size);
    }
    return ptr;
}

static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool has_gba_ext(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) {
        return false;
    }

    const char *ext = &name[len - 4];
    return (ext[0] == '.') &&
           (ext[1] == 'g' || ext[1] == 'G') &&
           (ext[2] == 'b' || ext[2] == 'B') &&
           (ext[3] == 'a' || ext[3] == 'A');
}

static bool has_zip_ext(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) {
        return false;
    }

    const char *ext = &name[len - 4];
    return (ext[0] == '.') &&
           (ext[1] == 'z' || ext[1] == 'Z') &&
           (ext[2] == 'i' || ext[2] == 'I') &&
           (ext[3] == 'p' || ext[3] == 'P');
}

static size_t file_size_bytes(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return 0;
    }

    size_t size = 0;
    if (fseek(file, 0, SEEK_END) == 0) {
        long end = ftell(file);
        if (end > 0) {
            size = (size_t)end;
        }
    }
    fclose(file);
    return size;
}

static void format_rom_size(size_t bytes, char *out, size_t out_size)
{
    if (bytes >= 1024 * 1024) {
        unsigned tenths = (unsigned)((bytes * 10 + 512 * 1024) / (1024 * 1024));
        snprintf(out, out_size, "%u.%uM", tenths / 10, tenths % 10);
    } else if (bytes >= 1024) {
        snprintf(out, out_size, "%uK", (unsigned)((bytes + 512) / 1024));
    } else {
        snprintf(out, out_size, "%uB", (unsigned)bytes);
    }
}

static bool point_in_rect(uint16_t x, uint16_t y, int16_t rx, int16_t ry, int16_t rw, int16_t rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_lcd_fb || w <= 0 || h <= 0) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > s_lcd_width) {
        w = s_lcd_width - x;
    }
    if (y + h > s_lcd_height) {
        h = s_lcd_height - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    for (int row = 0; row < h; ++row) {
        uint16_t *dst = s_lcd_fb + (y + row) * s_lcd_width + x;
        for (int col = 0; col < w; ++col) {
            dst[col] = color;
        }
    }
}

static void draw_border(int x, int y, int w, int h, uint16_t color)
{
    fill_rect(x, y, w, 2, color);
    fill_rect(x, y + h - 2, w, 2, color);
    fill_rect(x, y, 2, h, color);
    fill_rect(x + w - 2, y, 2, h, color);
}

static void draw_soft_button(int x, int y, int w, int h, uint16_t fill, uint16_t border)
{
    fill_rect(x, y, w, h, fill);
    draw_border(x, y, w, h, border);
    fill_rect(x + 8, y + 6, w - 16, 2, RGB565(92, 152, 160));
}

static void draw_triangle_up(int cx, int cy, int half, uint16_t color)
{
    for (int row = 0; row <= half; ++row) {
        int width = row * 2 + 1;
        fill_rect(cx - row, cy - half + row, width, 2, color);
    }
}

static void draw_triangle_down(int cx, int cy, int half, uint16_t color)
{
    for (int row = 0; row <= half; ++row) {
        int width = (half - row) * 2 + 1;
        fill_rect(cx - half + row, cy + row, width, 2, color);
    }
}

static void draw_page_button(int x, int y, int w, int h, bool up, bool enabled)
{
    uint16_t fill = enabled ? RGB565(46, 62, 70) : RGB565(30, 36, 40);
    uint16_t border = enabled ? RGB565(184, 204, 208) : RGB565(78, 88, 92);
    uint16_t icon = enabled ? RGB565(242, 248, 238) : RGB565(112, 122, 126);

    fill_rect(x, y, w, h, fill);
    draw_border(x, y, w, h, border);
    if (up) {
        draw_triangle_up(x + w / 2, y + h / 2 + 4, 14, icon);
    } else {
        draw_triangle_down(x + w / 2, y + h / 2 - 14, 14, icon);
    }
}

static void draw_dpad_arrow(const pad_button_t *btn, uint16_t color)
{
    int cx = btn->x + btn->w / 2;
    int cy = btn->y + btn->h / 2;
    int half = btn->w / 4;

    if (btn->key == KEY_MASK(GBA_KEY_UP)) {
        for (int i = 0; i < half; ++i) {
            fill_rect(cx - i, cy - half + i, i * 2 + 1, 2, color);
        }
        fill_rect(cx - 4, cy, 8, 14, color);
    } else if (btn->key == KEY_MASK(GBA_KEY_DOWN)) {
        fill_rect(cx - 4, cy - 14, 8, 14, color);
        for (int i = 0; i < half; ++i) {
            fill_rect(cx - half + i, cy + i, (half - i) * 2 + 1, 2, color);
        }
    } else if (btn->key == KEY_MASK(GBA_KEY_LEFT)) {
        for (int i = 0; i < half; ++i) {
            fill_rect(cx - half + i, cy - i, 2, i * 2 + 1, color);
        }
        fill_rect(cx, cy - 4, 14, 8, color);
    } else if (btn->key == KEY_MASK(GBA_KEY_RIGHT)) {
        fill_rect(cx - 14, cy - 4, 14, 8, color);
        for (int i = 0; i < half; ++i) {
            fill_rect(cx + i, cy - half + i, 2, (half - i) * 2 + 1, color);
        }
    }
}

static void clear_screen(uint16_t color)
{
    fill_rect(0, 0, s_lcd_width, s_lcd_height, color);
}

static void draw_button(const pad_button_t *btn)
{
    bool pressed = (atomic_load(&s_keys) & btn->key) != 0;
    uint16_t color = pressed ? RGB565(230, 210, 92) : btn->color;
    uint16_t border = pressed ? RGB565(255, 248, 184) : RGB565(210, 220, 220);
    bool is_dpad = btn->key & (KEY_MASK(GBA_KEY_UP) | KEY_MASK(GBA_KEY_DOWN) |
                               KEY_MASK(GBA_KEY_LEFT) | KEY_MASK(GBA_KEY_RIGHT));

    if (is_dpad) {
        draw_soft_button(btn->x, btn->y, btn->w, btn->h, color, border);
        draw_dpad_arrow(btn, RGB565(246, 250, 240));
    } else if (btn->key & (KEY_MASK(GBA_KEY_A) | KEY_MASK(GBA_KEY_B))) {
        draw_soft_button(btn->x, btn->y, btn->w, btn->h, color, border);
        int scale = 4;
        int text_w = FONT_W * scale;
        int text_h = FONT_H * scale;
        draw_text(btn->x + (btn->w - text_w) / 2, btn->y + (btn->h - text_h) / 2,
                  btn->label, RGB565(250, 252, 238), scale, btn->w - 4);
    } else {
        draw_soft_button(btn->x, btn->y, btn->w, btn->h, color, border);
        int scale = strlen(btn->label) > 2 ? 2 : 3;
        int text_w = (int)strlen(btn->label) * FONT_W * scale +
                     ((int)strlen(btn->label) - 1) * scale;
        int text_h = FONT_H * scale;
        draw_text(btn->x + (btn->w - text_w) / 2, btn->y + (btn->h - text_h) / 2,
                  btn->label, RGB565(245, 248, 240), scale, btn->w - 4);
    }
}

static void draw_menu_button(const menu_button_t *btn)
{
    char state_label[20];
    const char *label = btn->label;
    uint16_t fill = btn->color;
    if (btn->cmd == EMU_CMD_TOGGLE_FAST) {
        bool fast = atomic_load(&s_fast_forward);
        snprintf(state_label, sizeof(state_label), "FAST %s", fast ? "ON" : "OFF");
        label = state_label;
        fill = fast ? RGB565(48, 126, 62) : RGB565(118, 88, 34);
    } else if (btn->cmd == EMU_CMD_TOGGLE_AUDIO) {
        bool audio = atomic_load(&s_audio_enabled);
        snprintf(state_label, sizeof(state_label), "AUDIO %s", audio ? "ON" : "OFF");
        label = state_label;
        fill = audio ? RGB565(48, 110, 132) : RGB565(52, 66, 76);
    } else if (btn->cmd == EMU_CMD_VOLUME_DOWN || btn->cmd == EMU_CMD_VOLUME_UP) {
        snprintf(state_label, sizeof(state_label), "%s %u", btn->label, (unsigned)atomic_load(&s_audio_volume));
        label = state_label;
    } else if (btn->cmd == EMU_CMD_CYCLE_FRAMESKIP) {
        snprintf(state_label, sizeof(state_label), "FSKIP %u", (unsigned)atomic_load(&s_frameskip));
        label = state_label;
    } else if (btn->cmd == EMU_CMD_TOGGLE_SKIP_BIOS) {
        bool skip = atomic_load(&s_skip_bios);
        snprintf(state_label, sizeof(state_label), "BIOS %s", skip ? "SKIP" : "RUN");
        label = state_label;
        fill = skip ? RGB565(82, 104, 70) : RGB565(116, 78, 58);
    } else if (btn->cmd == EMU_CMD_TOGGLE_AUDIO_SYNC) {
        bool sync = atomic_load(&s_audio_sync);
        snprintf(state_label, sizeof(state_label), "ASYNC %s", sync ? "ON" : "OFF");
        label = state_label;
        fill = sync ? RGB565(52, 104, 132) : RGB565(76, 86, 112);
    } else if (btn->cmd == EMU_CMD_TOGGLE_VIDEO_SYNC) {
        bool sync = atomic_load(&s_video_sync);
        snprintf(state_label, sizeof(state_label), "VSYNC %s", sync ? "ON" : "OFF");
        label = state_label;
        fill = sync ? RGB565(50, 118, 96) : RGB565(76, 96, 94);
    } else if (btn->cmd == EMU_CMD_TOGGLE_SHOW_FPS) {
        bool show = atomic_load(&s_show_fps);
        snprintf(state_label, sizeof(state_label), "FPS %s", show ? "ON" : "OFF");
        label = state_label;
        fill = show ? RGB565(70, 118, 62) : RGB565(82, 92, 74);
    } else if (btn->cmd == EMU_CMD_TOGGLE_SCALE_LINEAR) {
        bool linear = atomic_load(&s_scale_linear);
        snprintf(state_label, sizeof(state_label), "SCALE %s", linear ? "LIN" : "NN");
        label = state_label;
        fill = linear ? RGB565(46, 112, 126) : RGB565(84, 78, 108);
    }

    draw_soft_button(btn->x, btn->y, btn->w, btn->h, fill, RGB565(210, 220, 220));
    int scale = strlen(label) > 5 || btn->w < 130 ? 2 : 3;
    int text_w = (int)strlen(label) * FONT_W * scale +
                 ((int)strlen(label) - 1) * scale;
    int text_h = FONT_H * scale;
    draw_text(btn->x + (btn->w - text_w) / 2, btn->y + (btn->h - text_h) / 2,
              label, RGB565(245, 248, 240), scale, btn->w - 4);
}

static void flush_button_rect(const pad_button_t *btn)
{
    flush_lcd_rect(btn->x, btn->y, btn->w, btn->h);
}

static void draw_buttons(void)
{
    ui_lock();
    int min_x = s_lcd_width;
    int min_y = s_lcd_height;
    int max_x = 0;
    int max_y = 0;

    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        const pad_button_t *btn = &s_buttons[i];
        draw_button(btn);
        if (btn->x < min_x) {
            min_x = btn->x;
        }
        if (btn->y < min_y) {
            min_y = btn->y;
        }
        if (btn->x + btn->w > max_x) {
            max_x = btn->x + btn->w;
        }
        if (btn->y + btn->h > max_y) {
            max_y = btn->y + btn->h;
        }
    }
    flush_lcd_rect(min_x, min_y, max_x - min_x, max_y - min_y);
    ui_unlock();
}

static void draw_changed_buttons(uint32_t old_keys, uint32_t new_keys)
{
    uint32_t changed = old_keys ^ new_keys;
    if (!changed) {
        return;
    }

    ui_lock();
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        const pad_button_t *btn = &s_buttons[i];
        if (changed & btn->key) {
            draw_button(btn);
            flush_button_rect(btn);
        }
    }
    ui_unlock();
}

static void draw_rom_menu(size_t count)
{
    ui_lock();
    clear_screen(RGB565(12, 18, 24));
    fill_rect(40, 26, s_lcd_width - 80, 48, RGB565(32, 44, 54));
    draw_border(40, 26, s_lcd_width - 80, 48, RGB565(180, 198, 204));
    draw_text(58, 40, "SELECT ROM", RGB565(235, 240, 230), FONT_SCALE, s_lcd_width - 116);

    size_t offset = atomic_load(&s_rom_list_offset);
    if (offset >= count) {
        offset = 0;
        atomic_store(&s_rom_list_offset, 0);
    }
    size_t remaining = count > offset ? count - offset : 0;
    size_t visible = remaining < ROM_VISIBLE_MAX ? remaining : ROM_VISIBLE_MAX;
    for (size_t i = 0; i < visible; ++i) {
        int y = 96 + (int)i * 44;
        size_t rom_index = offset + i;
        char size_text[12];
        format_rom_size(s_rom_sizes[rom_index], size_text, sizeof(size_text));
        fill_rect(80, y, s_lcd_width - 160, 34, RGB565(28, 68 + i * 10, 82));
        draw_border(80, y, s_lcd_width - 160, 34, RGB565(190, 210, 210));
        draw_text(98, y + 8, path_basename(s_rom_paths[rom_index]), RGB565(242, 248, 236), 2, s_lcd_width - 286);
        fill_rect(s_lcd_width - 174, y + 4, 84, 26, RGB565(22, 42, 50));
        draw_text(s_lcd_width - 164, y + 10, size_text, RGB565(238, 230, 168), 2, 72);
    }

    if (count > ROM_VISIBLE_MAX) {
        draw_page_button(686, 96, 58, 58, true, offset > 0);
        draw_page_button(686, 390, 58, 58, false, offset + visible < count);
        char page[24];
        snprintf(page, sizeof(page), "%u/%u",
                 (unsigned)(offset / ROM_VISIBLE_MAX + 1),
                 (unsigned)((count + ROM_VISIBLE_MAX - 1) / ROM_VISIBLE_MAX));
        draw_text(586, 42, page, RGB565(230, 236, 220), 2, 120);
    }

    if (count == 0) {
        fill_rect(120, 180, s_lcd_width - 240, 60, RGB565(96, 32, 32));
        draw_text(148, 202, "ROM NOT FOUND", RGB565(255, 235, 220), FONT_SCALE, s_lcd_width - 296);
    }
    flush_lcd_fb();
    ui_unlock();
}

static void draw_status_screen(const char *title, const char *line1, const char *line2)
{
    ui_lock();
    clear_screen(RGB565(12, 18, 24));
    fill_rect(40, 26, s_lcd_width - 80, 48, RGB565(32, 44, 54));
    draw_border(40, 26, s_lcd_width - 80, 48, RGB565(180, 198, 204));
    draw_text(58, 40, title, RGB565(235, 240, 230), FONT_SCALE, s_lcd_width - 116);
    if (line1 && line1[0]) {
        draw_text(82, 170, line1, RGB565(242, 248, 236), 2, s_lcd_width - 164);
    }
    if (line2 && line2[0]) {
        draw_text(82, 216, line2, RGB565(238, 230, 168), 2, s_lcd_width - 164);
    }
    flush_lcd_fb();
    ui_unlock();
}

static void draw_zip_progress(const char *name, uint32_t done, uint32_t total)
{
    char line1[128];
    char line2[64];
    unsigned pct = total ? (unsigned)(((uint64_t)done * 100) / total) : 0;
    char done_text[12];
    char total_text[12];

    format_rom_size(done, done_text, sizeof(done_text));
    format_rom_size(total, total_text, sizeof(total_text));
    snprintf(line1, sizeof(line1), "%s", path_basename(name));
    snprintf(line2, sizeof(line2), "%u%%  %s/%s", pct, done_text, total_text);
    draw_status_screen("UNZIP ROM", line1, line2);
}

static bool state_slot_exists(uint8_t slot)
{
    char path[STATE_PATH_MAX];
    make_state_slot_path(slot, path, sizeof(path));
    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    fclose(file);
    return true;
}

static void draw_state_slot_panel(void)
{
    uint8_t slot = atomic_load(&s_state_slot);
    char text[48];
    snprintf(text, sizeof(text), "S%u %s", (unsigned)(slot + 1),
             state_slot_exists(slot) ? "USED" : "EMPTY");

    draw_text(208, 128, "STATE", RGB565(176, 196, 198), 2, 120);
    fill_rect(208, 202, 118, 36, RGB565(18, 28, 34));
    draw_border(208, 202, 118, 36, RGB565(176, 196, 198));
    draw_soft_button(340, 202, 118, 36, RGB565(54, 64, 72), RGB565(190, 210, 210));
    draw_soft_button(472, 202, 118, 36, RGB565(54, 64, 72), RGB565(190, 210, 210));
    draw_text(218, 212, text, RGB565(238, 230, 168), 2, 100);
    draw_text(366, 212, "SLOT-", RGB565(245, 248, 240), 2, 66);
    draw_text(498, 212, "SLOT+", RGB565(245, 248, 240), 2, 66);
}

static void draw_game_menu(void)
{
    ui_lock();
    fill_rect(188, 52, 424, 386, RGB565(14, 20, 26));
    draw_border(188, 52, 424, 386, RGB565(190, 206, 210));
    const char *title = "GAME MENU";
    int title_w = (int)strlen(title) * FONT_W * FONT_SCALE +
                  ((int)strlen(title) - 1) * FONT_SCALE;
    draw_text(188 + (424 - title_w) / 2, 62, title, RGB565(238, 244, 232), FONT_SCALE, 252);

    for (size_t i = 0; i < sizeof(s_menu_buttons) / sizeof(s_menu_buttons[0]); ++i) {
        draw_menu_button(&s_menu_buttons[i]);
    }

    draw_state_slot_panel();
    draw_text(208, 250, "OPTIONS", RGB565(176, 196, 198), 2, 140);
    flush_lcd_rect(188, 52, 424, 386);
    ui_unlock();
}

static void draw_gba_frame(void)
{
    if (!s_lcd_fb || !s_render_framebuffer) {
        return;
    }
    if (atomic_load(&s_ui_mode) == UI_MODE_GAME_MENU) {
        return;
    }

    ui_lock();
    const int scale = GBA_SCALE;
    const int out_w = GBA_WIDTH * scale;
    const int out_h = GBA_HEIGHT * scale;
    const int dst_x = (s_lcd_width - out_w) / 2;
    const int dst_y = (s_lcd_height - out_h) / 2;
    s_game_rect = (rect_t) { dst_x, dst_y, out_w, out_h };

    if (USE_PPA_GAME_SCALE && atomic_load(&s_scale_linear) && s_ppa_srm) {
        // PPA driver does its own input C2M and output M2C cache sync, so the
        // explicit flushes here would just walk thousands of cache lines for
        // no benefit. CPU does not read the LCD framebuffer game area, so no
        // post-PPA flush is needed either.
        ppa_srm_oper_config_t ppa_cfg = {
            .in = {
                .buffer = s_render_framebuffer,
                .pic_w = GBA_WIDTH,
                .pic_h = GBA_HEIGHT,
                .block_w = GBA_WIDTH,
                .block_h = GBA_HEIGHT,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer = s_lcd_fb,
                .buffer_size = (uint32_t)((size_t)s_lcd_width * s_lcd_height * sizeof(uint16_t)),
                .pic_w = s_lcd_width,
                .pic_h = s_lcd_height,
                .block_offset_x = dst_x,
                .block_offset_y = dst_y,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = scale,
            .scale_y = scale,
            .mode = s_ppa_done_sem ? PPA_TRANS_MODE_NON_BLOCKING : PPA_TRANS_MODE_BLOCKING,
        };
        esp_err_t ppa_ret = ppa_do_scale_rotate_mirror(s_ppa_srm, &ppa_cfg);
        if (ppa_ret == ESP_OK) {
            if (ppa_cfg.mode == PPA_TRANS_MODE_NON_BLOCKING) {
                xSemaphoreTake(s_ppa_done_sem, portMAX_DELAY);
            }
            ui_unlock();
            return;
        }
    }

    if (scale == 2 && (dst_x & 1) == 0) {
        for (int y = 0; y < GBA_HEIGHT; ++y) {
            const uint16_t *src = (const uint16_t *)&s_render_framebuffer[y * GBA_WIDTH];
            uint32_t *dst0 = (uint32_t *)(s_lcd_fb + (dst_y + y * 2) * s_lcd_width + dst_x);
            uint32_t *dst1 = (uint32_t *)(s_lcd_fb + (dst_y + y * 2 + 1) * s_lcd_width + dst_x);

            for (int x = 0; x < GBA_WIDTH; ++x) {
                uint32_t pair = (uint32_t)src[x] | ((uint32_t)src[x] << 16);
                dst0[x] = pair;
                dst1[x] = pair;
            }
        }
    } else {
        for (int y = 0; y < GBA_HEIGHT; ++y) {
            const uint16_t *src = (const uint16_t *)&s_render_framebuffer[y * GBA_WIDTH];
            for (int sy = 0; sy < scale; ++sy) {
                uint16_t *dst = s_lcd_fb + (dst_y + y * scale + sy) * s_lcd_width + dst_x;
                for (int x = 0; x < GBA_WIDTH; ++x) {
                    uint16_t px = src[x];
                    for (int sx = 0; sx < scale; ++sx) {
                        *dst++ = px;
                    }
                }
            }
        }
    }
    flush_lcd_rect(s_game_rect.x, s_game_rect.y, s_game_rect.w, s_game_rect.h);
    ui_unlock();
}

static void redraw_game_screen(void)
{
    ui_lock();
    clear_screen(RGB565(8, 12, 16));
    flush_lcd_fb();
    ui_unlock();
    draw_gba_frame();
    draw_buttons();
}

static void request_render_frame(void)
{
    if (!s_render_task_handle || !s_active_framebuffer || !s_framebuffers[0] || !s_framebuffers[1]) {
        return;
    }
    if (xSemaphoreTake(s_frame_lock, 0) == pdTRUE) {
        s_render_framebuffer = s_active_framebuffer;
        s_active_framebuffer = (s_active_framebuffer == s_framebuffers[0]) ? s_framebuffers[1] : s_framebuffers[0];
        xSemaphoreGive(s_frame_lock);
        xTaskNotifyGive(s_render_task_handle);
    }
}

static void render_task(void *arg)
{
    (void)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (xSemaphoreTake(s_frame_lock, portMAX_DELAY) == pdTRUE) {
            draw_gba_frame();
            xSemaphoreGive(s_frame_lock);
        }
    }
}

static void draw_fps(float fps)
{
    if (!atomic_load(&s_show_fps) || atomic_load(&s_ui_mode) == UI_MODE_GAME_MENU) {
        return;
    }

    ui_lock();
    char text[16];
    snprintf(text, sizeof(text), "FPS:%2.1f", (double)fps);
    fill_rect(650, 72, 132, 30, RGB565(8, 12, 16));
    draw_text(656, 78, text, RGB565(240, 248, 210), 2, 120);
    flush_lcd_rect(650, 72, 132, 30);
    ui_unlock();
}

static void update_status(const char *text)
{
    ESP_LOGI(TAG, "%s", text);
    draw_status_screen("STATUS", text, "");
}

static void update_status_detail(const char *title, const char *line1, const char *line2)
{
    ESP_LOGI(TAG, "%s: %s %s", title, line1 ? line1 : "", line2 ? line2 : "");
    draw_status_screen(title, line1, line2);
}

static void make_state_path(const char *rom_path, char *out, size_t out_size)
{
    const char *name = path_basename(rom_path);
    int written = snprintf(out, out_size, ROM_DIR "/%s.ss", name);
    if (written < 0 || (size_t)written >= out_size) {
        strlcpy(out, ROM_DIR "/state.ss", out_size);
    }
}

static void make_save_path(const char *rom_path, char *out, size_t out_size)
{
    const char *name = path_basename(rom_path);
    int written = snprintf(out, out_size, ROM_DIR "/%s.sav", name);
    if (written < 0 || (size_t)written >= out_size) {
        strlcpy(out, ROM_DIR "/save.sav", out_size);
    }
}

// RTC source: provides a monotonic wall-clock to mGBA. Games like Pokemon
// Emerald (cart RTC) read this to advance berry growth, tides, etc. We use
// the system clock if it's been set (NTP / save state), otherwise a fixed
// epoch (2025-01-01) plus board uptime so time still advances monotonically.
static time_t rtc_unix_time(struct mRTCSource *rtc)
{
    (void)rtc;
    time_t t = time(NULL);
    if (t < 1577836800) { // pre-2020 => system clock not set
        t = (time_t)1735689600 + (time_t)(esp_timer_get_time() / 1000000);
    }
    return t;
}

static void rtc_sample(struct mRTCSource *rtc)
{
    (void)rtc;
}

static struct mRTCSource s_rtc_source = {
    .sample = rtc_sample,
    .unixTime = rtc_unix_time,
    .serialize = NULL,
    .deserialize = NULL,
};

static void make_state_slot_path(uint8_t slot, char *out, size_t out_size)
{
    int written = snprintf(out, out_size, "%s%u", s_state_path, (unsigned)slot);
    if (written < 0 || (size_t)written >= out_size) {
        strlcpy(out, ROM_DIR "/state.ss0", out_size);
    }
}

static bool save_state_file(struct mCore *core, const char *path)
{
    size_t state_size = core->stateSize(core);
    void *state = heap_caps_malloc(state_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!state) {
        state = malloc(state_size);
    }
    if (!state) {
        ESP_LOGE(TAG, "Failed to allocate state buffer: %u", (unsigned)state_size);
        return false;
    }

    bool ok = core->saveState(core, state);
    if (ok) {
        FILE *file = fopen(path, "wb");
        if (!file) {
            ESP_LOGE(TAG, "Failed to open state for write: %s", path);
            ok = false;
        } else {
            ok = fwrite(state, 1, state_size, file) == state_size;
            fclose(file);
        }
    }

    free(state);
    ESP_LOGI(TAG, "%s state: %s", ok ? "Saved" : "Failed to save", path);
    return ok;
}

static bool load_state_file(struct mCore *core, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open state for read: %s", path);
        return false;
    }

    size_t state_size = core->stateSize(core);
    void *state = heap_caps_malloc(state_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!state) {
        state = malloc(state_size);
    }
    if (!state) {
        fclose(file);
        ESP_LOGE(TAG, "Failed to allocate state buffer: %u", (unsigned)state_size);
        return false;
    }

    bool ok = fread(state, 1, state_size, file) == state_size;
    fclose(file);
    if (ok) {
        ok = core->loadState(core, state);
    }

    free(state);
    ESP_LOGI(TAG, "%s state: %s", ok ? "Loaded" : "Failed to load", path);
    return ok;
}

static bool delete_state_file(const char *path)
{
    bool ok = remove(path) == 0;
    ESP_LOGI(TAG, "%s state: %s", ok ? "Deleted" : "Failed to delete", path);
    return ok;
}

static void clear_audio_queue(void)
{
    if (!s_audio_ringbuf) {
        return;
    }

    size_t bytes = 0;
    void *item = NULL;
    while ((item = xRingbufferReceive(s_audio_ringbuf, &bytes, 0)) != NULL) {
        vRingbufferReturnItem(s_audio_ringbuf, item);
    }
}

static void wake_audio_task(void)
{
    if (s_audio_ringbuf && xRingbufferGetCurFreeSize(s_audio_ringbuf) >= sizeof(s_audio_silence_chunk)) {
        xRingbufferSend(s_audio_ringbuf, s_audio_silence_chunk, sizeof(s_audio_silence_chunk), 0);
    }
}

static int mgba_volume_from_percent(uint8_t volume)
{
    return (int)volume * 0x100 / 100;
}

static esp_err_t audio_codec_set_enabled(bool enabled)
{
    if (!s_audio_codec) {
        return ESP_ERR_INVALID_STATE;
    }

    atomic_store(&s_audio_codec_wanted, enabled);
    if (!enabled) {
        clear_audio_queue();
    }
    wake_audio_task();
    return ESP_OK;
}

static void audio_codec_set_paused(bool paused)
{
    bool old = atomic_exchange(&s_audio_paused, paused);
    if (old == paused) {
        return;
    }
    if (paused) {
        clear_audio_queue();
    }
    wake_audio_task();
}

static void apply_audio_volume(struct mCore *core)
{
    uint8_t volume = atomic_load(&s_audio_volume);
    if (core) {
        core->opts.volume = (atomic_load(&s_audio_enabled) && !atomic_load(&s_audio_paused)) ?
                mgba_volume_from_percent(volume) : 0;
        core->reloadConfigOption(core, NULL, NULL);
    }
}

static void apply_runtime_options(struct mCore *core)
{
    if (!core) {
        return;
    }

    core->opts.frameskip = atomic_load(&s_frameskip);
    core->opts.skipBios = atomic_load(&s_skip_bios);
    core->opts.audioSync = atomic_load(&s_audio_sync);
    core->opts.videoSync = atomic_load(&s_video_sync);
    core->reloadConfigOption(core, NULL, NULL);
}

static bool parse_bool_setting(const char *value, bool *out)
{
    if (!value || !out) {
        return false;
    }
    if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 || strcasecmp(value, "on") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 || strcasecmp(value, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static void load_settings_from_sd(void)
{
    FILE *file = fopen(SETTINGS_PATH, "r");
    if (!file) {
        ESP_LOGI(TAG, "Settings file not found, using defaults: " SETTINGS_PATH);
        return;
    }

    char line[64];
    while (fgets(line, sizeof(line), file)) {
        char *key = line;
        while (*key == ' ' || *key == '\t') {
            ++key;
        }
        if (*key == '#' || *key == '\n' || *key == '\0') {
            continue;
        }

        char *eq = strchr(key, '=');
        if (!eq) {
            continue;
        }
        *eq++ = '\0';
        char *value = eq;
        char *end = value + strlen(value);
        while (end > value && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        end = key + strlen(key);
        while (end > key && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }

        if (strcmp(key, "audio_enabled") == 0) {
            bool enabled;
            if (parse_bool_setting(value, &enabled)) {
                atomic_store(&s_audio_enabled, enabled);
            }
        } else if (strcmp(key, "fast_forward") == 0) {
            bool fast;
            if (parse_bool_setting(value, &fast)) {
                atomic_store(&s_fast_forward, fast);
            }
        } else if (strcmp(key, "audio_volume") == 0) {
            unsigned volume = (unsigned)strtoul(value, NULL, 10);
            if (volume <= 100) {
                atomic_store(&s_audio_volume, (uint8_t)volume);
            }
        } else if (strcmp(key, "frameskip") == 0) {
            unsigned frameskip = (unsigned)strtoul(value, NULL, 10);
            if (frameskip <= MAX_FRAMESKIP) {
                atomic_store(&s_frameskip, (uint8_t)frameskip);
            }
        } else if (strcmp(key, "skip_bios") == 0) {
            bool skip;
            if (parse_bool_setting(value, &skip)) {
                atomic_store(&s_skip_bios, skip);
            }
        } else if (strcmp(key, "audio_sync") == 0) {
            bool sync;
            if (parse_bool_setting(value, &sync)) {
                atomic_store(&s_audio_sync, sync);
            }
        } else if (strcmp(key, "video_sync") == 0) {
            bool sync;
            if (parse_bool_setting(value, &sync)) {
                atomic_store(&s_video_sync, sync);
            }
        } else if (strcmp(key, "show_fps") == 0) {
            bool show;
            if (parse_bool_setting(value, &show)) {
                atomic_store(&s_show_fps, show);
            }
        } else if (strcmp(key, "scale_linear") == 0) {
            bool linear;
            if (parse_bool_setting(value, &linear)) {
                atomic_store(&s_scale_linear, linear);
            }
        }
    }

    fclose(file);
    ESP_LOGI(TAG, "Settings loaded from " SETTINGS_PATH);
}

static void save_settings_to_sd(void)
{
    FILE *file = fopen(SETTINGS_PATH, "w");
    if (!file) {
        ESP_LOGW(TAG, "Failed to save settings: " SETTINGS_PATH);
        return;
    }

    fprintf(file, "audio_enabled=%u\n", atomic_load(&s_audio_enabled) ? 1U : 0U);
    fprintf(file, "fast_forward=%u\n", atomic_load(&s_fast_forward) ? 1U : 0U);
    fprintf(file, "audio_volume=%u\n", (unsigned)atomic_load(&s_audio_volume));
    fprintf(file, "frameskip=%u\n", (unsigned)atomic_load(&s_frameskip));
    fprintf(file, "skip_bios=%u\n", atomic_load(&s_skip_bios) ? 1U : 0U);
    fprintf(file, "audio_sync=%u\n", atomic_load(&s_audio_sync) ? 1U : 0U);
    fprintf(file, "video_sync=%u\n", atomic_load(&s_video_sync) ? 1U : 0U);
    fprintf(file, "show_fps=%u\n", atomic_load(&s_show_fps) ? 1U : 0U);
    fprintf(file, "scale_linear=%u\n", atomic_load(&s_scale_linear) ? 1U : 0U);
    fclose(file);
}

static void audio_rate_changed(struct mAVStream *stream, unsigned rate)
{
    (void)stream;

    if (!rate) {
        rate = AUDIO_CORE_SAMPLE_RATE;
    }

    uint32_t old_rate = atomic_exchange(&s_audio_source_rate, rate);
    if (old_rate != rate) {
        ESP_LOGI(TAG, "Audio source rate changed: %u -> %u Hz", (unsigned)old_rate, rate);
        clear_audio_queue();
    }
}

static void audio_post_buffer(struct mAVStream *stream, struct mAudioBuffer *buffer)
{
    (void)stream;

    if (!atomic_load(&s_audio_enabled) || !s_audio_ringbuf || !buffer) {
        return;
    }

    int16_t samples[AUDIO_CHUNK_SAMPLES * 2];
    const size_t chunk_bytes = AUDIO_CHUNK_SAMPLES * 2 * sizeof(int16_t);
    for (size_t chunk = 0;
         chunk < AUDIO_MAX_POST_CHUNKS && mAudioBufferAvailable(buffer) >= AUDIO_CHUNK_SAMPLES;
         ++chunk) {
        size_t read = mAudioBufferRead(buffer, samples, AUDIO_CHUNK_SAMPLES);
        if (read == 0) {
            break;
        }

        size_t bytes = read * 2 * sizeof(int16_t);
        if (xRingbufferGetCurFreeSize(s_audio_ringbuf) < chunk_bytes ||
                xRingbufferSend(s_audio_ringbuf, samples, bytes, 0) != pdTRUE) {
            atomic_fetch_add(&s_audio_drop_count, 1);
            continue;
        }
        atomic_fetch_add(&s_audio_post_count, 1);
    }
}

static void audio_resampler_reset(uint32_t source_rate)
{
    if (!source_rate) {
        source_rate = AUDIO_CORE_SAMPLE_RATE;
    }
    mAudioBufferClear(&s_audio_resample_source);
    mAudioBufferClear(&s_audio_resample_dest);
    mAudioResamplerSetSource(&s_audio_resampler, &s_audio_resample_source, source_rate, true);
    s_audio_resampler.timestamp = 0.0;
}

static bool audio_output_requested(void)
{
    return atomic_load(&s_audio_codec_wanted) &&
           atomic_load(&s_audio_enabled) &&
           !atomic_load(&s_audio_paused);
}

static bool audio_task_open_codec(void)
{
    if (!s_audio_codec) {
        return false;
    }
    if (s_audio_codec_opened) {
        uint8_t volume = atomic_load(&s_audio_volume);
        if (volume != s_audio_codec_volume_applied) {
            esp_codec_dev_set_out_vol(s_audio_codec, volume);
            s_audio_codec_volume_applied = volume;
        }
        if (!s_audio_pa_enabled) {
            gpio_set_level(AUDIO_PA_GPIO, 1);
            s_audio_pa_enabled = true;
        }
        return true;
    }

    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0,
        .sample_rate = AUDIO_CODEC_SAMPLE_RATE,
        .mclk_multiple = 256,
    };
    int ret = esp_codec_dev_open(s_audio_codec, &sample_info);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "open audio_dac failed: %d", ret);
        return false;
    }
    s_audio_codec_opened = true;
    s_audio_codec_volume_applied = atomic_load(&s_audio_volume);
    esp_codec_dev_set_out_vol(s_audio_codec, s_audio_codec_volume_applied);
    esp_codec_dev_set_out_mute(s_audio_codec, false);
    gpio_set_level(AUDIO_PA_GPIO, 1);
    s_audio_pa_enabled = true;
    return true;
}

static void audio_task_close_codec(void)
{
    if (s_audio_codec && s_audio_codec_opened) {
        memset(s_audio_write_buf, 0, sizeof(s_audio_write_buf));
        for (int i = 0; i < 2; ++i) {
            esp_codec_dev_write(s_audio_codec, s_audio_write_buf, sizeof(s_audio_write_buf));
        }
        esp_codec_dev_set_out_mute(s_audio_codec, true);
    }
    if (s_audio_pa_enabled) {
        gpio_set_level(AUDIO_PA_GPIO, 0);
        s_audio_pa_enabled = false;
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    uint32_t current_rate = atomic_load(&s_audio_source_rate);
    size_t pending_frames = 0;
    audio_resampler_reset(current_rate);

    while (true) {
        size_t bytes = 0;
        void *item = xRingbufferReceive(s_audio_ringbuf, &bytes, portMAX_DELAY);
        if (!item) {
            continue;
        }

        uint32_t source_rate = atomic_load(&s_audio_source_rate);
        if (!source_rate) {
            source_rate = AUDIO_CORE_SAMPLE_RATE;
        }
        if (source_rate != current_rate) {
            current_rate = source_rate;
            audio_resampler_reset(current_rate);
            pending_frames = 0;
        }

        if (!audio_output_requested()) {
            audio_task_close_codec();
            audio_resampler_reset(current_rate);
            pending_frames = 0;
            vRingbufferReturnItem(s_audio_ringbuf, item);
            continue;
        }

        if (audio_task_open_codec()) {
            const int16_t *input = (const int16_t *)item;
            size_t input_frames = bytes / (2 * sizeof(int16_t));
            size_t written = mAudioBufferWrite(&s_audio_resample_source, input, input_frames);
            if (written < input_frames) {
                atomic_fetch_add(&s_audio_drop_count, 1);
            }
            mAudioResamplerProcess(&s_audio_resampler);

            while (mAudioBufferAvailable(&s_audio_resample_dest) > 0) {
                size_t room = AUDIO_CODEC_WRITE_SAMPLES - pending_frames;
                size_t read = mAudioBufferRead(&s_audio_resample_dest,
                                               &s_audio_write_buf[pending_frames * 2],
                                               room);
                if (!read) {
                    break;
                }
                pending_frames += read;

                if (pending_frames == AUDIO_CODEC_WRITE_SAMPLES) {
                    int ret = esp_codec_dev_write(s_audio_codec, s_audio_write_buf,
                                                  sizeof(s_audio_write_buf));
                    if (ret != ESP_CODEC_DEV_OK) {
                        ESP_LOGW(TAG, "Audio write failed: %d", ret);
                        pending_frames = 0;
                        break;
                    }
                    pending_frames = 0;
                    atomic_fetch_add(&s_audio_write_count, 1);
                }
            }
        } else {
            audio_resampler_reset(current_rate);
            pending_frames = 0;
        }
        vRingbufferReturnItem(s_audio_ringbuf, item);
    }
}

static size_t scan_roms(void)
{
    DIR *dir = opendir(ROM_DIR);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open ROM directory: " ROM_DIR);
        return 0;
    }

    s_rom_count = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) && s_rom_count < ROM_LIST_MAX) {
        if (!has_gba_ext(entry->d_name) && !has_zip_ext(entry->d_name)) {
            continue;
        }

        int written = snprintf(s_rom_paths[s_rom_count], ROM_PATH_MAX, ROM_DIR "/%s", entry->d_name);
        if (written > 0 && written < ROM_PATH_MAX) {
            s_rom_sizes[s_rom_count] = file_size_bytes(s_rom_paths[s_rom_count]);
            ESP_LOGI(TAG, "ROM[%u]: %s (%u bytes)",
                     (unsigned)s_rom_count, s_rom_paths[s_rom_count], (unsigned)s_rom_sizes[s_rom_count]);
            ++s_rom_count;
        }
    }

    closedir(dir);
    ESP_LOGI(TAG, "Found %u ROM file(s)", (unsigned)s_rom_count);
    return s_rom_count;
}

static void log_heap_state(const char *label, size_t need)
{
    ESP_LOGI(TAG,
             "%s: need=%u, psram_free=%u, psram_largest=%u, default_largest=%u, internal_free=%u, internal_largest=%u",
             label,
             (unsigned)need,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static void log_memory_usage(void)
{
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t psram_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    size_t sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t sram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t sram_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG,
             "MEM psram used=%u/%u free=%u largest=%u min_free=%u | sram used=%u/%u free=%u largest=%u min_free=%u",
             (unsigned)(psram_total - psram_free),
             (unsigned)psram_total,
             (unsigned)psram_free,
             (unsigned)psram_largest,
             (unsigned)psram_min_free,
             (unsigned)(sram_total - sram_free),
             (unsigned)sram_total,
             (unsigned)sram_free,
             (unsigned)sram_largest,
             (unsigned)sram_min_free);
    ESP_LOGI(TAG,
             "Audio enabled=%u codec=%u src=%uHz in=%u out=%u queued=%u written=%u dropped=%u",
             atomic_load(&s_audio_enabled) ? 1U : 0U,
             (s_audio_codec && s_audio_codec_opened) ? 1U : 0U,
             (unsigned)atomic_load(&s_audio_source_rate),
             (unsigned)mAudioBufferAvailable(&s_audio_resample_source),
             (unsigned)mAudioBufferAvailable(&s_audio_resample_dest),
             (unsigned)atomic_exchange(&s_audio_post_count, 0),
             (unsigned)atomic_exchange(&s_audio_write_count, 0),
             (unsigned)atomic_exchange(&s_audio_drop_count, 0));
}

static void mem_monitor_task(void *arg)
{
    (void)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(MEM_MONITOR_INTERVAL_MS));
        log_heap_state("Periodic heap", 0);
        log_memory_usage();
    }
}

static esp_err_t init_board_display_touch(void)
{
    ESP_LOGI(TAG, "Initializing esp_board_manager for esp32_s31_korvo1");
    ESP_RETURN_ON_ERROR(esp_board_manager_init(), TAG, "board manager init");

    dev_display_lcd_handles_t *lcd = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle("display_lcd", (void **)&lcd), TAG, "get display_lcd");

    dev_display_lcd_config_t *lcd_cfg = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_config("display_lcd", (void **)&lcd_cfg), TAG, "get LCD config");
    s_lcd_width = lcd_cfg->lcd_width;
    s_lcd_height = lcd_cfg->lcd_height;

    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(lcd->panel_handle, 1, (void **)&s_lcd_fb),
                        TAG, "get RGB LCD framebuffer");
    ESP_RETURN_ON_FALSE(s_lcd_fb, ESP_ERR_INVALID_STATE, TAG, "RGB LCD framebuffer is NULL");

    ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    esp_err_t ppa_ret = ppa_register_client(&ppa_cfg, &s_ppa_srm);
    if (ppa_ret != ESP_OK) {
        ESP_LOGW(TAG, "PPA SRM unavailable, CPU scaler will be used: %s", esp_err_to_name(ppa_ret));
        s_ppa_srm = NULL;
    } else {
        ppa_event_callbacks_t cbs = { .on_trans_done = ppa_trans_done_cb };
        esp_err_t cb_ret = ppa_client_register_event_callbacks(s_ppa_srm, &cbs);
        if (cb_ret != ESP_OK) {
            ESP_LOGW(TAG, "PPA callback register failed: %s", esp_err_to_name(cb_ret));
        }
    }

    if (esp_board_manager_check_name("lcd_touch")) {
        dev_lcd_touch_handles_t *touch = NULL;
        ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle("lcd_touch", (void **)&touch), TAG, "get lcd_touch");
        s_touch_handle = touch->touch_handle;
    }

    clear_screen(RGB565(12, 18, 24));
    flush_lcd_fb();
    return ESP_OK;
}

static esp_err_t init_board_audio(void)
{
    dev_audio_codec_handles_t *audio = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle("audio_dac", (void **)&audio),
                        TAG, "get audio_dac");
    ESP_RETURN_ON_FALSE(audio && audio->codec_dev, ESP_ERR_INVALID_STATE, TAG, "audio_dac codec handle is NULL");

    s_audio_codec = audio->codec_dev;
    s_audio_codec_opened = false;
    s_audio_pa_enabled = false;
    s_audio_codec_volume_applied = 0xff;
    gpio_reset_pin(AUDIO_PA_GPIO);
    gpio_set_direction(AUDIO_PA_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(AUDIO_PA_GPIO, 0);
    esp_codec_set_disable_when_closed(s_audio_codec, false);
    ESP_LOGI(TAG, "Audio codec ready: core %u Hz -> codec %u Hz, stereo, 16-bit",
             (unsigned)atomic_load(&s_audio_source_rate), (unsigned)AUDIO_CODEC_SAMPLE_RATE);
    return ESP_OK;
}

static esp_err_t mount_sdcard(void)
{
    if (!esp_board_manager_check_name(ESP_BOARD_DEVICE_NAME_FS_SDCARD)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    gpio_set_level(SD_POWER_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_FS_SDCARD);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card at /sdcard: %s", esp_err_to_name(ret));
    }
    return ret;
}

#define ROM_CACHE_BYTES_DEFAULT (12u * 1024u * 1024u)

typedef enum {
    ROM_FORMAT_GBA,
    ROM_FORMAT_ZIP,
    ROM_FORMAT_GZIP,
    ROM_FORMAT_UNKNOWN,
} rom_format_t;

static rom_format_t detect_rom_format(const char *path, uint8_t *out_head)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return ROM_FORMAT_UNKNOWN;
    }
    uint8_t head[8] = {0};
    size_t got = fread(head, 1, sizeof(head), fp);
    fclose(fp);
    if (out_head) {
        memcpy(out_head, head, sizeof(head));
    }
    if (got < 4) {
        return ROM_FORMAT_UNKNOWN;
    }
    if (head[3] == 0xEA) {
        return ROM_FORMAT_GBA;
    }
    if (head[0] == 0x50 && head[1] == 0x4B && (head[2] == 0x03 || head[2] == 0x05 || head[2] == 0x07)) {
        return ROM_FORMAT_ZIP;
    }
    if (head[0] == 0x1F && head[1] == 0x8B && head[2] == 0x08) {
        return ROM_FORMAT_GZIP;
    }
    return ROM_FORMAT_UNKNOWN;
}

// Allocate a PSRAM buffer sized up to the next power of two >= raw_size.
// Returns the buffer (filled by the caller from `raw_size` bytes onward with
// the actual ROM data; the padding is set to 0xFF) and writes the final
// allocation size into *alloc_out. NULL on failure.
static void *alloc_rom_buffer(size_t raw_size, size_t *alloc_out)
{
    size_t alloc_size = raw_size;
    if (alloc_size & (alloc_size - 1)) {
        size_t pow2 = 1;
        while (pow2 < alloc_size) {
            pow2 <<= 1;
        }
        alloc_size = pow2;
    }
    void *data = heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        data = malloc(alloc_size);
    }
    if (data && alloc_size > raw_size) {
        memset((uint8_t *)data + raw_size, 0xFF, alloc_size - raw_size);
    }
    *alloc_out = alloc_size;
    return data;
}

// Minimal streaming ZIP parser: walks local file headers, finds the first
// *.gba entry, and decompresses it (deflate or store) to an SD-card .gba file.
// ESP-ROM ships a stripped miniz with archive APIs disabled, so we do the
// container parsing ourselves and only use the raw tinfl deflate decoder.
//
// We deliberately avoid full ZIP64 / encrypted ZIP support — those are not
// used for GBA ROM distributions.
#define ZIP_LFH_SIGNATURE 0x04034B50u
#define ZIP_CDR_SIGNATURE 0x02014B50u
#define ZIP_DATA_DESCRIPTOR_FLAG 0x0008u

static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void make_unzip_path(const char *zip_path, const char *entry_name, char *out, size_t out_size)
{
    const char *slash = strrchr(zip_path, '/');
    size_t dir_len = slash ? (size_t)(slash - zip_path + 1) : 0;
    const char *name = path_basename(entry_name);
    snprintf(out, out_size, "%.*s%s", (int)dir_len, zip_path, name);
}

static bool validate_gba_file(const char *path, uint32_t expected_size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    uint8_t head[4] = {0};
    bool ok = fread(head, 1, sizeof(head), fp) == sizeof(head) && head[3] == 0xEA;
    if (ok && expected_size > 0) {
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        ok = size == (long)expected_size;
    }
    fclose(fp);
    return ok;
}

static bool unzip_stored_to_file(FILE *zip, FILE *out, uint32_t usize, const char *name)
{
    uint8_t *buf = heap_caps_malloc(ZIP_IO_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(ZIP_IO_CHUNK);
    }
    if (!buf) {
        ESP_LOGE(TAG, "ZIP store: failed to alloc IO buffer");
        return false;
    }

    uint32_t written = 0;
    uint32_t next_log = 0;
    while (written < usize) {
        size_t want = usize - written;
        if (want > ZIP_IO_CHUNK) {
            want = ZIP_IO_CHUNK;
        }
        if (fread(buf, 1, want, zip) != want || fwrite(buf, 1, want, out) != want) {
            ESP_LOGE(TAG, "ZIP store: IO failed at %u/%u", (unsigned)written, (unsigned)usize);
            free(buf);
            return false;
        }
        written += want;
        if (usize == 0 || written >= usize || written >= next_log) {
            ESP_LOGW(TAG, "ZIP: extracting %s %u/%u bytes", name, (unsigned)written, (unsigned)usize);
            draw_zip_progress(name, written, usize);
            next_log = written + 512 * 1024;
        }
    }
    free(buf);
    return true;
}

static bool unzip_deflate_to_file(FILE *zip, FILE *out, uint32_t csize, uint32_t usize,
                                  bool has_descriptor, const char *name)
{
    uint8_t *in_buf = heap_caps_malloc(ZIP_IO_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *dict = heap_caps_malloc(ZIP_DICT_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    tinfl_decompressor *inflator = heap_caps_malloc(sizeof(tinfl_decompressor),
                                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!in_buf || !dict || !inflator) {
        ESP_LOGE(TAG, "ZIP deflate: buffer alloc failed");
        free(in_buf);
        free(dict);
        free(inflator);
        return false;
    }

    tinfl_init(inflator);
    size_t in_size = 0;
    size_t in_pos = 0;
    uint32_t compressed_read = 0;
    uint32_t written = 0;
    uint32_t dict_pos = 0;
    uint32_t next_log = 0;
    bool done = false;
    bool ok = true;

    while (!done) {
        if (in_pos == in_size) {
            size_t want = ZIP_IO_CHUNK;
            if (!has_descriptor && csize > compressed_read && want > csize - compressed_read) {
                want = csize - compressed_read;
            }
            if (!has_descriptor && compressed_read >= csize) {
                want = 0;
            }
            in_size = want ? fread(in_buf, 1, want, zip) : 0;
            in_pos = 0;
            compressed_read += (uint32_t)in_size;
            if (want && in_size == 0) {
                ESP_LOGE(TAG, "ZIP deflate: short read");
                ok = false;
                break;
            }
        }

        size_t in_avail = in_size - in_pos;
        size_t out_avail = ZIP_DICT_SIZE - dict_pos;
        mz_uint32 flags = 0;
        if (has_descriptor || compressed_read < csize || in_pos + in_avail < in_size) {
            flags |= TINFL_FLAG_HAS_MORE_INPUT;
        }

        tinfl_status status = tinfl_decompress(inflator, in_buf + in_pos, &in_avail,
                                               dict, dict + dict_pos, &out_avail, flags);
        in_pos += in_avail;
        if (out_avail > 0) {
            if (fwrite(dict + dict_pos, 1, out_avail, out) != out_avail) {
                ESP_LOGE(TAG, "ZIP deflate: write failed at %u/%u", (unsigned)written, (unsigned)usize);
                ok = false;
                break;
            }
            dict_pos = (dict_pos + out_avail) & (ZIP_DICT_SIZE - 1);
            written += (uint32_t)out_avail;
            if (written >= usize || written >= next_log) {
                ESP_LOGW(TAG, "ZIP: extracting %s %u/%u bytes", name, (unsigned)written, (unsigned)usize);
                draw_zip_progress(name, written, usize);
                next_log = written + 512 * 1024;
            }
        }

        if (status == TINFL_STATUS_DONE) {
            done = true;
        } else if (status < TINFL_STATUS_DONE) {
            ESP_LOGE(TAG, "ZIP deflate: tinfl failed, status=%d", (int)status);
            ok = false;
            break;
        } else if (in_avail == 0 && out_avail == 0 && in_pos == in_size) {
            ESP_LOGE(TAG, "ZIP deflate: no progress");
            ok = false;
            break;
        }
    }

    if (ok && written != usize) {
        ESP_LOGE(TAG, "ZIP deflate: produced %u bytes, expected %u", (unsigned)written, (unsigned)usize);
        ok = false;
    }

    free(in_buf);
    free(dict);
    free(inflator);
    return ok;
}

static bool extract_zip_rom_to_sd(const char *path, char *out_path, size_t out_path_size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "fopen %s failed", path);
        return false;
    }
    char target_name[80] = {0};
    uint16_t flags = 0;
    uint16_t method = 0;
    uint32_t csize = 0, usize = 0;
    long data_offset = -1;

    while (true) {
        uint8_t lfh[30];
        size_t got = fread(lfh, 1, sizeof(lfh), fp);
        if (got != sizeof(lfh)) {
            break;
        }
        uint32_t sig = le32(lfh);
        if (sig == ZIP_CDR_SIGNATURE) {
            break; // reached central directory; no more local files
        }
        if (sig != ZIP_LFH_SIGNATURE) {
            ESP_LOGE(TAG, "ZIP: bad LFH signature 0x%08x", (unsigned)sig);
            fclose(fp);
            return false;
        }
        uint16_t entry_flags  = le16(lfh + 6);
        uint16_t entry_method = le16(lfh + 8);
        uint32_t entry_csize  = le32(lfh + 18);
        uint32_t entry_usize  = le32(lfh + 22);
        uint16_t fname_len    = le16(lfh + 26);
        uint16_t extra_len    = le16(lfh + 28);
        char fname[160] = {0};
        size_t read_n = fname_len < sizeof(fname) - 1 ? fname_len : sizeof(fname) - 1;
        if (fread(fname, 1, read_n, fp) != read_n) {
            ESP_LOGE(TAG, "ZIP: short read on filename");
            fclose(fp);
            return false;
        }
        if (read_n < fname_len) {
            fseek(fp, fname_len - read_n, SEEK_CUR);
        }
        fseek(fp, extra_len, SEEK_CUR);

        size_t name_len = strlen(fname);
        bool is_gba = name_len >= 4 && strcasecmp(fname + name_len - 4, ".gba") == 0;
        if (is_gba) {
            strlcpy(target_name, fname, sizeof(target_name));
            flags = entry_flags;
            method = entry_method;
            csize = entry_csize;
            usize = entry_usize;
            data_offset = ftell(fp);
            break;
        }
        // Skip this entry's data.
        fseek(fp, entry_csize, SEEK_CUR);
    }

    if (data_offset < 0) {
        ESP_LOGE(TAG, "ZIP %s has no .gba entry", path);
        fclose(fp);
        return false;
    }
    if (method != 0 && method != 8) {
        ESP_LOGE(TAG, "ZIP entry %s uses unsupported compression method %u", target_name, method);
        fclose(fp);
        return false;
    }
    if (usize < 0xC0) {
        ESP_LOGE(TAG, "ZIP entry %s too small (%u bytes)", target_name, (unsigned)usize);
        fclose(fp);
        return false;
    }

    make_unzip_path(path, target_name, out_path, out_path_size);
    if (validate_gba_file(out_path, usize)) {
        ESP_LOGW(TAG, "ZIP: %s already extracted; using %s", path_basename(path), out_path);
        fclose(fp);
        remove(path);
        return true;
    }

    char tmp_path[ROM_PATH_MAX + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", out_path);
    remove(tmp_path);

    ESP_LOGW(TAG, "ZIP: extracting %s to %s (%s, %u -> %u bytes)",
             target_name, out_path, method == 0 ? "stored" : "deflate",
             (unsigned)csize, (unsigned)usize);
    draw_zip_progress(target_name, 0, usize);

    FILE *out = fopen(tmp_path, "wb");
    if (!out) {
        ESP_LOGE(TAG, "ZIP: fopen %s failed", tmp_path);
        fclose(fp);
        return false;
    }

    fseek(fp, data_offset, SEEK_SET);
    bool has_descriptor = (flags & ZIP_DATA_DESCRIPTOR_FLAG) != 0 || csize == 0;
    bool ok = method == 0
              ? unzip_stored_to_file(fp, out, usize, target_name)
              : unzip_deflate_to_file(fp, out, csize, usize, has_descriptor, target_name);

    fclose(out);
    fclose(fp);
    if (!ok || !validate_gba_file(tmp_path, usize)) {
        ESP_LOGE(TAG, "ZIP: extracted file is not a valid GBA ROM: %s", tmp_path);
        remove(tmp_path);
        return false;
    }

    remove(out_path);
    if (rename(tmp_path, out_path) != 0) {
        ESP_LOGE(TAG, "ZIP: rename %s -> %s failed", tmp_path, out_path);
        remove(tmp_path);
        return false;
    }

    ESP_LOGW(TAG, "ZIP: extracted OK, deleting original ZIP: %s", path);
    remove(path);
    draw_zip_progress(target_name, usize, usize);
    return true;
}

// Decompress a gzip stream into a power-of-two-padded PSRAM buffer.
// Uses the raw tinfl decoder; gzip header/trailer are stripped manually.
static void *load_rom_from_gzip(const char *path, size_t *alloc_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "fopen %s failed", path);
        return NULL;
    }
    // gzip header: magic(2) + method(1) + flags(1) + mtime(4) + xfl(1) + os(1) = 10 bytes
    uint8_t header[10];
    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        ESP_LOGE(TAG, "gzip: short header in %s", path);
        fclose(fp);
        return NULL;
    }
    uint8_t flg = header[3];
    if (flg & 0x04) { // FEXTRA
        uint8_t xlen[2];
        if (fread(xlen, 1, 2, fp) != 2) { fclose(fp); return NULL; }
        fseek(fp, xlen[0] | (xlen[1] << 8), SEEK_CUR);
    }
    if (flg & 0x08) {
        int c; while ((c = fgetc(fp)) != 0 && c != EOF) {}
    }
    if (flg & 0x10) {
        int c; while ((c = fgetc(fp)) != 0 && c != EOF) {}
    }
    if (flg & 0x02) {
        fseek(fp, 2, SEEK_CUR);
    }

    long deflate_start = ftell(fp);
    fseek(fp, -4, SEEK_END);
    uint8_t isz[4];
    fread(isz, 1, 4, fp);
    size_t raw = (size_t)(isz[0] | (isz[1] << 8) | (isz[2] << 16) | (isz[3] << 24));
    long file_end = ftell(fp);   // == (size - 4), but use ftell after seek to be safe
    long deflate_end = file_end; // last 4 bytes were isize, before them 4 bytes are crc32
    deflate_end -= 4;            // skip crc32
    size_t deflate_len = (size_t)(deflate_end - deflate_start);

    size_t alloc_size = 0;
    void *out = alloc_rom_buffer(raw, &alloc_size);
    if (!out) {
        ESP_LOGE(TAG, "Failed to alloc %u bytes for gunzipped ROM", (unsigned)alloc_size);
        fclose(fp);
        return NULL;
    }
    void *src = heap_caps_malloc(deflate_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!src) {
        src = malloc(deflate_len);
    }
    if (!src) {
        ESP_LOGE(TAG, "Failed to alloc %u bytes for gzip input", (unsigned)deflate_len);
        free(out);
        fclose(fp);
        return NULL;
    }
    fseek(fp, deflate_start, SEEK_SET);
    if (fread(src, 1, deflate_len, fp) != deflate_len) {
        ESP_LOGE(TAG, "gzip: short read");
        free(src);
        free(out);
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    ESP_LOGW(TAG, "GZIP: decompressing %s (%u -> %u bytes)", path,
             (unsigned)deflate_len, (unsigned)raw);
    size_t produced = tinfl_decompress_mem_to_mem(out, raw, src, deflate_len,
                                                 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(src);
    if (produced == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || produced != raw) {
        ESP_LOGE(TAG, "tinfl gzip failed: produced=%u expected=%u",
                 (unsigned)produced, (unsigned)raw);
        free(out);
        return NULL;
    }

    if (((const uint8_t *)out)[3] != 0xEA) {
        ESP_LOGE(TAG, "Decompressed gzip stream isn't a GBA ROM (byte[3]=%02x)", ((const uint8_t *)out)[3]);
        free(out);
        return NULL;
    }
    *alloc_out = alloc_size;
    return out;
}

static struct VFile *load_rom_vfile(const char *path)
{
    uint8_t head[8] = {0};
    rom_format_t fmt = detect_rom_format(path, head);

    free(s_rom_data);
    s_rom_data = NULL;
    s_rom_size = 0;
    strlcpy(s_loaded_rom_path, path, sizeof(s_loaded_rom_path));
    rom_cache_deinit();

    if (fmt == ROM_FORMAT_ZIP) {
        // Extract straight into the static loaded-rom path so the buffer
        // survives this scope (GCC otherwise flags `path` as a dangling
        // reference once `extracted_path` goes out of scope).
        if (!extract_zip_rom_to_sd(path, s_loaded_rom_path, sizeof(s_loaded_rom_path))) {
            return NULL;
        }
        ESP_LOGW(TAG, "ZIP ROM ready on SD: %s", s_loaded_rom_path);
        path = s_loaded_rom_path;
        fmt = ROM_FORMAT_GBA;
    }
    if (fmt == ROM_FORMAT_GZIP) {
        size_t alloc_size = 0;
        void *data = load_rom_from_gzip(path, &alloc_size);
        if (!data) {
            return NULL;
        }
        s_rom_data = data;
        s_rom_size = alloc_size;
        ESP_LOGW(TAG, "GZIP ROM ready: %u bytes (padded)", (unsigned)alloc_size);
        return VFileFromMemory(data, alloc_size);
    }
    if (fmt == ROM_FORMAT_UNKNOWN) {
        ESP_LOGE(TAG, "ROM header isn't GBA/ZIP/GZIP: bytes=%02x %02x %02x %02x. Path: %s",
                 head[0], head[1], head[2], head[3], path);
        return NULL;
    }

    // Raw .gba path - keep the original full-load (with paged-cache fallback)
    // behaviour.
    FILE *file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open ROM: %s", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "Failed to seek ROM end");
        fclose(file);
        return NULL;
    }
    long file_size = ftell(file);
    if (file_size <= 0) {
        ESP_LOGE(TAG, "Invalid ROM size: %ld", file_size);
        fclose(file);
        return NULL;
    }
    fclose(file);

    size_t file_size_bytes = (size_t)file_size;
    log_heap_state("Before ROM alloc", file_size_bytes);

    // Round up to a power of two. mGBA would otherwise allocate a second
    // (toPow2) buffer internally and memcpy into it — on PSRAM-tight boards
    // that double-alloc fails. Doing it ourselves keeps memory use to a
    // single buffer.
    size_t alloc_size = file_size_bytes;
    if (alloc_size & (alloc_size - 1)) {
        size_t pow2 = 1;
        while (pow2 < alloc_size) {
            pow2 <<= 1;
        }
        alloc_size = pow2;
    }

    // Try full in-memory load first - fastest path for ROMs that fit.
    void *data = heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        data = malloc(alloc_size);
    }
    if (data) {
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            free(data);
            return NULL;
        }
        uint8_t *out = (uint8_t *)data;
        size_t remaining = file_size_bytes;
        while (remaining > 0) {
            size_t chunk = remaining > 4096 ? 4096 : remaining;
            size_t got = fread(out, 1, chunk, fp);
            if (got != chunk) {
                ESP_LOGE(TAG, "ROM read failed at remaining=%u", (unsigned)remaining);
                fclose(fp);
                free(data);
                return NULL;
            }
            out += got;
            remaining -= got;
        }
        fclose(fp);
        if (alloc_size > file_size_bytes) {
            memset((uint8_t *)data + file_size_bytes, 0xFF, alloc_size - file_size_bytes);
        }
        s_rom_data = data;
        s_rom_size = alloc_size;
        ESP_LOGW(TAG, "Full ROM loaded into PSRAM: %u bytes (padded from %u)",
                 (unsigned)alloc_size, (unsigned)file_size_bytes);
        return VFileFromMemory(data, alloc_size);
    }

    // Fallback: demand-paged ROM cache.
    ESP_LOGW(TAG, "Full ROM alloc failed (%u bytes); falling back to %u MiB paged cache",
             (unsigned)file_size_bytes, (unsigned)(ROM_CACHE_BYTES_DEFAULT / (1024 * 1024)));
    // Hand the full default size; rom_cache_init will cap to (rom_pages + 1)
    // slots itself. Don't pre-cap to file size — that rounds down on a page
    // boundary and we end up one slot short, which makes the LRU thrash on
    // ROMs that don't quite fit (e.g. a 10.2 MiB ROM with 156 pages would get
    // only 155 slots, forcing constant SD I/O).
    if (!rom_cache_init(path, ROM_CACHE_BYTES_DEFAULT)) {
        ESP_LOGE(TAG, "rom_cache_init failed");
        return NULL;
    }
    void *base = (void *)rom_cache_buffer_base();
    size_t padded = rom_cache_padded_size();
    s_rom_size = padded;
    // Report the padded power-of-two size to mGBA so it doesn't try to expand
    // a non-pow2 ROM by allocating another 16/32 MiB buffer (which fails on
    // tight PSRAM). ROM access is re-routed through rom_cache_load*, which
    // returns the 0xFF padding slot for offsets past the real ROM end.
    return VFileFromMemory(base, padded);
}

static void release_rom_buffer(void)
{
    if (s_rom_data) {
        free(s_rom_data);
        s_rom_data = NULL;
    }
    s_rom_size = 0;
    s_loaded_rom_path[0] = '\0';
    rom_cache_deinit();
}

static void touch_isr_cb(esp_lcd_touch_handle_t tp)
{
    (void)tp;

    BaseType_t higher_priority_task_woken = pdFALSE;
    if (s_touch_task_handle) {
        vTaskNotifyGiveFromISR(s_touch_task_handle, &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

static void init_touch_interrupt(void)
{
    if (!s_touch_handle || !s_touch_task_handle) {
        return;
    }

    esp_err_t ret = esp_lcd_touch_register_interrupt_callback(s_touch_handle, touch_isr_cb);
    if (ret == ESP_OK) {
        s_touch_irq_mode = true;
        xTaskNotifyGive(s_touch_task_handle);
        ESP_LOGI(TAG, "Touch interrupt mode enabled");
    } else if (ret == ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "Touch INT GPIO is not configured; falling back to polling");
    } else {
        ESP_LOGW(TAG, "Touch interrupt setup failed (%s); falling back to polling", esp_err_to_name(ret));
    }
}

static void touch_task(void *arg)
{
    (void)arg;

    uint32_t last_keys = 0;
    uint8_t last_rom_index = 0xff;
    emu_cmd_t last_menu_cmd = EMU_CMD_NONE;
    bool ui_touch_latched = false;
    while (true) {
        if (s_touch_irq_mode) {
            TickType_t wait_ticks = last_keys ? pdMS_TO_TICKS(TOUCH_RELEASE_POLL_MS) : portMAX_DELAY;
            ulTaskNotifyTake(pdTRUE, wait_ticks);
        }

        uint32_t keys = 0;
        uint8_t selected_rom = 0xff;
        bool menu_pressed = false;
        emu_cmd_t menu_cmd = EMU_CMD_NONE;
        ui_mode_t ui_mode = atomic_load(&s_ui_mode);
        bool touch_active = false;
        bool state_slot_changed = false;

        if (s_touch_handle && esp_lcd_touch_read_data(s_touch_handle) == ESP_OK) {
            esp_lcd_touch_point_data_t points[TOUCH_MAX_POINTS] = {0};
            uint8_t point_num = 0;
            if (esp_lcd_touch_get_data(s_touch_handle, points, &point_num, TOUCH_MAX_POINTS) == ESP_OK && point_num > 0) {
                touch_active = true;
                if (ui_mode == UI_MODE_GAME) {
                    for (uint8_t point = 0; point < point_num; ++point) {
                        for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
                            const pad_button_t *btn = &s_buttons[i];
                            if (point_in_rect(points[point].x, points[point].y, btn->x, btn->y, btn->w, btn->h)) {
                                if (btn->key) {
                                    keys |= btn->key;
                                } else {
                                    menu_pressed = true;
                                }
                            }
                        }
                    }
                } else if (ui_mode == UI_MODE_GAME_MENU) {
                    if (!ui_touch_latched) {
                        for (uint8_t point = 0; point < point_num && menu_cmd == EMU_CMD_NONE; ++point) {
                            for (size_t i = 0; i < sizeof(s_menu_buttons) / sizeof(s_menu_buttons[0]); ++i) {
                                const menu_button_t *btn = &s_menu_buttons[i];
                                if (point_in_rect(points[point].x, points[point].y, btn->x, btn->y, btn->w, btn->h)) {
                                    menu_cmd = btn->cmd;
                                    break;
                                }
                            }
                            if (menu_cmd == EMU_CMD_NONE &&
                                    point_in_rect(points[point].x, points[point].y, 340, 202, 118, 36)) {
                                uint8_t slot = atomic_load(&s_state_slot);
                                atomic_store(&s_state_slot, slot == 0 ? STATE_SLOT_COUNT - 1 : slot - 1);
                                state_slot_changed = true;
                                break;
                            }
                            if (menu_cmd == EMU_CMD_NONE &&
                                    point_in_rect(points[point].x, points[point].y, 472, 202, 118, 36)) {
                                uint8_t slot = atomic_load(&s_state_slot);
                                atomic_store(&s_state_slot, (uint8_t)((slot + 1) % STATE_SLOT_COUNT));
                                state_slot_changed = true;
                                break;
                            }
                        }
                    }
                } else {
                    if (!ui_touch_latched) {
                        for (uint8_t point = 0; point < point_num && selected_rom == 0xff; ++point) {
                            if (s_rom_count > ROM_VISIBLE_MAX &&
                                    point_in_rect(points[point].x, points[point].y, 686, 96, 58, 58)) {
                                size_t offset = atomic_load(&s_rom_list_offset);
                                if (offset > 0) {
                                    size_t prev = offset > ROM_VISIBLE_MAX ? offset - ROM_VISIBLE_MAX : 0;
                                    atomic_store(&s_rom_list_offset, prev);
                                    draw_rom_menu(s_rom_count);
                                    ui_touch_latched = true;
                                }
                                break;
                            }
                            if (s_rom_count > ROM_VISIBLE_MAX &&
                                    point_in_rect(points[point].x, points[point].y, 686, 390, 58, 58)) {
                                size_t offset = atomic_load(&s_rom_list_offset);
                                if (offset + ROM_VISIBLE_MAX < s_rom_count) {
                                    atomic_store(&s_rom_list_offset, offset + ROM_VISIBLE_MAX);
                                    draw_rom_menu(s_rom_count);
                                    ui_touch_latched = true;
                                }
                                break;
                            }

                            size_t offset = atomic_load(&s_rom_list_offset);
                            size_t remaining = s_rom_count > offset ? s_rom_count - offset : 0;
                            size_t visible = remaining < ROM_VISIBLE_MAX ? remaining : ROM_VISIBLE_MAX;
                            for (size_t i = 0; i < visible; ++i) {
                                if (point_in_rect(points[point].x, points[point].y, 80, 96 + (int)i * 44, s_lcd_width - 160, 34)) {
                                    selected_rom = (uint8_t)(offset + i);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        if (ui_mode == UI_MODE_GAME) {
            atomic_store(&s_keys, keys);
            if (keys != last_keys) {
                draw_changed_buttons(last_keys, keys);
                last_keys = keys;
            }
            if (menu_pressed && last_menu_cmd == EMU_CMD_NONE) {
                atomic_store(&s_ui_mode, UI_MODE_GAME_MENU);
                audio_codec_set_paused(true);
                atomic_store(&s_keys, 0);
                last_keys = 0;
                draw_game_menu();
                last_menu_cmd = EMU_CMD_NONE;
                ui_touch_latched = true;
            }
        } else if (ui_mode == UI_MODE_GAME_MENU) {
            atomic_store(&s_keys, 0);
            if (state_slot_changed) {
                draw_game_menu();
                ui_touch_latched = true;
            }
            if (menu_cmd != last_menu_cmd) {
                if (menu_cmd != EMU_CMD_NONE) {
                    xQueueOverwrite(s_emu_cmd_queue, &menu_cmd);
                    ui_touch_latched = true;
                }
                last_menu_cmd = menu_cmd;
            }
        } else if (selected_rom != 0xff && selected_rom != last_rom_index) {
            char selected[ROM_PATH_MAX];
            strlcpy(selected, s_rom_paths[selected_rom], sizeof(selected));
            xQueueOverwrite(s_rom_queue, selected);
            last_rom_index = selected_rom;
            ui_touch_latched = true;
        } else if (selected_rom == 0xff) {
            last_rom_index = 0xff;
        }

        if (!touch_active) {
            ui_touch_latched = false;
        }

        if (menu_cmd == EMU_CMD_NONE && !menu_pressed) {
            last_menu_cmd = EMU_CMD_NONE;
        }

        if (!s_touch_irq_mode) {
            vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
        }
    }
}

static void emu_task(void *arg)
{
    (void)arg;

    update_status_detail("STARTING", "MOUNT SD CARD", "");
    esp_err_t sd_ret = mount_sdcard();
    if (sd_ret != ESP_OK && sd_ret != ESP_ERR_NOT_SUPPORTED) {
        update_status_detail("ERROR", "SD CARD MOUNT FAILED", esp_err_to_name(sd_ret));
        vTaskDelete(NULL);
        return;
    }
    if (sd_ret == ESP_ERR_NOT_SUPPORTED) {
        update_status_detail("ERROR", "SD CARD NOT SUPPORTED", "");
        vTaskDelete(NULL);
        return;
    }
    if (sd_ret == ESP_OK) {
        update_status_detail("STARTING", "LOADING SETTINGS", "");
        load_settings_from_sd();
    }

    while (true) {
        atomic_store(&s_ui_mode, UI_MODE_ROM_SELECT);
        atomic_store(&s_keys, 0);
        s_game_running = false;
        atomic_store(&s_rom_list_offset, 0);
        emu_cmd_t stale_cmd;
        while (xQueueReceive(s_emu_cmd_queue, &stale_cmd, 0) == pdTRUE) {
        }
        update_status_detail("STARTING", "SCANNING ROMS", ROM_DIR);
        size_t rom_count = scan_roms();
        draw_rom_menu(rom_count);

        char rom_path[ROM_PATH_MAX];
        if (xQueueReceive(s_rom_queue, rom_path, portMAX_DELAY) != pdTRUE) {
            update_status("ROM selection failed");
            continue;
        }

        clear_screen(RGB565(8, 12, 16));
        ESP_LOGI(TAG, "Loading ROM: %s", path_basename(rom_path));
        make_state_path(rom_path, s_state_path, sizeof(s_state_path));
        atomic_store(&s_state_slot, 0);

        struct mCore *core = GBACoreCreate();
        if (core) {
            // Idle loop optimisation lets mGBA detect tight wait-for-interrupt
            // spins in game code and skip the emulator forward instead of
            // burning RISC-V cycles on them. Pokemon and similar games can
            // sit in idle loops 30-50% of the time, so this is by far the
            // biggest single emu-speed knob.
            mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "detect");
        }
        if (!core || !core->init || !core->setVideoBuffer || !core->reset ||
                !core->runFrame || !core->setKeys || !core->deinit || !core->init(core)) {
            update_status("mGBA core init failed");
            if (core && core->deinit) {
                core->deinit(core);
            }
            continue;
        }
        ESP_LOGI(TAG, "mGBA core initialized");

        apply_runtime_options(core);
        core->opts.mute = !atomic_load(&s_audio_enabled);
        core->opts.volume = atomic_load(&s_audio_enabled) ? mgba_volume_from_percent(atomic_load(&s_audio_volume)) : 0;
        core->reloadConfigOption(core, NULL, NULL);
        core->setVideoBuffer(core, s_active_framebuffer, GBA_WIDTH);
        memset(&s_audio_stream, 0, sizeof(s_audio_stream));
        s_audio_stream.stream.audioRateChanged = audio_rate_changed;
        s_audio_stream.stream.postAudioBuffer = audio_post_buffer;
        atomic_store(&s_audio_source_rate, AUDIO_CORE_SAMPLE_RATE);
        atomic_store(&s_audio_paused, false);
        core->setAudioBufferSize(core, AUDIO_BUFFER_SAMPLES);
        core->setAVStream(core, &s_audio_stream.stream);
        clear_audio_queue();
        if (atomic_load(&s_audio_enabled)) {
            audio_codec_set_enabled(true);
        }

        // Optional: real GBA BIOS at /sdcard/gba_bios.bin (16 KiB). Falls back
        // to mGBA's HLE BIOS if missing — most games run fine on HLE, but
        // some (e.g. RNG-sensitive Pokemon sequences) behave more accurately
        // with the real BIOS and avoid `Stub opcode` warnings.
        struct VFile *bios_vf = VFileOpen(ROM_DIR "/gba_bios.bin", O_RDONLY);
        if (bios_vf) {
            if (core->loadBIOS && core->loadBIOS(core, bios_vf, 0)) {
                ESP_LOGI(TAG, "Real GBA BIOS loaded");
            } else {
                ESP_LOGW(TAG, "BIOS rejected (wrong size/checksum); using HLE");
                bios_vf->close(bios_vf);
            }
        } else {
            ESP_LOGI(TAG, "No /sdcard/gba_bios.bin; using HLE BIOS");
        }

        struct VFile *rom = load_rom_vfile(rom_path);
        if (!rom || !core->loadROM(core, rom)) {
            if (rom) {
                rom->close(rom);
            }
            update_status("ROM load failed");
            audio_codec_set_enabled(false);
            core->deinit(core);
            continue;
        }
        ESP_LOGI(TAG, "ROM loaded");

        // Persistent battery-backed save: <ROM>.sav on the SD card. mGBA opens
        // it as read+write; if it doesn't exist it gets created on first write.
        char save_path[STATE_PATH_MAX];
        make_save_path(s_loaded_rom_path[0] ? s_loaded_rom_path : rom_path, save_path, sizeof(save_path));
        struct VFile *save_vf = VFileOpen(save_path, O_RDWR | O_CREAT);
        if (save_vf && core->loadSave) {
            if (!core->loadSave(core, save_vf)) {
                ESP_LOGW(TAG, "Load save rejected: %s", save_path);
                save_vf->close(save_vf);
            }
        } else if (!save_vf) {
            ESP_LOGW(TAG, "Could not open save file: %s", save_path);
        }

        // RTC for games like Pokemon Emerald that have a cart RTC.
        mCoreSetRTC(core, &s_rtc_source);

        core->reset(core);
        s_game_running = true;
        atomic_store(&s_ui_mode, UI_MODE_GAME);
        redraw_game_screen();
        ESP_LOGI(TAG, "GBA core running: %s", path_basename(s_loaded_rom_path[0] ? s_loaded_rom_path : rom_path));

        const int64_t frame_us = 1000000 / GBA_FPS;
        uint32_t frame_count = 0;
        uint32_t frames_this_second = 0;
        uint32_t display_interval = ((uint32_t)atomic_load(&s_frameskip) + 1) * DISPLAY_FRAME_DIV;
        int64_t fps_time = esp_timer_get_time();
        int64_t next_frame = esp_timer_get_time();
        bool return_to_rom_menu = false;
        mColor *last_video_buffer = NULL;
        bool audio_menu_paused = false;

        while (!return_to_rom_menu) {
            emu_cmd_t cmd = EMU_CMD_NONE;
            while (xQueueReceive(s_emu_cmd_queue, &cmd, 0) == pdTRUE) {
                switch (cmd) {
                case EMU_CMD_RESUME:
                    atomic_store(&s_ui_mode, UI_MODE_GAME);
                    audio_codec_set_paused(false);
                    audio_menu_paused = false;
                    core->opts.mute = !atomic_load(&s_audio_enabled);
                    apply_audio_volume(core);
                    redraw_game_screen();
                    break;
                case EMU_CMD_SAVE_STATE:
                {
                    char path[STATE_PATH_MAX];
                    make_state_slot_path(atomic_load(&s_state_slot), path, sizeof(path));
                    save_state_file(core, path);
                    atomic_store(&s_ui_mode, UI_MODE_GAME);
                    audio_codec_set_paused(false);
                    audio_menu_paused = false;
                    core->opts.mute = !atomic_load(&s_audio_enabled);
                    apply_audio_volume(core);
                    redraw_game_screen();
                    break;
                }
                case EMU_CMD_LOAD_STATE:
                {
                    char path[STATE_PATH_MAX];
                    make_state_slot_path(atomic_load(&s_state_slot), path, sizeof(path));
                    load_state_file(core, path);
                    atomic_store(&s_ui_mode, UI_MODE_GAME);
                    audio_codec_set_paused(false);
                    audio_menu_paused = false;
                    core->opts.mute = !atomic_load(&s_audio_enabled);
                    apply_audio_volume(core);
                    redraw_game_screen();
                    break;
                }
                case EMU_CMD_DELETE_STATE:
                {
                    char path[STATE_PATH_MAX];
                    make_state_slot_path(atomic_load(&s_state_slot), path, sizeof(path));
                    delete_state_file(path);
                    draw_game_menu();
                    break;
                }
                case EMU_CMD_TOGGLE_FAST:
                    atomic_store(&s_fast_forward, !atomic_load(&s_fast_forward));
                    save_settings_to_sd();
                    draw_game_menu();
                    break;
                case EMU_CMD_TOGGLE_AUDIO:
                    atomic_store(&s_audio_enabled, !atomic_load(&s_audio_enabled));
                    audio_codec_set_enabled(atomic_load(&s_audio_enabled));
                    core->opts.mute = !atomic_load(&s_audio_enabled) || atomic_load(&s_audio_paused);
                    apply_audio_volume(core);
                    save_settings_to_sd();
                    draw_game_menu();
                    break;
                case EMU_CMD_VOLUME_DOWN:
                {
                    uint8_t volume = atomic_load(&s_audio_volume);
                    atomic_store(&s_audio_volume, volume > 10 ? volume - 10 : 0);
                    apply_audio_volume(core);
                    save_settings_to_sd();
                    draw_game_menu();
                    break;
                }
                case EMU_CMD_VOLUME_UP:
                {
                    uint8_t volume = atomic_load(&s_audio_volume);
                    atomic_store(&s_audio_volume, volume < 90 ? volume + 10 : 100);
                    apply_audio_volume(core);
                    save_settings_to_sd();
                    draw_game_menu();
                    break;
                }
                case EMU_CMD_CYCLE_FRAMESKIP:
                {
                    uint8_t frameskip = atomic_load(&s_frameskip);
                    atomic_store(&s_frameskip, (uint8_t)((frameskip + 1) % (MAX_FRAMESKIP + 1)));
                    display_interval = ((uint32_t)atomic_load(&s_frameskip) + 1) * DISPLAY_FRAME_DIV;
                    apply_runtime_options(core);
                    save_settings_to_sd();
                    draw_game_menu();
                    break;
                }
                case EMU_CMD_TOGGLE_SKIP_BIOS:
                    atomic_store(&s_skip_bios, !atomic_load(&s_skip_bios));
                    apply_runtime_options(core);
                    save_settings_to_sd();
                    draw_game_menu();
                    break;
                case EMU_CMD_TOGGLE_AUDIO_SYNC:
                    atomic_store(&s_audio_sync, !atomic_load(&s_audio_sync));
                    apply_runtime_options(core);
                    save_settings_to_sd();
                    draw_game_menu();
                    break;
                case EMU_CMD_TOGGLE_VIDEO_SYNC:
                    atomic_store(&s_video_sync, !atomic_load(&s_video_sync));
                    apply_runtime_options(core);
                    save_settings_to_sd();
                    draw_game_menu();
                    break;
                case EMU_CMD_TOGGLE_SHOW_FPS:
                    atomic_store(&s_show_fps, !atomic_load(&s_show_fps));
                    save_settings_to_sd();
                    if (!atomic_load(&s_show_fps)) {
                        ui_lock();
                        fill_rect(650, 72, 132, 30, RGB565(8, 12, 16));
                        flush_lcd_rect(650, 72, 132, 30);
                        ui_unlock();
                    }
                    draw_game_menu();
                    break;
                case EMU_CMD_TOGGLE_SCALE_LINEAR:
                    atomic_store(&s_scale_linear, !atomic_load(&s_scale_linear));
                    save_settings_to_sd();
                    draw_game_menu();
                    break;
                case EMU_CMD_ROM_MENU:
                    audio_codec_set_paused(true);
                    core->opts.mute = true;
                    apply_audio_volume(core);
                    return_to_rom_menu = true;
                    break;
                default:
                    break;
                }
            }

            if (return_to_rom_menu) {
                break;
            }

            if (atomic_load(&s_ui_mode) == UI_MODE_GAME_MENU) {
                if (!audio_menu_paused) {
                    audio_codec_set_paused(true);
                    core->opts.mute = true;
                    apply_audio_volume(core);
                    audio_menu_paused = true;
                }
                vTaskDelay(pdMS_TO_TICKS(15));
                next_frame = esp_timer_get_time();
                continue;
            }

            if (s_active_framebuffer != last_video_buffer) {
                core->setVideoBuffer(core, s_active_framebuffer, GBA_WIDTH);
                last_video_buffer = s_active_framebuffer;
            }
            core->setKeys(core, atomic_load(&s_keys));
            core->runFrame(core);
            ++frame_count;
            ++frames_this_second;
            if ((frame_count % display_interval) == 0) {
                request_render_frame();
            }

            int64_t now = esp_timer_get_time();
            if (atomic_load(&s_show_fps) && now - fps_time >= 1000000) {
                float fps = (float)frames_this_second * 1000000.0f / (float)(now - fps_time);
                draw_fps(fps);
                frames_this_second = 0;
                fps_time = now;
            } else if (!atomic_load(&s_show_fps) && now - fps_time >= 1000000) {
                frames_this_second = 0;
                fps_time = now;
            }

            if (!atomic_load(&s_fast_forward)) {
                next_frame += frame_us;
                int64_t delay_us = next_frame - now;
                if (delay_us > 0) {
                    vTaskDelay(pdMS_TO_TICKS((delay_us + 999) / 1000));
                } else {
                    next_frame = esp_timer_get_time();
                    vTaskDelay(1);
                }
            } else {
                next_frame = esp_timer_get_time();
                taskYIELD();
            }
        }

        s_game_running = false;
        atomic_store(&s_ui_mode, UI_MODE_ROM_SELECT);
        atomic_store(&s_keys, 0);
        atomic_store(&s_rom_list_offset, 0);
        audio_codec_set_enabled(false);
        clear_audio_queue();
        core->deinit(core);
        release_rom_buffer();
    }
}

void app_main(void)
{
    atomic_store(&s_audio_volume, 80);
    atomic_store(&s_audio_source_rate, AUDIO_CORE_SAMPLE_RATE);
    atomic_store(&s_audio_paused, false);
    atomic_store(&s_fast_forward, false);
    atomic_store(&s_audio_enabled, false);
    atomic_store(&s_frameskip, DEFAULT_FRAMESKIP);
    atomic_store(&s_skip_bios, true);
    atomic_store(&s_audio_sync, false);
    atomic_store(&s_video_sync, false);
    atomic_store(&s_show_fps, false);

    s_ui_lock = xSemaphoreCreateMutex();
    if (!s_ui_lock) {
        ESP_LOGE(TAG, "Failed to create UI lock");
        return;
    }

    s_rom_queue = xQueueCreate(1, ROM_PATH_MAX);
    if (!s_rom_queue) {
        ESP_LOGE(TAG, "Failed to create ROM queue");
        return;
    }
    s_emu_cmd_queue = xQueueCreate(1, sizeof(emu_cmd_t));
    if (!s_emu_cmd_queue) {
        ESP_LOGE(TAG, "Failed to create emulator command queue");
        return;
    }
    s_frame_lock = xSemaphoreCreateMutex();
    if (!s_frame_lock) {
        ESP_LOGE(TAG, "Failed to create frame lock");
        return;
    }
    s_ppa_done_sem = xSemaphoreCreateBinary();
    if (!s_ppa_done_sem) {
        ESP_LOGE(TAG, "Failed to create PPA done semaphore");
        return;
    }
    s_audio_ringbuf = xRingbufferCreate(AUDIO_CHUNK_SAMPLES * 2 * sizeof(int16_t) * AUDIO_RING_CHUNKS,
                                        RINGBUF_TYPE_NOSPLIT);
    if (!s_audio_ringbuf) {
        ESP_LOGE(TAG, "Failed to create audio ring buffer");
        return;
    }
    mAudioBufferInit(&s_audio_resample_source, AUDIO_RESAMPLE_SOURCE_SAMPLES, 2);
    mAudioBufferInit(&s_audio_resample_dest, AUDIO_RESAMPLE_DEST_SAMPLES, 2);
    mAudioResamplerInit(&s_audio_resampler, mINTERPOLATOR_COSINE);
    mAudioResamplerSetDestination(&s_audio_resampler, &s_audio_resample_dest, AUDIO_CODEC_SAMPLE_RATE);
    mAudioResamplerSetSource(&s_audio_resampler, &s_audio_resample_source, AUDIO_CORE_SAMPLE_RATE, true);

    // Frame buffers go to PSRAM on purpose: each is 150 KiB and there's not
    // enough INTERNAL DRAM left for them PLUS GBA EWRAM (288 KiB) + struct GBA
    // (~120 KiB with VRAM). EWRAM is hit millions of times per second by ARM
    // emulation and absolutely must stay in INTERNAL SRAM. Scanline renderer
    // writes to the framebuffer are sequential streams that the L1/L2 cache
    // handles well in PSRAM, and PPA reads from PSRAM equally fast either way.
    for (size_t i = 0; i < 2; ++i) {
        s_framebuffers[i] = aligned_calloc_caps(FRAMEBUFFER_ALIGN, GBA_WIDTH * GBA_HEIGHT, sizeof(mColor),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_framebuffers[i]) {
            s_framebuffers[i] = aligned_calloc_caps(FRAMEBUFFER_ALIGN, GBA_WIDTH * GBA_HEIGHT, sizeof(mColor),
                                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (!s_framebuffers[i]) {
            s_framebuffers[i] = aligned_calloc_caps(FRAMEBUFFER_ALIGN, GBA_WIDTH * GBA_HEIGHT, sizeof(mColor),
                                                    MALLOC_CAP_8BIT);
        }
        if (!s_framebuffers[i]) {
            ESP_LOGE(TAG, "Failed to allocate framebuffer %u", (unsigned)i);
            return;
        }
    }
    s_active_framebuffer = s_framebuffers[0];
    s_render_framebuffer = s_framebuffers[0];
    log_memory_usage();

    esp_err_t ret = init_board_display_touch();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display/touch initialization failed: %s", esp_err_to_name(ret));
        return;
    }
    update_status_detail("STARTING", "DISPLAY READY", "");
    ret = init_board_audio();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Audio codec disabled: %s", esp_err_to_name(ret));
        s_audio_codec = NULL;
        atomic_store(&s_audio_enabled, false);
        update_status_detail("STARTING", "AUDIO DISABLED", esp_err_to_name(ret));
    } else {
        update_status_detail("STARTING", "AUDIO READY", "");
    }

    if (xTaskCreatePinnedToCore(touch_task, "touch", TOUCH_TASK_STACK, NULL, TOUCH_TASK_PRIORITY,
                                &s_touch_task_handle, TOUCH_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create touch task");
        return;
    }
    init_touch_interrupt();

    if (xTaskCreatePinnedToCore(render_task, "render", RENDER_TASK_STACK, NULL, RENDER_TASK_PRIORITY,
                                &s_render_task_handle, RENDER_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create render task");
        return;
    }
    if (xTaskCreatePinnedToCore(audio_task, "audio", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIORITY,
                                NULL, AUDIO_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio task");
        return;
    }

    if (xTaskCreatePinnedToCore(mem_monitor_task, "memmon", MEM_MONITOR_STACK, NULL, MEM_MONITOR_PRIORITY,
                                NULL, MEM_MONITOR_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create memory monitor task");
        return;
    }

    if (xTaskCreatePinnedToCore(emu_task, "mgba", EMU_TASK_STACK, NULL, EMU_TASK_PRIORITY,
                                NULL, EMU_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mGBA task");
        return;
    }
}
