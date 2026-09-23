/*:*
 * ggaze — animated GIF / WebP support (plain C)
 *
 * See animation.h. The two container walks are deliberately dumb: they
 * follow the length prefixes the formats give them and stop at the first
 * thing they do not understand, never decoding anything. What they say is
 * checked again by the decoder (a GdkPixbufAnimation that turns out static
 * is shown as a still), so a wrong count costs at most a missed animation,
 * never a wrong picture.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "animation.h"

#include <string.h>

#include <gdk/gdk.h>
#include <glib.h>

#include "detect.h"

/* --- GIF ------------------------------------------------------------------
 *
 * Header (6) + logical screen descriptor (7) [+ global colour table], then
 * blocks until the 0x3B trailer: 0x21 extensions (label byte + data
 * sub-blocks) and 0x2C image descriptors (10 bytes [+ local colour table]
 * + LZW minimum code size + data sub-blocks). Data sub-blocks are a length
 * byte followed by that many bytes, ended by a zero length byte. */

enum {
   GIF_BLOCK_EXTENSION = 0x21,
   GIF_BLOCK_IMAGE     = 0x2C,
   GIF_BLOCK_TRAILER   = 0x3B,
   GIF_HEADER_LEN      = 13, /* signature + logical screen descriptor */
   GIF_IMAGE_DESC_LEN  = 10,
};

/* Bytes of the colour table a packed-fields byte announces: 3 x 2^(n+1)
 * when its high bit is set, none otherwise. */
static gsize
_gif_color_table_len(guint8 u_packed) {
   if ((u_packed & 0x80) == 0) {
      return (0);
   }
   return (3u * (1u << ((u_packed & 0x07) + 1)));
}

/* Skip the data sub-blocks at *p_pos. TRUE with *p_pos past the zero
 * terminator; FALSE when the buffer ends first (*p_pos is then past the
 * end and the walk stops). */
static gboolean
_gif_skip_sub_blocks(const guint8 *p_buf, gsize u_len, gsize *p_pos) {
   gsize u_pos = *p_pos;
   while (u_pos < u_len) {
      guint8 u_n = p_buf[u_pos++];
      if (u_n == 0) {
         *p_pos = u_pos;
         return (TRUE);
      }
      u_pos += u_n;
   }
   *p_pos = u_pos;
   return (FALSE);
}

/* One block at *p_pos: count an image descriptor, skip whatever else is
 * skippable. FALSE ends the walk (trailer, garbage, or the buffer ran
 * out mid-block). */
static gboolean
_gif_step(const guint8 *p_buf, gsize u_len, gsize *p_pos, guint *p_frames) {
   gsize u_pos = *p_pos;
   switch (p_buf[u_pos]) {
   case GIF_BLOCK_EXTENSION:
      u_pos += 2; /* introducer + label */
      break;
   case GIF_BLOCK_IMAGE:
      if (u_pos + GIF_IMAGE_DESC_LEN > u_len) {
         return (FALSE);
      }
      /* A descriptor whose header is complete counts even when its pixel
       * data is cut short below: the decoder shows what it can of it. */
      (*p_frames)++;
      u_pos += GIF_IMAGE_DESC_LEN + _gif_color_table_len(p_buf[u_pos + 9]);
      u_pos += 1; /* LZW minimum code size */
      break;
   default:
      return (FALSE); /* trailer, or not a GIF block at all */
   }
   *p_pos = u_pos;
   return (_gif_skip_sub_blocks(p_buf, u_len, p_pos));
}

static void
_gif_probe(const guint8 *p_buf, gsize u_len, GgazeAnimProbe *p_out) {
   if (u_len < GIF_HEADER_LEN) {
      return;
   }
   p_out->u_width  = (guint)p_buf[6] | ((guint)p_buf[7] << 8);
   p_out->u_height = (guint)p_buf[8] | ((guint)p_buf[9] << 8);
   gsize u_pos     = GIF_HEADER_LEN + _gif_color_table_len(p_buf[10]);
   while (u_pos < u_len && _gif_step(p_buf, u_len, &u_pos, &p_out->u_frames)) {
      /* next block */
   }
}

