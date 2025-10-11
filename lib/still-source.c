/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Abstract still source
 * Copyright (C) 2025 Yasin Tunçer
 *
 * Contact: Yasin Tunçer <yasintuncerr@gmail.com>
 */

#include "still-source.h"

void still_source_destroy(struct still_source *src)
{
    if (src)
        src->ops->destroy(src);
}

int still_source_set_format(struct still_source *src, struct v4l2_pix_format *fmt)
{
    return src->ops->set_format(src, fmt);
}

int still_source_alloc_buffer(struct still_source *src)
{
    return src->ops->alloc_buffer(src);
}

int still_source_free_buffer(struct still_source *src)
{
    return src->ops->free_buffer(src);
}

int still_source_capture(struct still_source *src)
{
    return src->ops->capture(src);
}

struct still_buffer *still_source_get_buffer(struct still_source *src)
{
    return src->ops->get_buffer(src);
}
