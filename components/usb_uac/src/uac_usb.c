/**
 * @file uac_usb.c
 * @brief USB Audio Class 2 speaker: descriptors and TinyUSB class callbacks (see uac_usb.h).
 *
 * One audio function, two interfaces: control (clock → input terminal → feature unit → output
 * terminal) and streaming with three alternate settings — 0 is silence, 1 carries 16-bit PCM,
 * 2 carries 24 bits in a 4-byte subslot. Both formats run at any of the four rates the clock
 * lists; the host picks. The data endpoint is isochronous and asynchronous: the panel's clock
 * sets the pace, and the feedback endpoint tells the host how many frames to send (USB 2.0
 * §5.12.4.2), so nothing on the panel has to resample.
 *
 * Built after TinyUSB's examples/device/uac2_speaker_fb (MIT) for TinyUSB 0.21.
 */

#include <string.h>

#include "esp_private/usb_phy.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "riscv/rvruntime-frames.h"
#include "tusb.h"
#include "uac_usb.h"

uac_shared_t g_uac = {.rate = 44100};

// ── Descriptors ─────────────────────────────────────────────────────────────────────────────────

enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING,
    ITF_NUM_TOTAL
};

enum {
    STR_LANG = 0,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIAL,
};

#define UAC_ENTITY_INPUT_TERMINAL   0x01
#define UAC_ENTITY_FEATURE_UNIT     0x02
#define UAC_ENTITY_OUTPUT_TERMINAL  0x03
#define UAC_ENTITY_CLOCK            0x04

#define EPNUM_AUDIO_OUT             0x01
#define EPNUM_AUDIO_FB              0x81

#define UAC_CHANNEL_CONFIG  (AUDIO20_CHANNEL_CONFIG_FRONT_LEFT | AUDIO20_CHANNEL_CONFIG_FRONT_RIGHT)

#define UAC_EP_SZ_16  TUD_AUDIO_EP_SIZE(true, USB_UAC_MAX_RATE, 2, USB_UAC_CHANNELS)
#define UAC_EP_SZ_24  TUD_AUDIO_EP_SIZE(true, USB_UAC_MAX_RATE, 4, USB_UAC_CHANNELS)

#define UAC_AC_UNITS_LEN (TUD_AUDIO20_DESC_CLK_SRC_LEN + TUD_AUDIO20_DESC_INPUT_TERM_LEN \
                          + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN + TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(2))

#define UAC_AS_ALT_LEN (TUD_AUDIO20_DESC_STD_AS_LEN + TUD_AUDIO20_DESC_CS_AS_INT_LEN \
                        + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN + TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN \
                        + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN + TUD_AUDIO20_DESC_STD_AS_ISO_FB_EP_LEN)

#define UAC_FUNCTION_LEN (TUD_AUDIO20_DESC_IAD_LEN + TUD_AUDIO20_DESC_STD_AC_LEN \
                          + TUD_AUDIO20_DESC_CS_AC_LEN + UAC_AC_UNITS_LEN \
                          + TUD_AUDIO20_DESC_STD_AS_LEN + 2 * UAC_AS_ALT_LEN)

#define UAC_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + UAC_FUNCTION_LEN)

/// One streaming alternate setting: format, data endpoint, feedback endpoint.
/// Data every microframe (bInterval 1); feedback every 8 (bInterval 4 = 1 ms), as TinyUSB's HS example.
#define UAC_AS_ALT(_alt, _subslot, _bits, _epsize)                                                 \
    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, _alt, 0x02, 0x00),                        \
    TUD_AUDIO20_DESC_CS_AS_INT(UAC_ENTITY_INPUT_TERMINAL, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I,  \
                               AUDIO20_DATA_FORMAT_TYPE_I_PCM, USB_UAC_CHANNELS, UAC_CHANNEL_CONFIG, \
                               0x00),                                                              \
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(_subslot, _bits),                                               \
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_OUT,                                                \
                                   (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS                        \
                                             | (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS               \
                                             | (uint8_t)TUSB_ISO_EP_ATT_DATA),                     \
                                   _epsize, 0x01),                                                 \
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK,                \
                                  AUDIO20_CTRL_NONE, AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, \
                                  0x0001),                                                         \
    TUD_AUDIO20_DESC_STD_AS_ISO_FB_EP(EPNUM_AUDIO_FB, 4, 4)

