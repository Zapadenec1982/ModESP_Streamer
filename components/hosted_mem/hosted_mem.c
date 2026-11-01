/*
 * hosted_mem.c — esp_hosted's per-frame copies out of internal DMA RAM, and a count of the
 * frames it drops when it cannot allocate one.
 *
 * WHAT ESP_HOSTED DOES
 *
 * On this board the P4 reads the C6 over SDIO in SW_AGGR mode (boot log: "SDIO SW_AGGR
 * negotiated (e2h=15872B h2e=15872B)"). One SDIO read brings an aggregate of up to ~10
 * frames into a staging buffer, and the de-aggregation task copies each frame out of it:
 *
 *     host/mcu/eh_host_mcu_transport/src/eh_host_bus_sdio.c (v3.0.7)
 *       sdio_buffer_alloc()        -> eh_host_port_dma_alloc(MAX_SDIO_BUFFER_SIZE)   :337
 *       sdio_push_aggr_to_queue()  -> copy, or "OOM: skip this sub-frame"            :1287-1292
 *
 * and every frame lwIP sends is copied the same way before it is packed into an aggregate:
 *
 *     host/mcu/eh_host_mcu_transport/src/eh_host_mcu_transport_channels.c:45
 *
 * eh_host_port_dma_alloc() is heap_caps_malloc(n, MALLOC_CAP_DMA), i.e. internal SRAM.
 * CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM, which sdkconfig.board sets, is read only by the
 * _aligned variant — it put the staging buffers and the TX aggregate in PSRAM and never
 * reached these copies. Up to CONFIG_ESP_HOSTED_HOST_SDIO_RX_Q_SIZE (20) received frames sit
 * in the queue at once, 1536 bytes each: ~33 KB of the memory the I2S descriptors, the SD
 * card and everything else asking for MALLOC_CAP_DMA also need. Measured during DLNA:
 * 30-33 KB free, sometimes 12.5 KB, once 2535 bytes.
 *
 * When the allocation fails, a received frame is skipped with no log line and no counter,
 * and an outgoing one (an ACK, a ping reply) is refused with ESP_ERR_NO_MEM, which lwIP
 * does not retry. That is the loss that grew with the TCP window on 17.09: the window is
 * the size of the burst, and the burst is how many of these copies exist at once.
 *
 * WHY PSRAM IS SAFE FOR THEM
 *
 * In SW_AGGR mode neither copy is ever a DMA target: the received one is copied again into
 * malloc() memory by sdio_process_rx_task, the outgoing one is memcpy'd into the TX
 * aggregate. The only paths that DO hand an eh_host_port_dma_alloc() buffer to the SDMMC
 * DMA are the per-packet ones (packet-mode RX, non-aggregated TX), which run only against a
 * co-processor that did not negotiate SW_AGGR; they transfer whole 512-byte blocks
 * (EH_HOST_PORT_SDIO_*_BLOCK_ONLY_XFER) of at most MAX_SDIO_BUFFER_SIZE = 1536, and the
 * allocation below is 64-byte aligned and rounded to 64 — the P4's PSRAM cache line, which
 * is what the SDMMC host checks (sdmmc_host_check_buffer_alignment). The request is the one
 * esp_hosted's own aligned allocator makes; IDF drops the DMA flag for a PSRAM request and
 * aligns to the cache line instead (esp_mm/heap_align_hw.c, "Workaround: the heap allocator
 * doesn't have regions marked MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM"). Both kinds of block are
 * freed by eh_host_port_dma_free() = heap_caps_free(), so the routing can change at any time.
 *
 * HOW
 *
 * -Wl,--wrap=eh_host_port_dma_alloc, the same link-time idiom components/esp_dlna uses for
 * httpd_register_uri_handler: managed_components stays untouched, and removing one Kconfig
 * line puts everything back. CMakeLists.txt checks the prototype in esp_hosted's header at
 * build time, because the wrap has to repeat it here — esp_hosted does not export that
 * header.
 */

#include "hosted_mem.h"

#include <stdatomic.h>
#include <stddef.h>

#include "sdkconfig.h"
#include "esp_heap_caps.h"

#if CONFIG_ESP_HOSTED_HOST

static _Atomic uint32_t s_frames_psram;
static _Atomic uint32_t s_frames_internal;
static _Atomic uint32_t s_frames_failed_rx;
static _Atomic uint32_t s_frames_failed_tx;
static _Atomic uint32_t s_heap_failed;
static _Atomic uint32_t s_heap_failed_dma;
static _Atomic uint32_t s_last_failed_size;
static _Atomic uint32_t s_last_failed_caps;

