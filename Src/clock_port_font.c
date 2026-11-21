/*
 * clock_port_font.c
 * ---------------------------------------------------------------------------
 * Minimal, from-scratch decoder for u8g2's *compiled* font format, so we can
 * draw u8g2_font_logisoso62_tn / u8g2_font_profont17_tr straight onto our own
 * g_framebuffer without linking any of u8g2 itself.
 *
 * This is a clean-room re-implementation of the algorithm documented in
 * u8g2's csrc/u8g2_font.c (BSD licensed) - i.e. it reproduces the *logic*
 * (header layout, bit-packed RLE glyph format) but none of u8g2's own code,
 * data structures, or display abstraction. Verified byte-for-byte against
 * the header of u8g2_font_profont17_tr.h (glyph_cnt=96, max_char_height=17 -
 * matches "ProFont17").
 *
 * Font binary layout (all u8g2 "new format" fonts, which both of yours are):
 *
 *   Offset  Bytes  Field
 *   0       1      glyph_cnt
 *   1       1      bbx_mode
 *   2       1      bits_per_0              (RLE "off-run" field width)
 *   3       1      bits_per_1              (RLE "on-run" field width)
 *   4       1      bits_per_char_width
 *   5       1      bits_per_char_height
 *   6       1      bits_per_char_x
 *   7       1      bits_per_char_y
 *   8       1      bits_per_delta_x
 *   9       1      max_char_width
 *   10      1      max_char_height
 *   11      1      x_offset
 *   12      1      y_offset
 *   13      1      ascent_A
 *   14      1      descent_g
 *   15      1      ascent_para
 *   16      1      descent_para
 *   17-18   2      start_pos_upper_A (big-endian)
 *   19-20   2      start_pos_lower_a (big-endian)
 *   21-22   2      start_pos_unicode (unused here, ASCII only)
 *
 * Followed immediately by the glyph table: a flat run of variable-size
 * entries, each starting with { uint8_t encoding; uint8_t entry_size; }
 * followed by the bit-packed glyph payload. entry_size == 0 terminates a
 * sub-table. To keep lookups fast, the header gives byte offsets (relative
 * to right after the header) for where the 'A' and 'a' sub-tables begin;
 * codes below 'A' are searched from the very start of the table.
 *
 * Per-glyph bit stream (all fields packed LSB-first, byte-independent):
 *   glyph_width   : bits_per_char_width  (unsigned)
 *   glyph_height  : bits_per_char_height (unsigned)
 *   x             : bits_per_char_x      (signed)   - offset of bbox from pen
 *   y             : bits_per_char_y      (signed)   - offset of bbox from baseline
 *   delta_x       : bits_per_delta_x     (signed)   - advance to next glyph
 *   [[ if glyph_width == 0, that's the whole entry (e.g. space) ]]
 *   then repeating RLE pairs, scanning the glyph bitmap row-major:
 *     a = bits_per_0 bits -> run of "off"  pixels
 *     b = bits_per_1 bits -> run of "on"   pixels
 *     (repeat a,b pair while next single bit == 1)
 *     when local y reaches glyph_height, glyph is done
 *
 * "Signed" fields are stored as unsigned value minus 2^(bits-1) (i.e. the
 * encoder biases them up by half range before packing).
 * ---------------------------------------------------------------------------
 */
#include "clock_port.h"

/* --- font & background assets ------------------------------------------ */
/* u8g2 normally places these arrays in a special linker section on AVR and
   wraps flash access in pgm_read_byte(). On STM32 flash is memory-mapped,
   so both macros are simply no-ops here and we index the arrays directly. */
#define U8G2_FONT_SECTION(name)
#include "u8g2_font_logisoso46_tn.h"
#include "u8g2_font_logisoso38_tn.h"
#include "u8g2_font_profont15_tr.h"

#ifndef PROGMEM
#define PROGMEM
#endif
#if USE_RLE_BACKGROUND
#include "monochromebg_rle.h"   /* converted 250x122 art; array must be named epd_bitmap_rle */
#endif

#define U8G2_FONT_HEADER_SIZE 23U

typedef struct {
  const uint8_t *ptr;
  uint8_t bit_pos;
} font_bitreader_t;

typedef struct {
  uint8_t bits_per_0;
  uint8_t bits_per_1;
  uint8_t bits_per_char_width;
  uint8_t bits_per_char_height;
  uint8_t bits_per_char_x;
  uint8_t bits_per_char_y;
  uint8_t bits_per_delta_x;
  uint16_t start_pos_upper_A;
  uint16_t start_pos_lower_a;
} font_info_t;