static const tusb_desc_device_t s_desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    // An audio function with an IAD: class MISC, subclass COMMON, protocol IAD (USB IAD ECN).
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,   // Espressif; 0x8000 is espressif/usb_device_uac's default PID
    .idProduct          = 0x8000,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STR_MANUFACTURER,
    .iProduct           = STR_PRODUCT,
    .iSerialNumber      = STR_SERIAL,
    .bNumConfigurations = 0x01,
};

static const uint8_t s_desc_config[] = {
    // The panel has its own supply; 100 mA is what it may draw while a laptop powers the bench.
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, UAC_CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),

    TUD_AUDIO20_DESC_IAD(ITF_NUM_AUDIO_CONTROL, 0x02, 0x00),
    TUD_AUDIO20_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0x00, STR_PRODUCT),
    TUD_AUDIO20_DESC_CS_AC(0x0200, AUDIO20_FUNC_DESKTOP_SPEAKER, UAC_AC_UNITS_LEN, 0x00),
    TUD_AUDIO20_DESC_CLK_SRC(UAC_ENTITY_CLOCK, AUDIO20_CLOCK_SOURCE_ATT_INT_PRO_CLK,
                             (AUDIO20_CTRL_RW << AUDIO20_CLOCK_SOURCE_CTRL_CLK_FRQ_POS)
                                 | (AUDIO20_CTRL_R << AUDIO20_CLOCK_SOURCE_CTRL_CLK_VAL_POS),
                             UAC_ENTITY_INPUT_TERMINAL, 0x00),
    TUD_AUDIO20_DESC_INPUT_TERM(UAC_ENTITY_INPUT_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING, 0x00,
                                UAC_ENTITY_CLOCK, USB_UAC_CHANNELS, UAC_CHANNEL_CONFIG, 0x00, 0x0000, 0x00),
    TUD_AUDIO20_DESC_OUTPUT_TERM(UAC_ENTITY_OUTPUT_TERMINAL, AUDIO_TERM_TYPE_OUT_GENERIC_SPEAKER,
                                 UAC_ENTITY_INPUT_TERMINAL, UAC_ENTITY_FEATURE_UNIT, UAC_ENTITY_CLOCK,
                                 0x0000, 0x00),
    // Mute and volume on the master channel only.
    TUD_AUDIO20_DESC_FEATURE_UNIT(UAC_ENTITY_FEATURE_UNIT, UAC_ENTITY_INPUT_TERMINAL, 0x00,
                                  (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS)
                                      | (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS),
                                  0x00000000, 0x00000000),

    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0x00, 0x00, 0x00),
    UAC_AS_ALT(0x01, 2, 16, UAC_EP_SZ_16),
    UAC_AS_ALT(0x02, 4, 24, UAC_EP_SZ_24),
};
TU_VERIFY_STATIC(sizeof(s_desc_config) == UAC_CONFIG_TOTAL_LEN, "UAC2 descriptor length");

static const tusb_desc_device_qualifier_t s_desc_qualifier = {
    .bLength            = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType    = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved          = 0x00,
};

/// At full speed the panel offers no audio: a configuration with no interfaces. A full-speed-only
/// host still enumerates it and simply finds nothing to play to.
static const uint8_t s_desc_other_speed[] = {
    9, TUSB_DESC_OTHER_SPEED_CONFIG, U16_TO_U8S_LE(9), 0x00, 0x01, 0x00,
    TU_BIT(7) | TUSB_DESC_CONFIG_ATT_SELF_POWERED, 50,
};

uint8_t const* tud_descriptor_device_cb(void) { return (uint8_t const*)&s_desc_device; }
uint8_t const* tud_descriptor_configuration_cb(uint8_t index) { (void)index; return s_desc_config; }
uint8_t const* tud_descriptor_device_qualifier_cb(void) { return (uint8_t const*)&s_desc_qualifier; }
uint8_t const* tud_descriptor_other_speed_configuration_cb(uint8_t index) {
    (void)index;
    return s_desc_other_speed;
}

// ── Strings ─────────────────────────────────────────────────────────────────────────────────────

#define STR_MAX 32
static char     s_product[STR_MAX * 4 + 1] = "P4 Player";
static char     s_serial[STR_MAX + 1]      = "000000000000";
static uint16_t s_str_buf[STR_MAX + 1];

void uac_usb_set_strings(const char* product, const char* serial) {
    if (product && product[0]) strlcpy(s_product, product, sizeof(s_product));
    if (serial && serial[0])   strlcpy(s_serial, serial, sizeof(s_serial));
}

