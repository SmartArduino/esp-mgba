#include "rom_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "rom_cache";

#define SLOT_INVALID 0xFFFFu

static rom_cache_t s_cache;
bool g_rom_cache_active;
#define s_active g_rom_cache_active

// Slot 0 is permanently pinned to ROM page 0 (cartridge header).
// Slot 1 is the "out-of-range padding" slot pre-filled with 0xFF, used when
// mGBA reads addresses inside the padded power-of-two view but past the
// real ROM size. Skip both when picking an LRU victim.
#define ROM_CACHE_PINNED_SLOTS 2u
static IRAM_ATTR uint32_t evict_lru_slot(void)
{
    uint32_t oldest_slot = ROM_CACHE_PINNED_SLOTS;
    uint32_t oldest_ts = UINT32_MAX;
    for (uint32_t i = ROM_CACHE_PINNED_SLOTS; i < s_cache.slot_count; ++i) {
        if (s_cache.slot_last_used[i] < oldest_ts) {
            oldest_ts = s_cache.slot_last_used[i];
            oldest_slot = i;
        }
    }
    return oldest_slot;
}

static uint32_t IRAM_ATTR load_page(uint32_t page_idx)
{
    uint32_t slot = evict_lru_slot();
    uint16_t old_page = s_cache.slot_to_page[slot];
    if (old_page != SLOT_INVALID) {
        s_cache.page_to_slot[old_page] = SLOT_INVALID;
    }
    s_cache.page_to_slot[page_idx] = (uint16_t)slot;
    s_cache.slot_to_page[slot] = (uint16_t)page_idx;
    s_cache.slot_last_used[slot] = ++s_cache.lru_counter;

    FILE *fp = (FILE *)s_cache.fp;
    long offset = (long)page_idx * ROM_CACHE_PAGE_SIZE;
    long want = ROM_CACHE_PAGE_SIZE;
    if ((uint64_t)offset + want > s_cache.rom_size) {
        want = (long)(s_cache.rom_size - offset);
    }
    uint8_t *dst = (uint8_t *)s_cache.base + (size_t)slot * ROM_CACHE_PAGE_SIZE;
    fseek(fp, offset, SEEK_SET);
    size_t got = fread(dst, 1, (size_t)want, fp);
    if ((long)got < want) {
        ESP_LOGE(TAG, "page %u: short read (%u of %ld)", (unsigned)page_idx, (unsigned)got, want);
        memset(dst + got, 0xFF, (size_t)((long)ROM_CACHE_PAGE_SIZE - (long)got));
    } else if (got < ROM_CACHE_PAGE_SIZE) {
        // Pad the rest of the slot for safety.
        memset(dst + got, 0xFF, ROM_CACHE_PAGE_SIZE - got);
    }
    s_cache.miss_count++;
    return slot;
}

const uint8_t *IRAM_ATTR rom_cache_page_for_addr(uint32_t rom_offset)
{
    uint32_t page_idx = rom_offset >> ROM_CACHE_PAGE_SHIFT;
    if (page_idx >= s_cache.rom_pages) {
        // Out-of-range read in the padded power-of-two view. Return the
        // pre-filled 0xFF slot; this mirrors the behaviour real GBA carts
        // have for reads past the end of the ROM.
        return (const uint8_t *)s_cache.base + (size_t)1 * ROM_CACHE_PAGE_SIZE;
    }
    uint16_t slot = s_cache.page_to_slot[page_idx];
    if (slot == SLOT_INVALID) {
        slot = (uint16_t)load_page(page_idx);
    } else {
        s_cache.slot_last_used[slot] = ++s_cache.lru_counter;
        s_cache.hit_count++;
    }
    return (const uint8_t *)s_cache.base + (size_t)slot * ROM_CACHE_PAGE_SIZE;
}

uint32_t IRAM_ATTR rom_cache_load32(uint32_t rom_offset)
{
    const uint8_t *page = rom_cache_page_for_addr(rom_offset);
    return *(const uint32_t *)(page + (rom_offset & ROM_CACHE_PAGE_MASK));
}

uint16_t IRAM_ATTR rom_cache_load16(uint32_t rom_offset)
{
    const uint8_t *page = rom_cache_page_for_addr(rom_offset);
    return *(const uint16_t *)(page + (rom_offset & ROM_CACHE_PAGE_MASK));
}

uint8_t IRAM_ATTR rom_cache_load8(uint32_t rom_offset)
{
    const uint8_t *page = rom_cache_page_for_addr(rom_offset);
    return page[rom_offset & ROM_CACHE_PAGE_MASK];
}