// Called by the heap for EVERY failed allocation in the firmware, from the allocating task.
// Counting only: no logging, no allocation, nothing that can block. main.cpp reports the
// counts from the main loop.
static void on_alloc_failed(size_t size, uint32_t caps, const char* function_name)
{
    (void)function_name;
    atomic_fetch_add_explicit(&s_heap_failed, 1, memory_order_relaxed);
    if (caps & MALLOC_CAP_DMA) {
        atomic_fetch_add_explicit(&s_heap_failed_dma, 1, memory_order_relaxed);
    }
    atomic_store_explicit(&s_last_failed_size, (uint32_t)size, memory_order_relaxed);
    atomic_store_explicit(&s_last_failed_caps, caps, memory_order_relaxed);
}

// The heap is up before global constructors run (heap_caps_init is called from the CPU start
// code, do_global_ctors after it). The component is WHOLE_ARCHIVE so this object is linked
// even on a build where nothing else in it is referenced.
__attribute__((constructor)) static void hosted_mem_init(void)
{
    heap_caps_register_failed_alloc_callback(on_alloc_failed);
}

#if CONFIG_HOSTED_MEM_FRAMES_IN_PSRAM

/// esp_hosted's own frame buffer size (MAX_SDIO_BUFFER_SIZE = ESP_TRANSPORT_SDIO_MAX_BUF_SIZE
/// in eh_common_interface.h). sdio_buffer_alloc() always asks for exactly this; the send
/// path asks for header + frame, which for an Ethernet frame is always less. That is how a
/// failure below is told apart as a received or an outgoing frame.
#define HOSTED_RX_FRAME_BYTES 1536u

/// The P4's PSRAM cache line: the alignment the SDMMC host demands of a PSRAM buffer.
#define PSRAM_LINE 64u

static _Atomic bool s_in_psram = true;

// The prototype repeats port/os/include/eh_host_port_dma.h, which esp_hosted does not put on
// its public include path; CMakeLists.txt refuses to build if that header's line changes.
void* __real_eh_host_port_dma_alloc(size_t n);
void* __wrap_eh_host_port_dma_alloc(size_t n);

void* __wrap_eh_host_port_dma_alloc(size_t n)
{
    if (atomic_load_explicit(&s_in_psram, memory_order_relaxed)) {
        void* p = heap_caps_aligned_alloc(PSRAM_LINE, n,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (p) {
            atomic_fetch_add_explicit(&s_frames_psram, 1, memory_order_relaxed);
            return p;
        }
    }
    // Routing off, or PSRAM refused: esp_hosted's own allocation, exactly as before.
    void* p = __real_eh_host_port_dma_alloc(n);
    if (p) {
        atomic_fetch_add_explicit(&s_frames_internal, 1, memory_order_relaxed);
        return p;
    }
    // esp_hosted is about to drop this frame. The heap callback counted the allocation; this
    // counts the frame, and which way it was going.
    if (n == HOSTED_RX_FRAME_BYTES) {
        atomic_fetch_add_explicit(&s_frames_failed_rx, 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&s_frames_failed_tx, 1, memory_order_relaxed);
    }
    return NULL;
}

void hosted_mem_set_frames_in_psram(bool on)
{
    atomic_store_explicit(&s_in_psram, on, memory_order_relaxed);
}

#else  // !CONFIG_HOSTED_MEM_FRAMES_IN_PSRAM

void hosted_mem_set_frames_in_psram(bool on) { (void)on; }

#endif

void hosted_mem_get_stats(hosted_mem_stats_t* out)
{
    if (!out) return;
#if CONFIG_HOSTED_MEM_FRAMES_IN_PSRAM
    out->active   = true;
    out->in_psram = atomic_load_explicit(&s_in_psram, memory_order_relaxed);
#else
    out->active   = false;
    out->in_psram = false;
#endif
    out->frames_psram      = atomic_load_explicit(&s_frames_psram, memory_order_relaxed);
    out->frames_internal   = atomic_load_explicit(&s_frames_internal, memory_order_relaxed);
    out->frames_failed_rx  = atomic_load_explicit(&s_frames_failed_rx, memory_order_relaxed);
    out->frames_failed_tx  = atomic_load_explicit(&s_frames_failed_tx, memory_order_relaxed);
    out->heap_failed       = atomic_load_explicit(&s_heap_failed, memory_order_relaxed);
    out->heap_failed_dma   = atomic_load_explicit(&s_heap_failed_dma, memory_order_relaxed);
    out->last_failed_size  = atomic_load_explicit(&s_last_failed_size, memory_order_relaxed);
    out->last_failed_caps  = atomic_load_explicit(&s_last_failed_caps, memory_order_relaxed);
}

#else  // !CONFIG_ESP_HOSTED_HOST — a board without the co-processor link

void hosted_mem_get_stats(hosted_mem_stats_t* out)
{
    if (!out) return;
    *out = (hosted_mem_stats_t){0};
}

void hosted_mem_set_frames_in_psram(bool on) { (void)on; }

#endif