/// UTF-8 to UTF-16 for the Basic Multilingual Plane — the name may be Cyrillic. A code point
/// outside it, or a broken sequence, becomes '?'.
static uint8_t utf8_to_utf16(const char* s, uint16_t* out, uint8_t cap) {
    const uint8_t* p = (const uint8_t*)s;
    uint8_t n = 0;
    while (*p && n < cap) {
        uint32_t cp;
        int more;
        if (*p < 0x80)                { cp = *p++; more = 0; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p++ & 0x1F; more = 1; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p++ & 0x0F; more = 2; }
        else                          { p++; out[n++] = '?'; while ((*p & 0xC0) == 0x80) p++; continue; }
        for (; more > 0; more--) {
            if ((*p & 0xC0) != 0x80) { cp = '?'; break; }
            cp = (cp << 6) | (*p++ & 0x3F);
        }
        out[n++] = (uint16_t)cp;
    }
    return n;
}

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t n;
    switch (index) {
        case STR_LANG:         s_str_buf[1] = 0x0409; n = 1; break;
        case STR_MANUFACTURER: n = utf8_to_utf16("ModESP", &s_str_buf[1], STR_MAX); break;
        case STR_PRODUCT:      n = utf8_to_utf16(s_product, &s_str_buf[1], STR_MAX); break;
        case STR_SERIAL:       n = utf8_to_utf16(s_serial, &s_str_buf[1], STR_MAX); break;
        default:               return NULL;
    }
    s_str_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return s_str_buf;
}

// ── Device state ────────────────────────────────────────────────────────────────────────────────

// Packet timing (tud_audio_rx_done_isr): the previous packet's microframe and time.
static uint32_t s_prev_fn;
static int64_t  s_prev_us;
static bool     s_prev_valid;

/// A configured panel starts with every interface at alternate setting 0, TinyUSB's own state
/// included. Reset here because nothing else does after a bus reset — a cable moved to another
/// computer, a reset-resume: TinyUSB calls back nothing for it, and with VBUS tied (below) no
/// unplug ever comes. Kept, the old setting played a stream that was not there.
void tud_mount_cb(void)   { g_uac.suspended = 0; g_uac.alt = 0; g_uac.stream_gen++; }
void tud_umount_cb(void)  { g_uac.alt = 0; g_uac.stream_gen++; }
/// Also what pulling the cable looks like on this board: VBUS stays up through the other port
/// (docs/CONCEPT.md, "Tied VBUS"), so the controller never sees a disconnect — only the bus
/// going quiet.
void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    g_uac.suspended = 1;
    s_prev_valid = false;   // no SOFs while suspended: the next packet is not a loss
}
void tud_resume_cb(void)  { g_uac.suspended = 0; }

// ── Clock source and feature unit ───────────────────────────────────────────────────────────────

static const uint32_t s_rates[] = {44100, 48000, 88200, 96000};
#define N_RATES TU_ARRAY_SIZE(s_rates)

/// Host volume range: −80 dB to 0 dB in 0.5 dB steps, in 1/256 dB.
#define VOL_MIN  (-80 * 256)
#define VOL_MAX  0
#define VOL_RES  128

static bool clock_get(uint8_t rhport, tusb_control_request_t const* req) {
    uint8_t const sel = TU_U16_HIGH(req->wValue);
    if (sel == AUDIO20_CS_CTRL_SAM_FREQ) {
        if (req->bRequest == AUDIO20_CS_REQ_CUR) {
            audio20_control_cur_4_t cur = {.bCur = (int32_t)tu_htole32(g_uac.rate)};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &cur, sizeof(cur));
        }
        if (req->bRequest == AUDIO20_CS_REQ_RANGE) {
            audio20_control_range_4_n_t(N_RATES) range = {.wNumSubRanges = tu_htole16(N_RATES)};
            for (uint8_t i = 0; i < N_RATES; i++) {
                range.subrange[i].bMin = (int32_t)s_rates[i];
                range.subrange[i].bMax = (int32_t)s_rates[i];
                range.subrange[i].bRes = 0;
            }
            return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &range, sizeof(range));
        }
    } else if (sel == AUDIO20_CS_CTRL_CLK_VALID && req->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_1_t valid = {.bCur = 1};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &valid, sizeof(valid));
    }
    return false;
}

