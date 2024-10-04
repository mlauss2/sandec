#ifndef _BYTESWAP_H_
#define _BYTESWAP_H_

#include <byteswap.h>

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

#define le32_to_cpu(x) (x)
#define le16_to_cpu(x) (x)
#define be32_to_cpu(x) bswap_32(x)
#define be16_to_cpu(x) bswap_16(x)

#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__

#define be32_to_cpu(x)  (x)
#define be16_to_cpu(x)  (x)
#define le32_to_cpu(x)  bswap_32(x)
#define le16_to_cpu(x)  bswap_16(x)

#else

#error "unknown endianness"

#endif

#endif