static void font_read_info(const uint8_t *font, font_info_t *info)
{
  info->bits_per_0 = font[2];
  info->bits_per_1 = font[3];
  info->bits_per_char_width = font[4];
  info->bits_per_char_height = font[5];
  info->bits_per_char_x = font[6];
  info->bits_per_char_y = font[7];
  info->bits_per_delta_x = font[8];
  info->start_pos_upper_A = (uint16_t)(((uint16_t)font[17] << 8) | font[18]);
  info->start_pos_lower_a = (uint16_t)(((uint16_t)font[19] << 8) | font[20]);
}

/* Reads 'cnt' bits (cnt <= 8) starting at the reader's current bit position,
   LSB-first, advancing across byte boundaries as needed. Matches u8g2's
   u8g2_font_decode_get_unsigned_bits() exactly. */
static uint8_t font_get_unsigned_bits(font_bitreader_t *f, uint8_t cnt)
{
  uint16_t val = f->ptr[0];
  val >>= f->bit_pos;
  uint8_t bit_pos_plus_cnt = (uint8_t)(f->bit_pos + cnt);
  if (bit_pos_plus_cnt >= 8U) {
    uint8_t s = (uint8_t)(8U - f->bit_pos);
    f->ptr++;
    val |= (uint16_t)((uint16_t)f->ptr[0] << s);
    bit_pos_plus_cnt = (uint8_t)(bit_pos_plus_cnt - 8U);
  }
  val &= (uint16_t)((1U << cnt) - 1U);
  f->bit_pos = bit_pos_plus_cnt;
  return (uint8_t)val;
}

/* Signed fields are unsigned value biased by 2^(cnt-1). */
static int8_t font_get_signed_bits(font_bitreader_t *f, uint8_t cnt)
{
  int16_t v = (int16_t)font_get_unsigned_bits(f, cnt);
  int16_t bias = (int16_t)(1 << (cnt - 1U));
  return (int8_t)(v - bias);
}

/* Locates the bit-stream start for `encoding` in `font`, or NULL if the
   font doesn't contain that glyph. */
static const uint8_t *font_find_glyph(const uint8_t *font, const font_info_t *info, uint8_t encoding)
{
  const uint8_t *p = font + U8G2_FONT_HEADER_SIZE;
  if (encoding >= (uint8_t)'a') {
    p += info->start_pos_lower_a;
  } else if (encoding >= (uint8_t)'A') {
    p += info->start_pos_upper_A;
  }
  for (;;) {
    uint8_t entry_size = p[1];
    if (entry_size == 0U) {
      return NULL;
    }
    if (p[0] == encoding) {
      return p + 2;
    }
    p += entry_size;
  }
}

/* Decodes and (optionally) draws one glyph at pen position (x, y-is-baseline).
   Returns the horizontal advance (delta_x) for the character, or 0 if the
   font has no such glyph. Pass draw=false to just measure. */
static int8_t font_draw_glyph(const uint8_t *font, const font_info_t *info,
                               uint16_t x, uint16_t y, uint8_t encoding,
                               uint8_t value, bool draw)
{
  const uint8_t *glyph = font_find_glyph(font, info, encoding);
  if (glyph == NULL) {
    return 0;
  }

  font_bitreader_t br;
  br.ptr = glyph;
  br.bit_pos = 0U;

  uint8_t width = font_get_unsigned_bits(&br, info->bits_per_char_width);
  uint8_t height = font_get_unsigned_bits(&br, info->bits_per_char_height);
  int8_t gx = font_get_signed_bits(&br, info->bits_per_char_x);
  int8_t gy = font_get_signed_bits(&br, info->bits_per_char_y);
  int8_t dx = font_get_signed_bits(&br, info->bits_per_delta_x);

  if ((width == 0U) || !draw) {
    return dx;
  }

  int32_t start_x = (int32_t)x + gx;
  int32_t start_y = (int32_t)y - ((int32_t)height + gy);

  uint8_t lx = 0U;
  uint8_t ly = 0U;
  for (;;) {
    uint8_t a = font_get_unsigned_bits(&br, info->bits_per_0);
    uint8_t b = font_get_unsigned_bits(&br, info->bits_per_1);
    do {
      for (uint8_t i = 0U; i < a; ++i) {
        ++lx;
        if (lx >= width) { lx = 0U; ++ly; }
      }
      for (uint8_t i = 0U; i < b; ++i) {
        int32_t px = start_x + lx;
        int32_t py = start_y + ly;
        if ((px >= 0) && (py >= 0)) {
          fb_set_pixel((uint16_t)px, (uint16_t)py, value);
        }
        ++lx;
        if (lx >= width) { lx = 0U; ++ly; }
      }
    } while (font_get_unsigned_bits(&br, 1U) != 0U);
    if (ly >= height) {
      break;
    }
  }
  return dx;
}