static bool clock_set(tusb_control_request_t const* req, uint8_t const* buf) {
    TU_VERIFY(req->bRequest == AUDIO20_CS_REQ_CUR);
    TU_VERIFY(TU_U16_HIGH(req->wValue) == AUDIO20_CS_CTRL_SAM_FREQ);
    TU_VERIFY(req->wLength == sizeof(audio20_control_cur_4_t));
    uint32_t const rate = (uint32_t)((audio20_control_cur_4_t const*)buf)->bCur;
    for (uint8_t i = 0; i < N_RATES; i++) {
        if (s_rates[i] == rate) {
            g_uac.rate = rate;
            // A host may set the clock while it streams; until the producer catches up, the
            // nominal value for the new rate, not the old rate's.
            tud_audio_fb_set(uac_usb_nominal_feedback(rate));
            return true;
        }
    }
    return false;   // not a rate we listed: stall
}

static bool feature_get(uint8_t rhport, tusb_control_request_t const* req) {
    uint8_t const sel = TU_U16_HIGH(req->wValue);
    TU_VERIFY(TU_U16_LOW(req->wValue) == 0);   // master channel only
    if (sel == AUDIO20_FU_CTRL_MUTE && req->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_1_t mute = {.bCur = g_uac.mute};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &mute, sizeof(mute));
    }
    if (sel == AUDIO20_FU_CTRL_VOLUME) {
        if (req->bRequest == AUDIO20_CS_REQ_RANGE) {
            audio20_control_range_2_n_t(1) range = {.wNumSubRanges = tu_htole16(1)};
            range.subrange[0].bMin = (int16_t)tu_htole16(VOL_MIN);
            range.subrange[0].bMax = (int16_t)tu_htole16(VOL_MAX);
            range.subrange[0].bRes = (int16_t)tu_htole16(VOL_RES);
            return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &range, sizeof(range));
        }
        if (req->bRequest == AUDIO20_CS_REQ_CUR) {
            audio20_control_cur_2_t cur = {.bCur = (int16_t)tu_htole16(g_uac.volume)};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &cur, sizeof(cur));
        }
    }
    return false;
}

static bool feature_set(tusb_control_request_t const* req, uint8_t const* buf) {
    uint8_t const sel = TU_U16_HIGH(req->wValue);
    TU_VERIFY(TU_U16_LOW(req->wValue) == 0);
    TU_VERIFY(req->bRequest == AUDIO20_CS_REQ_CUR);
    if (sel == AUDIO20_FU_CTRL_MUTE) {
        TU_VERIFY(req->wLength == sizeof(audio20_control_cur_1_t));
        g_uac.mute = ((audio20_control_cur_1_t const*)buf)->bCur;
        return true;
    }
    if (sel == AUDIO20_FU_CTRL_VOLUME) {
        TU_VERIFY(req->wLength == sizeof(audio20_control_cur_2_t));
        g_uac.volume = ((audio20_control_cur_2_t const*)buf)->bCur;
        return true;
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const* req) {
    uint8_t const entity = TU_U16_HIGH(req->wIndex);
    if (entity == UAC_ENTITY_CLOCK)        return clock_get(rhport, req);
    if (entity == UAC_ENTITY_FEATURE_UNIT) return feature_get(rhport, req);
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const* req, uint8_t* buf) {
    (void)rhport;
    uint8_t const entity = TU_U16_HIGH(req->wIndex);
    if (entity == UAC_ENTITY_CLOCK)        return clock_set(req, buf);
    if (entity == UAC_ENTITY_FEATURE_UNIT) return feature_set(req, buf);
    return false;
}

// ── Streaming ───────────────────────────────────────────────────────────────────────────────────

/// DSTS of the P4's USB-DWC2.0 (TinyUSB rhport 1, base 0x50000000 in dwc2_esp32.h); SOFFN, the
/// (micro)frame number, is bits 21:8 — at high speed it counts microframes.
#define DWC2_HS_DSTS (*(volatile uint32_t*)(0x50000000UL + 0x808))

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const* req) {
    (void)rhport;
    if (tu_u16_low(req->wIndex) == ITF_NUM_AUDIO_STREAMING) {
        g_uac.alt = (uint8_t)tu_u16_low(req->wValue);
        g_uac.stream_gen++;
        s_prev_valid = false;
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const* req) {
    (void)rhport;
    if (tu_u16_low(req->wIndex) == ITF_NUM_AUDIO_STREAMING) {
        g_uac.alt = 0;
        g_uac.stream_gen++;
        s_prev_valid = false;
    }
    return true;
}

bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out,
                           uint8_t cur_alt_setting) {
    (void)rhport; (void)func_id; (void)ep_out; (void)cur_alt_setting;
    const uint32_t fn  = (DWC2_HS_DSTS >> 8) & 0x3FFF;
    const int64_t  now = esp_timer_get_time();
    if (s_prev_valid) {
        const uint32_t d   = (fn - s_prev_fn) & 0x3FFF;
        const uint32_t gap = (uint32_t)(now - s_prev_us);
        if (gap > g_uac.gap_max_us) g_uac.gap_max_us = gap;
        if (d == 0) {
            g_uac.same_frame++;
        } else if (d > 1 && d < 0x2000) {
            const uint32_t i = g_uac.ev_head % UAC_EVENTS;
            uint32_t pc;
            __asm__ volatile("csrr %0, mepc" : "=r"(pc));
            g_uac.ev_t_ms[i]   = (uint32_t)(now / 1000);
            g_uac.ev_lost[i]   = (uint16_t)(d - 1);
            g_uac.ev_gap_us[i] = gap;
            g_uac.ev_pc[i]     = pc;
            // Who held the interrupt off: the interrupted task's frame sits at its TCB's
            // pxTopOfStack (portasm.S, rtos_int_enter). If it was leaving a critical section,
            // vPortExitCriticalMultiCore's own frame still holds the return address into its
            // caller at sp + 12 (the epilogue's "lw ra,12(sp)" is where mepc points).
            TaskHandle_t t = xTaskGetCurrentTaskHandleForCore(xPortGetCoreID());
            g_uac.ev_caller[i] = 0;
            g_uac.ev_task[i][0] = '\0';
            if (t) {
                const RvExcFrame* f = *(RvExcFrame* const*)t;
                g_uac.ev_caller[i] = *(const uint32_t*)(f->sp + 12);
                const char* name = pcTaskGetName(t);
                for (int k = 0; k < 8 && name[k]; k++) g_uac.ev_task[i][k] = name[k], g_uac.ev_task[i][k + 1] = '\0';
            }
            g_uac.ev_head++;
            g_uac.lost += d - 1;
            g_uac.loss_events++;
        }
    }
    s_prev_fn    = fn;
    s_prev_us    = now;
    s_prev_valid = true;
    g_uac.pkts++;
    g_uac.rx_bytes += n_bytes_received;
    return true;
}

