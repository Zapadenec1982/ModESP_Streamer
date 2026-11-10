/**
 * @file audio_tags.cpp
 * @brief ID3v2, Vorbis comments and RIFF INFO — see audio_tags.h.
 *
 * Three formats, one shape: find a block of metadata near the start of the file, walk its
 * entries, and copy the four that matter. The awkward part is not the walking, it is the
 * text: ID3v2 alone has four encodings and two of them are UTF-16, so every string goes
 * through a converter on the way out. Everything this writes is UTF-8.
 *
 * The parser never trusts a length it reads. Every one of them comes from the file and is
 * checked against what is left of the block before it is used, because a truncated download
 * is a perfectly ordinary thing to find on a memory card.
 */

#include "audio_tags.h"

#include <cstring>
#include <cstdlib>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace audio_tags {
namespace {

/// The parser's scratch block, allocated once and kept.
///
/// On the device it comes from PSRAM. As a plain static it was 16 KB of internal .bss held
/// for the life of the firmware in order to parse a tag once per track, and internal .bss on
/// this board is the memory the SDIO co-processor's receive pool and the I2S DMA descriptors
/// are left to share — a DLNA stream was measured leaving 2535 bytes of it free. Only the
/// player task reads tags and nothing here is reachable from an ISR, so PSRAM is the right
/// place for it; a task that touches PSRAM during a flash write is suspended, not faulted.
///
/// Behind ESP_PLATFORM because this component is pure by design: the host suite builds it
/// with no ESP-IDF at all, and there it stays an ordinary static.
uint8_t* scratch(uint32_t bytes) {
#ifdef ESP_PLATFORM
    static uint8_t* p = (uint8_t*)heap_caps_calloc_prefer(
        1, bytes, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
#else
    static uint8_t* p = (uint8_t*)calloc(1, bytes);
    return p;
#endif
}

/// The largest metadata block worth reading. ID3v2 with several covers reaches a megabyte;
/// the text in it is in the first few kilobytes, and the artwork is located, not copied.
constexpr uint32_t MAX_BLOCK = 1u << 20;
/// One frame's text. Longer than any title, short enough to sit on the stack.
constexpr size_t   MAX_TEXT_BYTES = 512;

uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
uint32_t be24(const uint8_t* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}
uint32_t le32(const uint8_t* p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}
/// ID3's "synchsafe" integer: seven bits per byte, so the value can never contain 0xFF.
uint32_t syncsafe(const uint8_t* p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7)  | (uint32_t)(p[3] & 0x7F);
}

/// Append one code point as UTF-8. Returns false when the output is full, and writes
/// nothing in that case — a half-written character is worse than a short string.
bool put_utf8(char* out, size_t cap, size_t& len, uint32_t cp) {
    char b[4];
    size_t n;
    if (cp < 0x80)          { n = 1; b[0] = (char)cp; }
    else if (cp < 0x800)    { n = 2; b[0] = (char)(0xC0 | (cp >> 6));  b[1] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000)  { n = 3; b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                     b[2] = (char)(0x80 | (cp & 0x3F)); }
    else                    { n = 4; b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                                     b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); }
    if (len + n + 1 > cap) return false;
    memcpy(out + len, b, n);
    len += n;
    return true;
}

/// Copy already-UTF-8 text, stopping on a partial character rather than in the middle of
/// one: a field cut mid-sequence draws as a replacement glyph for the rest of its life.
void copy_utf8(const uint8_t* in, size_t n, char* out, size_t cap) {
    size_t len = 0;
    for (size_t i = 0; i < n;) {
        const uint8_t c = in[i];
        if (c == 0) break;
        size_t seq;
        if      ((c & 0x80) == 0x00) seq = 1;
        else if ((c & 0xE0) == 0xC0) seq = 2;
        else if ((c & 0xF0) == 0xE0) seq = 3;
        else if ((c & 0xF8) == 0xF0) seq = 4;
        else { i++; continue; }                       // stray continuation byte
        if (i + seq > n || len + seq + 1 > cap) break;
        memcpy(out + len, in + i, seq);
        len += seq;
        i += seq;
    }
    out[len] = '\0';
}

void copy_latin1(const uint8_t* in, size_t n, char* out, size_t cap) {
    size_t len = 0;
    for (size_t i = 0; i < n && in[i]; i++) {
        if (!put_utf8(out, cap, len, in[i])) break;
    }
    out[len] = '\0';
}

