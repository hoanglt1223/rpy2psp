/* PC-only access to the software renderer's last completed frame (0xAABBGGRR). */
#ifndef VN_GFX_SOFT_H
#define VN_GFX_SOFT_H

#include <stdint.h>

const uint32_t *gfx_soft_pixels(void);

#endif