/// Feedback is set by hand (uac_usb_set_feedback): the producer in usb_uac.cpp drains this FIFO
/// into the player's ring as fast as it fills, so the fill that tells how far the host is from
/// the panel's clock is the ring's, not this FIFO's. Until the producer says otherwise the host
/// is told the nominal rate — also what it is told while USB is not the source.
void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t* p) {
    (void)alt_itf;
    p->method      = AUDIO_FEEDBACK_METHOD_DISABLED;
    p->sample_freq = g_uac.rate;
    tud_audio_n_fb_set(func_id, uac_usb_nominal_feedback(g_uac.rate));
}

uint32_t uac_usb_nominal_feedback(uint32_t rate) {
    // Samples per microframe in 16.16 (USB 2.0 §5.12.4.2, high speed).
    return (uint32_t)(((uint64_t)rate << 16) / 8000u);
}

void uac_usb_set_feedback(uint32_t fb) { tud_audio_fb_set(fb); }

// ── API for usb_uac.cpp ─────────────────────────────────────────────────────────────────────────

static usb_phy_handle_t s_phy;

bool uac_usb_init(void) {
    // ESP32-P4: USB-DWC2.0 sits on the UTMI PHY, which is high speed only (usb_phy.c).
    usb_phy_config_t phy = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_UTMI,
        .otg_mode   = USB_OTG_MODE_DEVICE,
        .otg_speed  = USB_PHY_SPEED_HIGH,
    };
    if (usb_new_phy(&phy, &s_phy) != ESP_OK) return false;

    tusb_rhport_init_t const dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_HIGH};
    return tusb_init(1, &dev_init);
}

void uac_usb_task(void) { tud_task(); }

bool uac_usb_mounted(void) { return tud_mounted(); }

uint8_t uac_usb_frame_bytes(uint8_t alt) {
    return alt == 1 ? 2 * USB_UAC_CHANNELS : alt == 2 ? 4 * USB_UAC_CHANNELS : 0;
}

uint16_t uac_usb_available(void) { return tud_audio_available(); }
uint16_t uac_usb_read(void* buf, uint16_t len) { return tud_audio_read(buf, len); }
void     uac_usb_clear(void) { tud_audio_clear_ep_out_ff(); }

uint16_t uac_usb_fifo_depth(void) {
    tu_fifo_t* ff = tud_audio_get_ep_out_ff();
    return ff ? tu_fifo_depth(ff) : 0;
}