/// UTF-16 to UTF-8, surrogate pairs included. `be` picks the byte order when there is no BOM.
void copy_utf16(const uint8_t* in, size_t n, bool be, char* out, size_t cap) {
    size_t i = 0;
    if (n >= 2) {
        if (in[0] == 0xFF && in[1] == 0xFE)      { be = false; i = 2; }
        else if (in[0] == 0xFE && in[1] == 0xFF) { be = true;  i = 2; }
    }
    size_t len = 0;
    while (i + 1 < n) {
        uint32_t u = be ? ((uint32_t)in[i] << 8 | in[i + 1]) : ((uint32_t)in[i + 1] << 8 | in[i]);
        i += 2;
        if (u == 0) break;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n) {
            const uint32_t lo = be ? ((uint32_t)in[i] << 8 | in[i + 1])
                                   : ((uint32_t)in[i + 1] << 8 | in[i]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (!put_utf8(out, cap, len, u)) break;
    }
    out[len] = '\0';
}

/// One ID3v2 text frame body: an encoding byte, then the text.
void id3_text(const uint8_t* body, size_t n, char* out, size_t cap) {
    out[0] = '\0';
    if (n < 2) return;
    const uint8_t enc = body[0];
    const uint8_t* p  = body + 1;
    const size_t   m  = n - 1;
    switch (enc) {
    case 0:  copy_latin1(p, m, out, cap); break;
    case 1:  copy_utf16(p, m, false, out, cap); break;   // the BOM decides; LE if there is none
    case 2:  copy_utf16(p, m, true,  out, cap); break;
    default: copy_utf8(p, m, out, cap); break;
    }
}

uint16_t parse_u16(const char* s) {
    uint32_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        v = v * 10 + (uint32_t)(*s - '0');
        if (v > 65535) return 0;
    }
    return (uint16_t)v;
}

PicMime mime_of(const char* s) {
    // Both the full type and the bare extension appear in the wild; v2.2 writes "JPG".
    if (strstr(s, "jpeg") || strstr(s, "jpg") || strstr(s, "JPEG") || strstr(s, "JPG")) return PicMime::JPEG;
    if (strstr(s, "png")  || strstr(s, "PNG")) return PicMime::PNG;
    return PicMime::OTHER;
}

// ── ID3v2, the tag on almost every MP3 ────────────────────────────────────────────────────

