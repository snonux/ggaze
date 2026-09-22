#ifndef GGAZE_ENUMS_H
#define GGAZE_ENUMS_H

/*:*
 * ggaze — enums shared across layers
 *
 * The three preference enums live here, in a header with no includes of its
 * own, so the settings wrapper (infrastructure) no longer has to include the
 * navigator (domain) for GgazeSort, and the viewer widget no longer has to
 * include the settings wrapper for its background/scroll modes. The values
 * are the schema's enum nick order (data/org.buetow.ggaze.gschema.xml), so
 * a GSettings enum read casts straight to them; prefs.c reads the nicks
 * from the schema at runtime, so there is no second copy to keep in step.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

/* Folder listing order (schema enum org.buetow.ggaze.sort). */
typedef enum {
   GGAZE_SORT_NAME = 0,
   GGAZE_SORT_TIME,
   GGAZE_SORT_SIZE
} GgazeSort;

/* Background colour behind the image in the large view (schema enum
 * org.buetow.ggaze.background: black=0 ... checker=3). */
typedef enum {
   GGAZE_BG_BLACK = 0,
   GGAZE_BG_DARK,
   GGAZE_BG_GREY,
   GGAZE_BG_CHECKER
} GgazeBackground;

/* What the mouse scroll wheel does in the large view (schema enum
 * org.buetow.ggaze.scroll-behavior: zoom=0 ... navigate=2). */
typedef enum {
   GGAZE_SCROLL_ZOOM = 0,
   GGAZE_SCROLL_PAN_WHEN_ZOOMED,
   GGAZE_SCROLL_NAVIGATE
} GgazeScrollBehavior;

#endif /* GGAZE_ENUMS_H */
