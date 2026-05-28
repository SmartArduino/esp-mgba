#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Demand-paged ROM cache for GBA ROMs that don't fit in PSRAM.
// Layout: ROM in SD card -> N x PAGE_SIZE slots in PSRAM, with LRU replacement.

#define ROM_CACHE_PAGE_SHIFT 16
#define ROM_CACHE_PAGE_SIZE  (1u << ROM_CACHE_PAGE_SHIFT)   // 64 KiB
#define ROM_CACHE_PAGE_MASK  (ROM_CACHE_PAGE_SIZE - 1)
#define ROM_CACHE_MAX_PAGES  512  // 32 MiB / 64 KiB

typedef struct {
    void *base;                 // cache buffer in PSRAM, slot_count * PAGE_SIZE bytes
    void *fp;                   // FILE* (opaque to header)
    size_t rom_size;
    size_t padded_size;         // rom_size rounded up to next power of two
    uint32_t rom_pages;         // ceil(rom_size / PAGE_SIZE)
    uint32_t slot_count;
    uint16_t page_to_slot[ROM_CACHE_MAX_PAGES];  // 0xFFFF = not cached
    uint16_t *slot_to_page;     // slot_count entries
    uint32_t *slot_last_used;   // slot_count entries, monotonic counter
    uint32_t lru_counter;
    // Stats
    uint64_t hit_count;
    uint64_t miss_count;
} rom_cache_t;

// Initialize a global cache from a ROM file path. Allocates `cache_bytes` of
// PSRAM and opens the ROM file for paging. Returns false on failure.
bool rom_cache_init(const char *path, size_t cache_bytes);

// Tear down the global cache, free PSRAM and close the file.
void rom_cache_deinit(void);

// True if a cache has been initialized. This is a plain global so callers in
// hot paths (mGBA's memory.c) can read it without a function call.
extern bool g_rom_cache_active;
static inline bool rom_cache_active(void) { return g_rom_cache_active; }

// Returns the actual ROM size (in bytes) opened by rom_cache_init.
size_t rom_cache_size(void);

// Returns the actual ROM size rounded up to the next power of two. mGBA loads
// non-power-of-two ROMs by allocating a fresh toPow2 buffer and memcpy'ing
// into it — on tight PSRAM that fails. By reporting the padded size as the
// VFile size and faking the extra range with 0xFF pages, we let mGBA see a
// power-of-two ROM without any extra allocation.
size_t rom_cache_padded_size(void);

// Returns the start of the cache buffer in PSRAM. Page 0 is pinned to slot 0,
// so the first 64 KiB of the buffer always mirrors the first 64 KiB of the
// ROM. This lets mGBA initialization code that reads the cartridge header
// directly via memory->rom (e.g. GBAHardwareInit) work transparently.
const uint8_t *rom_cache_buffer_base(void);

// Resolve a ROM address to a host pointer. May trigger an SD load on miss.
// Always returns non-NULL while the cache is active.
const uint8_t *rom_cache_page_for_addr(uint32_t rom_offset);

// Convenience: load 32/16/8-bit values from ROM at the given offset.
// rom_offset must be < rom_size; the caller is responsible for masking.
uint32_t rom_cache_load32(uint32_t rom_offset);
uint16_t rom_cache_load16(uint32_t rom_offset);
uint8_t  rom_cache_load8 (uint32_t rom_offset);

// Stats for debug.
void rom_cache_stats(uint64_t *hits, uint64_t *misses);

#ifdef __cplusplus
}
#endif