/// Walks the frames by READING them, one at a time, rather than pulling the tag into a
/// buffer first.
///
/// A tag with artwork in it is hundreds of kilobytes, and buffering it was buffering the
/// artwork — which this deliberately does not copy. Worse, a buffer smaller than the tag
/// made the walk stop at the first frame that did not fit, which is exactly the APIC frame
/// the picture lives in: every embedded cover was missed, and the text before it read fine,
/// so nothing looked wrong. Reading frame by frame has no size limit at all.
bool read_id3v2(const Reader& r, const uint8_t* hdr, Tags& out) {
    const uint8_t  ver   = hdr[3];
    const uint8_t  flags = hdr[5];
    const uint32_t size  = syncsafe(hdr + 6);
    // No limit on the tag's size: the walk reads a frame header and a little text at a time,
    // and a tag is large only because of its picture. The frame count bounds the walk instead.
    if (ver < 2 || ver > 4 || size == 0) return false;

    const bool     v22     = (ver == 2);
    const uint32_t hdr_len = v22 ? 6 : 10;
    const size_t   id_len  = v22 ? 3 : 4;
    constexpr uint64_t TAG_AT = 10;              // the frames start after the tag header

    uint32_t pos = 0;
    if (flags & 0x40) {
        // Extended header. v3 gives its size excluding itself, v4 including itself and
        // synchsafe. Both are skipped whole; nothing in there is a tag.
        uint8_t eh[4];
        if (r.read(r.ctx, TAG_AT, eh, 4) != 4) return false;
        const uint32_t ext = (ver == 4) ? syncsafe(eh) : be32(eh) + 4;
        if (ext >= size) return false;
        pos = ext;
    }

    uint8_t fh[10];
    for (int frames = 0; frames < 256 && pos + hdr_len <= size; frames++) {
        if (r.read(r.ctx, TAG_AT + pos, fh, hdr_len) != hdr_len) break;
        if (fh[0] == 0) break;                   // padding: the rest of the tag is zeros
        uint32_t fsz;
        if (v22)           fsz = be24(fh + 3);
        else if (ver == 4) fsz = syncsafe(fh + 4);
        else               fsz = be32(fh + 4);
        const uint32_t body = pos + hdr_len;
        if (fsz == 0 || fsz > size - body) break;

        char id[5] = {};
        memcpy(id, fh, id_len);
        const uint64_t at = TAG_AT + body;

        if (v22 ? !strcmp(id, "PIC") : !strcmp(id, "APIC")) {
            if (!out.has_pic()) {
                // encoding, mime, picture type, description, then the image. Only the part
                // before the image is read — long enough for any sane description.
                uint8_t head[192];
                const size_t want = fsz < sizeof(head) ? fsz : sizeof(head);
                const size_t got  = r.read(r.ctx, at, head, want);
                size_t i = 0;
                char   mime[32] = {};
                if (got >= 4) {
                    const uint8_t enc = head[i++];
                    if (v22) {
                        memcpy(mime, head + i, 3);      // "JPG" / "PNG", fixed width
                        i += 3;
                    } else {
                        size_t k = 0;
                        while (i < got && head[i]) {
                            if (k + 1 < sizeof(mime)) mime[k++] = (char)head[i];
                            i++;
                        }
                        i++;                            // the terminator
                    }
                    i++;                                // picture type (front cover, back, …)
                    // The description ends in one NUL, or two when the text is UTF-16.
                    if (enc == 1 || enc == 2) {
                        while (i + 1 < got && !(head[i] == 0 && head[i + 1] == 0)) i += 2;
                        i += 2;
                    } else {
                        while (i < got && head[i]) i++;
                        i++;
                    }
                    if (i < fsz) {
                        out.pic_off  = at + i;
                        out.pic_len  = (uint32_t)(fsz - i);
                        out.pic_mime = mime_of(mime);
                    }
                }
            }
        } else if (id[0] == 'T' && fsz <= MAX_TEXT_BYTES) {
            // Only the text frames are worth reading, and only while they are text-sized:
            // a lyrics frame the size of a page is not a title.
            uint8_t text[MAX_TEXT_BYTES];
            const size_t got = r.read(r.ctx, at, text, fsz);
            char tmp[MAX_TEXT_BYTES];
            if      (!strcmp(id, v22 ? "TT2" : "TIT2")) id3_text(text, got, out.title,  TEXT_LEN);
            else if (!strcmp(id, v22 ? "TP1" : "TPE1")) id3_text(text, got, out.artist, TEXT_LEN);
            else if (!strcmp(id, v22 ? "TAL" : "TALB")) id3_text(text, got, out.album,  TEXT_LEN);
            else if (!strcmp(id, v22 ? "TYE" : "TYER") || !strcmp(id, "TDRC")) {
                id3_text(text, got, tmp, sizeof(tmp));
                out.year = parse_u16(tmp);
            } else if (!strcmp(id, v22 ? "TRK" : "TRCK")) {
                id3_text(text, got, tmp, sizeof(tmp));
                out.track_no = parse_u16(tmp);   // "5/12" stops at the slash
            }
        }
        pos = body + fsz;
    }
    return out.has_text() || out.has_pic();
}

// ── Vorbis comments, as FLAC carries them ─────────────────────────────────────────────────

/// "TITLE=…" — the key is case-insensitive by the specification.
bool key_is(const uint8_t* p, size_t n, const char* key, size_t& val_at) {
    const size_t k = strlen(key);
    if (n < k + 1) return false;
    for (size_t i = 0; i < k; i++) {
        uint8_t c = p[i];
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 32);
        if (c != (uint8_t)key[i]) return false;
    }
    if (p[k] != '=') return false;
    val_at = k + 1;
    return true;
}

void vorbis_comments(const uint8_t* b, size_t n, Tags& out) {
    if (n < 8) return;
    const uint32_t vendor = le32(b);
    if (vendor > n - 4) return;
    size_t pos = 4 + vendor;
    if (pos + 4 > n) return;
    uint32_t count = le32(b + pos);
    pos += 4;
    if (count > 256) count = 256;                      // a sane bound; the rest is not text
    for (uint32_t c = 0; c < count && pos + 4 <= n; c++) {
        const uint32_t len = le32(b + pos);
        pos += 4;
        if (len > n - pos) break;
        const uint8_t* e = b + pos;
        size_t v = 0;
        char num[MAX_TEXT_BYTES];
        if      (key_is(e, len, "TITLE", v))       copy_utf8(e + v, len - v, out.title,  TEXT_LEN);
        else if (key_is(e, len, "ARTIST", v))      copy_utf8(e + v, len - v, out.artist, TEXT_LEN);
        else if (key_is(e, len, "ALBUM", v))       copy_utf8(e + v, len - v, out.album,  TEXT_LEN);
        else if (key_is(e, len, "DATE", v))        { copy_utf8(e + v, len - v, num, sizeof(num)); out.year = parse_u16(num); }
        else if (key_is(e, len, "TRACKNUMBER", v)) { copy_utf8(e + v, len - v, num, sizeof(num)); out.track_no = parse_u16(num); }
        pos += len;
    }
}

