/**
 * @file tusb_config.h
 * @brief TinyUSB configuration for the panel's USB Audio Class 2 speaker (components/usb_uac).
 *
 * CMakeLists.txt puts this directory on the tinyusb component's include path, so TinyUSB's own
 * sources are compiled against it — the pattern espressif/usb_device_uac uses. One function:
 * a stereo speaker with an asynchronous feedback endpoint, high-speed only.
 *
 * The P4 has two OTG controllers: rhport 0 is USB-DWC1.1 (full speed, FSLS PHY), rhport 1 is
 * USB-DWC2.0 on the UTMI PHY (tusb_mcu.h, OPT_MCU_ESP32P4). The panel's HS Type-C is the second.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ── Port ────────────────────────────────────────────────────────────────────────────────────────
#define CFG_TUSB_RHPORT1_MODE   (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#define CFG_TUD_MAX_SPEED       OPT_MODE_HIGH_SPEED
#define CFG_TUSB_OS             OPT_OS_FREERTOS
#define CFG_TUSB_DEBUG          0

// Slave mode (the default): the core copies packets out of the controller's FIFO in the ISR,
// straight into the audio FIFO (CFG_TUD_EDPT_DEDICATED_HWFIFO). No DMA, so no cache maintenance
// and no DMA-capable buffers — internal DMA SRAM is the panel's scarcest resource
// (docs/CONCEPT.md, "Board constraints").
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))

#define CFG_TUD_ENABLED         1
#define CFG_TUD_ENDPOINT0_SIZE  64

// ── Audio function ──────────────────────────────────────────────────────────────────────────────
#define CFG_TUD_AUDIO                       1
#define CFG_TUD_AUDIO_ENABLE_EP_OUT         1
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP    1

/// The ceiling the audio path holds (96 kHz, docs/CONCEPT.md, "The output clock follows the
/// source"), in the widest slot offered: 24 bits in a 4-byte subslot. 13 frames × 8 bytes =
/// 104 bytes per microframe.
#define USB_UAC_MAX_RATE                    96000
#define USB_UAC_CHANNELS                    2
#define USB_UAC_MAX_SUBSLOT                 4

#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX \
    TUD_AUDIO_EP_SIZE(true, USB_UAC_MAX_RATE, USB_UAC_MAX_SUBSLOT, USB_UAC_CHANNELS)
/// 32 microframes of the widest format: 3.3 KB, ~4.3 ms at 96 kHz/24-bit. The producer empties it
/// every millisecond into the player's ring, which is the elastic buffer (usb_uac.cpp); this only
/// has to cover the producer's wake-up. Internal RAM: it was 64 (6.6 KB) while TinyUSB's own
/// FIFO-count feedback needed it half full.
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ   (32 * CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX)

#ifdef __cplusplus
}
#endif