/* --- WebP -----------------------------------------------------------------
 *
 * RIFF header (12) then chunks: fourcc (4), little-endian payload size
 * (4), payload padded to an even length. An animated file has a VP8X
 * chunk whose flags carry the animation bit and one ANMF chunk per frame;
 * a still has a bare VP8/VP8L bitstream (with or without VP8X). */

enum {
   WEBP_HEADER_LEN     = 12,
   WEBP_CHUNK_HDR_LEN  = 8,
   WEBP_VP8X_LEN       = 10,
   WEBP_VP8X_FLAG_ANIM = 0x02,
};

static guint32
_le24(const guint8 *p) {
   return ((guint32)p[0] | ((guint32)p[1] << 8) | ((guint32)p[2] << 16));
}

static guint32
_le32(const guint8 *p) {
   return (_le24(p) | ((guint32)p[3] << 24));
}

/* The VP8X chunk's payload at p_c: the animation flag and the canvas,
 * stored as width-1 / height-1 in 24 bits each. */
static gboolean
_webp_read_vp8x(const guint8 *p_c, GgazeAnimProbe *p_out) {
   p_out->u_width  = 1u + _le24(p_c + 4);
   p_out->u_height = 1u + _le24(p_c + 7);
   return ((p_c[0] & WEBP_VP8X_FLAG_ANIM) != 0);
}

static void
_webp_probe(const guint8 *p_buf, gsize u_len, GgazeAnimProbe *p_out) {
   gboolean b_anim   = FALSE;
   guint    u_frames = 0;
   gsize    u_pos    = WEBP_HEADER_LEN;
   while (u_pos + WEBP_CHUNK_HDR_LEN <= u_len) {
      const guint8 *p_c    = p_buf + u_pos;
      guint32       u_size = _le32(p_c + 4);
      if (memcmp(p_c, "VP8X", 4) == 0) {
         if (u_pos + WEBP_CHUNK_HDR_LEN + WEBP_VP8X_LEN > u_len) {
            break;
         }
         b_anim = _webp_read_vp8x(p_c + WEBP_CHUNK_HDR_LEN, p_out);
      } else if (memcmp(p_c, "ANMF", 4) == 0) {
         u_frames++;
      }
      if (u_size > u_len) {
         break; /* a size past the file: nothing sound follows */
      }
      u_pos += WEBP_CHUNK_HDR_LEN + u_size + (u_size & 1u);
   }
   /* Without the animation flag the file is a still whatever chunks it
    * carries; with it, the frames are the ANMF chunks seen. */
   p_out->u_frames = b_anim ? u_frames : 1;
}

/* --- public ---------------------------------------------------------------
 */

gboolean
animation_probe(const guint8 *p_buf, gsize u_len, GgazeAnimProbe *p_out) {
   g_return_val_if_fail(p_out != NULL, FALSE);
   memset(p_out, 0, sizeof(*p_out));
   if (p_buf == NULL || u_len == 0) {
      return (FALSE);
   }
   switch (detect_format(p_buf, u_len)) {
   case GGAZE_FMT_GIF:
      _gif_probe(p_buf, u_len, p_out);
      break;
   case GGAZE_FMT_WEBP:
      _webp_probe(p_buf, u_len, p_out);
      break;
   default:
      break; /* every other format is a still here */
   }
   return (p_out->u_frames >= 2);
}

gboolean
animation_within_budget(const GgazeAnimProbe *p_probe) {
   g_return_val_if_fail(p_probe != NULL, FALSE);
   if (p_probe->u_frames == 0 || p_probe->u_frames > GGAZE_ANIM_MAX_FRAMES ||
       p_probe->u_width == 0 || p_probe->u_height == 0) {
      return (FALSE);
   }
   if (p_probe->u_width > GGAZE_IMAGE_MAX_SIDE ||
       p_probe->u_height > GGAZE_IMAGE_MAX_SIDE) {
      return (FALSE);
   }
   /* Each factor is at most 32-bit, so neither 64-bit product can wrap. */
   guint64 u_canvas = (guint64)p_probe->u_width * p_probe->u_height;
   if (u_canvas > GGAZE_ANIM_MAX_CANVAS_PIXELS) {
      return (FALSE);
   }
   return (u_canvas * p_probe->u_frames <= GGAZE_ANIM_MAX_PIXELS);
}