bool read_flac(const Reader& r, uint8_t* buf, uint32_t cap, Tags& out) {
    uint64_t pos = 4;                                  // past "fLaC"
    for (int guard = 0; guard < 64; guard++) {
        uint8_t h[4];
        if (r.read(r.ctx, pos, h, 4) != 4) break;
        const bool     last = (h[0] & 0x80) != 0;
        const uint8_t  type = h[0] & 0x7F;
        const uint32_t len  = be24(h + 1);
        const uint64_t body = pos + 4;
        if (len > MAX_BLOCK) break;

        if (type == 4 && len <= cap) {                 // VORBIS_COMMENT
            const size_t got = r.read(r.ctx, body, buf, len);
            vorbis_comments(buf, got, out);
        } else if (type == 6 && !out.has_pic()) {      // PICTURE
            // Only the header is read: the mime type and the two lengths before the image
            // say where it starts, and the image itself can be megabytes.
            uint8_t ph[64];
            const size_t got = r.read(r.ctx, body, ph, sizeof(ph));
            if (got >= 12) {
                const uint32_t mlen = be32(ph + 4);
                if (mlen < sizeof(ph) - 12) {
                    char mime[32] = {};
                    const size_t mc = mlen < sizeof(mime) - 1 ? mlen : sizeof(mime) - 1;
                    memcpy(mime, ph + 8, mc);
                    uint64_t at = body + 8 + mlen;     // description length is next
                    uint8_t  dl[4];
                    if (r.read(r.ctx, at, dl, 4) == 4) {
                        at += 4 + be32(dl) + 16;       // description, width, height, depth, colours
                        uint8_t il[4];
                        if (r.read(r.ctx, at, il, 4) == 4) {
                            out.pic_len  = be32(il);
                            out.pic_off  = at + 4;
                            out.pic_mime = out.pic_len ? mime_of(mime) : PicMime::NONE;
                        }
                    }
                }
            }
        }
        if (last) break;
        pos = body + len;
        if (pos >= r.size) break;
    }
    return out.has_text() || out.has_pic();
}

// ── RIFF INFO, which is what a tagged WAV carries ─────────────────────────────────────────

bool read_riff(const Reader& r, uint8_t* buf, uint32_t cap, Tags& out) {
    uint64_t pos = 12;                                 // past "RIFF" size "WAVE"
    for (int guard = 0; guard < 64 && pos + 8 <= r.size; guard++) {
        uint8_t h[8];
        if (r.read(r.ctx, pos, h, 8) != 8) break;
        const uint32_t len = le32(h + 4);
        if (!memcmp(h, "LIST", 4) && len >= 4 && len <= cap) {
            const size_t got = r.read(r.ctx, pos + 8, buf, len);
            if (got >= 4 && !memcmp(buf, "INFO", 4)) {
                size_t i = 4;
                while (i + 8 <= got) {
                    const uint32_t sl = le32(buf + i + 4);
                    const size_t   at = i + 8;
                    if (sl > got - at) break;
                    if      (!memcmp(buf + i, "INAM", 4)) copy_latin1(buf + at, sl, out.title,  TEXT_LEN);
                    else if (!memcmp(buf + i, "IART", 4)) copy_latin1(buf + at, sl, out.artist, TEXT_LEN);
                    else if (!memcmp(buf + i, "IPRD", 4)) copy_latin1(buf + at, sl, out.album,  TEXT_LEN);
                    i = at + sl + (sl & 1);            // chunks are padded to an even length
                }
            }
        }
        pos += 8 + (uint64_t)len + (len & 1);
    }
    return out.has_text();
}

}  // namespace

bool read(const Reader& r, Tags& out) {
    out = Tags{};
    if (!r.read || r.size < 16) return false;

    uint8_t hdr[16];
    if (r.read(r.ctx, 0, hdr, sizeof(hdr)) < sizeof(hdr)) return false;

    // One scratch block for whichever parser runs. 16 KB holds the text of any sane tag:
    // what makes a tag large is artwork, and artwork is located rather than copied. Not on
    // the stack: this is called from the player task, whose stack is 6 KB. See scratch().
    constexpr uint32_t SCRATCH = 16 * 1024;
    uint8_t* buf = scratch(SCRATCH);
    if (!buf) return false;

    if (!memcmp(hdr, "ID3", 3))  return read_id3v2(r, hdr, out);
    if (!memcmp(hdr, "fLaC", 4)) return read_flac(r, buf, SCRATCH, out);
    if (!memcmp(hdr, "RIFF", 4) && !memcmp(hdr + 8, "WAVE", 4)) return read_riff(r, buf, SCRATCH, out);
    return false;
}

}  // namespace audio_tags
