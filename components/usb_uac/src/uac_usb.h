/**
 * @file uac_usb.h
 * @brief The TinyUSB side of usb_uac: descriptors, class callbacks, the receive FIFO.
 *
 * Kept in C (uac_usb.c) so TinyUSB's headers never meet the C++ compiler; usb_uac.cpp talks to
 * it only through this header. The host writes the fields of `g_uac` from TinyUSB's task and
 * ISR on one core; the reader runs on the same core (usb_uac.cpp), so plain volatile 32-bit
 * fields are enough for the counters it polls.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile uint32_t rate;        ///< Hz, as the host last set the clock (44100 until it does)
    volatile uint8_t  alt;         ///< streaming interface: 0 idle, 1 16-bit, 2 24-in-32
    volatile uint8_t  suspended;   ///< bus suspended (also what an unplug looks like: no VBUS sense)
    volatile uint8_t  mute;        ///< feature unit, master channel
    volatile int16_t  volume;      ///< feature unit, master channel, 1/256 dB
    volatile uint32_t stream_gen;  ///< bumped on every alternate-setting change
    volatile uint32_t rx_bytes;    ///< payload bytes received, ever (ISR)

    // Packet timing, from the receive interrupt. The data endpoint carries one packet every
    // microframe, so two packets more than one microframe apart (DSTS frame number) are packets
    // the controller did not take — the endpoint was not re-armed in time.
    volatile uint32_t pkts;          ///< data packets received, ever
    volatile uint32_t lost;          ///< microframes skipped between two packets, ever
    volatile uint32_t loss_events;   ///< times `lost` grew
    volatile uint32_t same_frame;    ///< two packets in one microframe: the controller had held one
    volatile uint32_t gap_max_us;    ///< longest gap between two packets' interrupts; reader clears
    volatile uint32_t ev_head;       ///< loss events written to the ring below, ever
    volatile uint32_t ev_t_ms[16];   ///< esp_timer ms of each loss event
    volatile uint16_t ev_lost[16];   ///< microframes skipped
    volatile uint32_t ev_gap_us[16]; ///< interrupt gap that ended it
    volatile uint32_t ev_pc[16];     ///< mepc as the late interrupt was taken: where the core was
                                     ///< when it let the interrupt in — after whatever held it off
    volatile uint32_t ev_caller[16]; ///< when that was portEXIT_CRITICAL's epilogue: its caller
    char              ev_task[16][9];///< the task that was running, first 8 characters
} uac_shared_t;

#define UAC_EVENTS 16

extern uac_shared_t g_uac;

/// Product and serial strings, UTF-8 and ASCII; copied. Call before uac_usb_init().
void     uac_usb_set_strings(const char* product, const char* serial);
/// UTMI PHY in device mode, high speed, then TinyUSB on rhport 1. Call from the task that will
/// run uac_usb_task(): the controller's interrupt is allocated on the calling core.
bool     uac_usb_init(void);
/// TinyUSB's device task; blocks until there is an event.
void     uac_usb_task(void);
/// Configured by a host, as TinyUSB keeps it: a bus reset clears it, and calls back nothing.
bool     uac_usb_mounted(void);

/// Bytes per frame (both channels) of an alternate setting, 0 for alt 0.
uint8_t  uac_usb_frame_bytes(uint8_t alt);
uint16_t uac_usb_available(void);
uint16_t uac_usb_read(void* buf, uint16_t len);
uint16_t uac_usb_fifo_depth(void);
void     uac_usb_clear(void);

/// Feedback, in samples per microframe as 16.16 — what the host is asked to send.
uint32_t uac_usb_nominal_feedback(uint32_t rate);
void     uac_usb_set_feedback(uint32_t fb);

#ifdef __cplusplus
}
#endif