gint
animation_frame_delay_ms(gint i_reported) {
   if (i_reported < 0) {
      return (-1);
   }
   return (MAX(i_reported, GGAZE_ANIM_MIN_DELAY_MS));
}

/* --- the decoded frames ---------------------------------------------------
 */

struct _GgazeAnimation {
   GPtrArray *p_frames; /* GdkTexture of frames 1..n-1 (frame 0 is the
                         * texture the animation is attached to) */
   GArray *p_delays;    /* gint, as reported, of frames 0..n-1 */
};

GgazeAnimation *
animation_new(gint i_first_delay_ms) {
   GgazeAnimation *p_anim = g_new0(GgazeAnimation, 1);
   p_anim->p_frames       = g_ptr_array_new_with_free_func(g_object_unref);
   p_anim->p_delays       = g_array_new(FALSE, FALSE, sizeof(gint));
   g_array_append_val(p_anim->p_delays, i_first_delay_ms);
   return (p_anim);
}

void
animation_delete(GgazeAnimation *p_anim) {
   if (p_anim == NULL) {
      return;
   }
   g_ptr_array_unref(p_anim->p_frames);
   g_array_unref(p_anim->p_delays);
   g_free(p_anim);
}

void
animation_append_frame(GgazeAnimation *p_anim, GdkTexture *p_frame,
                       gint i_delay_ms) {
   g_return_if_fail(p_anim != NULL);
   g_return_if_fail(GDK_IS_TEXTURE(p_frame));
   g_ptr_array_add(p_anim->p_frames, g_object_ref(p_frame));
   g_array_append_val(p_anim->p_delays, i_delay_ms);
}

guint
animation_get_n_frames(const GgazeAnimation *p_anim) {
   g_return_val_if_fail(p_anim != NULL, 0);
   return (p_anim->p_delays->len);
}

GdkTexture *
animation_get_frame(const GgazeAnimation *p_anim, guint u_idx) {
   g_return_val_if_fail(p_anim != NULL, NULL);
   if (u_idx == 0 || u_idx > p_anim->p_frames->len) {
      return (NULL);
   }
   return (GDK_TEXTURE(g_ptr_array_index(p_anim->p_frames, u_idx - 1)));
}

gint
animation_get_delay_ms(const GgazeAnimation *p_anim, guint u_idx) {
   g_return_val_if_fail(p_anim != NULL, -1);
   if (u_idx >= p_anim->p_delays->len) {
      return (-1);
   }
   return (
      animation_frame_delay_ms(g_array_index(p_anim->p_delays, gint, u_idx)));
}

/* --- the texture <-> animation channel -------------------------------------
 *
 * The one quark the attach/lookup pair shares; the texture's qdata
 * destroy notify is animation_delete, so an attached animation lives
 * exactly as long as its first frame does. */
static GQuark
_animation_quark(void) {
   static GQuark u_quark = 0;
   if (u_quark == 0) {
      u_quark = g_quark_from_static_string("ggaze-animation");
   }
   return (u_quark);
}

void
animation_attach(GdkTexture *p_tex, GgazeAnimation *p_anim) {
   g_return_if_fail(GDK_IS_TEXTURE(p_tex));
   g_object_set_qdata_full(G_OBJECT(p_tex), _animation_quark(), p_anim,
                           (GDestroyNotify)animation_delete);
}

const GgazeAnimation *
animation_lookup(GdkTexture *p_tex) {
   if (p_tex == NULL) {
      return (NULL);
   }
   return ((const GgazeAnimation *)g_object_get_qdata(G_OBJECT(p_tex),
                                                      _animation_quark()));
}