bool rom_cache_init(const char *path, size_t cache_bytes)
{
    if (s_active) {
        rom_cache_deinit();
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "open %s failed", path);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size <= 0) {
        ESP_LOGE(TAG, "bad ROM size: %ld", size);
        fclose(fp);
        return false;
    }
    fseek(fp, 0, SEEK_SET);

    uint32_t rom_pages = (uint32_t)((size + ROM_CACHE_PAGE_SIZE - 1) >> ROM_CACHE_PAGE_SHIFT);
    if (rom_pages > ROM_CACHE_MAX_PAGES) {
        ESP_LOGE(TAG, "ROM too large: %u pages > %u", (unsigned)rom_pages, ROM_CACHE_MAX_PAGES);
        fclose(fp);
        return false;
    }

    // Round cache_bytes down to a page boundary.
    cache_bytes &= ~(size_t)ROM_CACHE_PAGE_MASK;
    if (cache_bytes < ROM_CACHE_PAGE_SIZE * 4) {
        ESP_LOGE(TAG, "cache too small: %u bytes", (unsigned)cache_bytes);
        fclose(fp);
        return false;
    }
    uint32_t slot_count = (uint32_t)(cache_bytes / ROM_CACHE_PAGE_SIZE);
    // We always reserve one slot for "out-of-range padding" (slot 1) and one
    // for pinned page 0, so we need at least PINNED_SLOTS + 1 working LRU
    // entries.
    if (slot_count < ROM_CACHE_PINNED_SLOTS + 1) {
        slot_count = ROM_CACHE_PINNED_SLOTS + 1;
        cache_bytes = (size_t)slot_count * ROM_CACHE_PAGE_SIZE;
    }
    // We need room for at most (rom_pages + 1 padding slot) total. Cap to
    // that so we don't allocate slots we'll never use.
    uint32_t max_useful = rom_pages + 1;
    if (slot_count > max_useful) {
        slot_count = max_useful;
        cache_bytes = (size_t)slot_count * ROM_CACHE_PAGE_SIZE;
    }

    ESP_LOGW(TAG, "alloc cache buffer: %u KiB in PSRAM", (unsigned)(cache_bytes / 1024));
    void *base = heap_caps_aligned_alloc(64, cache_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!base) {
        ESP_LOGE(TAG, "cache buffer alloc failed: %u bytes", (unsigned)cache_bytes);
        fclose(fp);
        return false;
    }
    ESP_LOGW(TAG, "cache buffer at %p", base);
    uint16_t *slot_to_page = heap_caps_malloc(slot_count * sizeof(uint16_t),
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t *slot_last_used = heap_caps_calloc(slot_count, sizeof(uint32_t),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!slot_to_page || !slot_last_used) {
        ESP_LOGE(TAG, "metadata alloc failed: stp=%p slu=%p",
                 (void*)slot_to_page, (void*)slot_last_used);
        free(slot_to_page);
        free(slot_last_used);
        heap_caps_free(base);
        fclose(fp);
        return false;
    }
    ESP_LOGW(TAG, "metadata: slot_to_page=%p slot_last_used=%p", (void*)slot_to_page, (void*)slot_last_used);
    for (uint32_t i = 0; i < slot_count; ++i) {
        slot_to_page[i] = SLOT_INVALID;
    }
    for (uint32_t i = 0; i < ROM_CACHE_MAX_PAGES; ++i) {
        s_cache.page_to_slot[i] = SLOT_INVALID;
    }
    // Padded view: round ROM size up to next power of two so mGBA never tries
    // to allocate a second toPow2 buffer at load time.
    size_t padded = (size_t)size;
    if (padded & (padded - 1)) {
        size_t p = 1;
        while (p < padded) {
            p <<= 1;
        }
        padded = p;
    }

    s_cache.base = base;
    s_cache.fp = fp;
    s_cache.rom_size = (size_t)size;
    s_cache.padded_size = padded;
    s_cache.rom_pages = rom_pages;
    s_cache.slot_count = slot_count;
    s_cache.slot_to_page = slot_to_page;
    s_cache.slot_last_used = slot_last_used;
    s_cache.lru_counter = 0;
    s_cache.hit_count = 0;
    s_cache.miss_count = 0;
    s_active = true;

    // Pin page 0 into slot 0 (cartridge header / vectors / mGBA init reads).
    fseek(fp, 0, SEEK_SET);
    long want0 = ROM_CACHE_PAGE_SIZE < (size_t)size ? ROM_CACHE_PAGE_SIZE : size;
    fread((uint8_t *)base, 1, (size_t)want0, fp);
    if ((size_t)want0 < ROM_CACHE_PAGE_SIZE) {
        memset((uint8_t *)base + want0, 0xFF, ROM_CACHE_PAGE_SIZE - want0);
    }
    s_cache.page_to_slot[0] = 0;
    s_cache.slot_to_page[0] = 0;
    s_cache.slot_last_used[0] = UINT32_MAX; // pinned

    // Slot 1: pre-filled 0xFF "out of range" padding page, returned for any
    // address in the padded power-of-two view past the real ROM end.
    memset((uint8_t *)base + ROM_CACHE_PAGE_SIZE, 0xFF, ROM_CACHE_PAGE_SIZE);
    s_cache.slot_to_page[1] = SLOT_INVALID;     // not mapped to a real page
    s_cache.slot_last_used[1] = UINT32_MAX;     // pinned

    ESP_LOGW(TAG, "ROM %ld bytes (padded %u), %u pages, %u-slot (%u KiB) cache; page 0 pinned, slot 1 padding",
             size, (unsigned)padded, (unsigned)rom_pages, (unsigned)slot_count,
             (unsigned)(cache_bytes / 1024));
    return true;
}

size_t rom_cache_padded_size(void)
{
    return s_active ? s_cache.padded_size : 0;
}

const uint8_t *rom_cache_buffer_base(void)
{
    return s_active ? (const uint8_t *)s_cache.base : NULL;
}

void rom_cache_deinit(void)
{
    if (!s_active) {
        return;
    }
    if (s_cache.fp) {
        fclose((FILE *)s_cache.fp);
    }
    if (s_cache.base) {
        heap_caps_free(s_cache.base);
    }
    free(s_cache.slot_to_page);
    free(s_cache.slot_last_used);
    memset(&s_cache, 0, sizeof(s_cache));
    s_active = false;
}


size_t rom_cache_size(void)
{
    return s_active ? s_cache.rom_size : 0;
}

void rom_cache_stats(uint64_t *hits, uint64_t *misses)
{
    if (hits) {
        *hits = s_cache.hit_count;
    }
    if (misses) {
        *misses = s_cache.miss_count;
    }
}
