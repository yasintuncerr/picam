/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Abstract still source
 * Copyright (C) 2025 Yasin Tunçer
 *
 * Contact: Yasin Tunçer <yasintuncerr@gmail.com>
 */

#include "still-source.h"
#include <stdio.h> 
#include <stdlib.h>


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

int still_source_capture(struct still_source *src, const capture_controls_t *cc)
{
    return src->ops->capture(src, cc);
}

int still_source_capture_off(struct still_source *src)
{
    return src->ops->capture_off(src);
}


