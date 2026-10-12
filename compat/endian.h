/* minimal <endian.h> for freestanding cross builds (SDL_endian.h needs it on Linux) */
#ifndef USDS_COMPAT_ENDIAN_H
#define USDS_COMPAT_ENDIAN_H
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define __PDP_ENDIAN 3412
#define __BYTE_ORDER __LITTLE_ENDIAN
#endif
