/*:*
 * ggaze — bounded reads on a GInputStream
 *
 * See streamread.h.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "streamread.h"

#include <gio/gio.h>
#include <glib.h>

StreamReadStatus
streamread_exact(GInputStream *p_in, guint8 *p_buf, gsize u_len,
                 GError **p_err) {
   g_return_val_if_fail(G_IS_INPUT_STREAM(p_in), STREAMREAD_ERROR);
   gsize u_got = 0;
   if (!g_input_stream_read_all(p_in, p_buf, u_len, &u_got, NULL, p_err)) {
      return (STREAMREAD_ERROR);
   }
   return (u_got == u_len ? STREAMREAD_OK : STREAMREAD_EOF);
}

StreamReadStatus
streamread_skip(GInputStream *p_in, gsize u_len, GError **p_err) {
   g_return_val_if_fail(G_IS_INPUT_STREAM(p_in), STREAMREAD_ERROR);
   /* g_input_stream_skip() may skip less than asked (a seekable stream
    * seeks; a plain one reads and discards a buffer at a time): loop until
    * done, and treat a zero skip as the end of the stream. */
   while (u_len > 0) {
      gssize i_n = g_input_stream_skip(p_in, u_len, NULL, p_err);
      if (i_n < 0) {
         return (STREAMREAD_ERROR);
      }
      if (i_n == 0) {
         return (STREAMREAD_EOF);
      }
      u_len -= (gsize)i_n;
   }
   return (STREAMREAD_OK);
}