uint16_t fb_string_width_u8g2(const uint8_t *font, const char *text)
{
  font_info_t info;
  font_read_info(font, &info);
  uint16_t total = 0U;
  while (*text != '\0') {
    total = (uint16_t)(total + font_draw_glyph(font, &info, 0U, 0U, (uint8_t)*text, 0U, false));
    ++text;
  }
  return total;
}

uint16_t fb_draw_string_u8g2(const uint8_t *font, uint16_t x, uint16_t y, const char *text, uint8_t value)
{
  font_info_t info;
  font_read_info(font, &info);
  uint16_t cursor = x;
  uint16_t start = x;
  while (*text != '\0') {
    int8_t dx = font_draw_glyph(font, &info, cursor, y, (uint8_t)*text, value, true);
    cursor = (uint16_t)(cursor + dx);
    ++text;
  }
  return (uint16_t)(cursor - start);
}

/* Pixels above the baseline for `ch` (glyph bbox height + y offset). Used by
   the layout code to place text by its top edge instead of hard-coding the
   font's size. Returns 0 if the font has no such glyph. */
uint8_t fb_font_glyph_top(const uint8_t *font, char ch)
{
  font_info_t info;
  font_read_info(font, &info);
  const uint8_t *glyph = font_find_glyph(font, &info, (uint8_t)ch);
  if (glyph == NULL) {
    return 0U;
  }
  font_bitreader_t br;
  br.ptr = glyph;
  br.bit_pos = 0U;
  (void)font_get_unsigned_bits(&br, info.bits_per_char_width);
  uint8_t height = font_get_unsigned_bits(&br, info.bits_per_char_height);
  (void)font_get_signed_bits(&br, info.bits_per_char_x);
  int8_t gy = font_get_signed_bits(&br, info.bits_per_char_y);
  int16_t top = (int16_t)height + gy;
  return (top > 0) ? (uint8_t)top : 0U;
}

/* ---------------------------------------------------------------------- */
/* RLE background decoder                                                  */
/* ---------------------------------------------------------------------- */
/* Ported logic-for-logic from ST7305clock.cpp's drawRleBackground(): the
   compressed data is a flat run of (count, value) byte pairs. Each `value`
   byte repeats `count` times in the decompressed stream, which is then
   sliced into (w_pixels/8)-byte rows and blitted top to bottom via
   u8g2_DrawBitmap() semantics in the original - which is MSB-first: bit 7
   is the LEFTMOST of the 8 pixels, verified against u8g2's own
   u8g2_DrawHorizontalBitmap() source (mask starts at 128 and shifts right). */
void fb_draw_rle_bitmap(uint16_t x, uint16_t y, uint16_t w_pixels, uint16_t h_pixels, const uint8_t *comp_data)
{
  /* Rows are padded to whole bytes (250 px -> 32 bytes). The 384-px original
     was a multiple of 8 so a plain w/8 worked; (w+7)/8 is the general form.
     Pixels past DISPLAY_WIDTH are clipped by fb_set_pixel(). */
  uint16_t bytes_per_row = (uint16_t)((w_pixels + 7U) / 8U);
  uint32_t total_bytes = (uint32_t)bytes_per_row * h_pixels;
  uint32_t drawn_bytes = 0U;
  uint32_t data_idx = 0U;
  uint16_t row_byte_idx = 0U;
  uint16_t current_y = y;

  while (drawn_bytes < total_bytes) {
    uint8_t count = comp_data[data_idx++];
    uint8_t val = comp_data[data_idx++];
    for (uint8_t i = 0U; i < count; ++i) {
      uint16_t px = (uint16_t)(x + (row_byte_idx * 8U));
      if (val != 0U) {
        for (uint8_t b = 0U; b < 8U; ++b) {
          if ((val & (uint8_t)(1U << (7U - b))) != 0U) {
            fb_set_pixel((uint16_t)(px + b), current_y, 1U);
          }
        }
      }
      ++drawn_bytes;
      ++row_byte_idx;
      if (row_byte_idx >= bytes_per_row) {
        row_byte_idx = 0U;
        ++current_y;
        if (current_y >= (uint16_t)(y + h_pixels)) {
          return;
        }
      }
    }
  }
}
