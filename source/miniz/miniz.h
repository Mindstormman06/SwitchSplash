/* Stub replacing miniz's full miniz.h: SwitchSplash vendors only the inflate half
 * (tinfl) of miniz (https://github.com/richgel999/miniz @ 2c50092, MIT, see LICENSE).
 * Always include this header (not miniz_tinfl.h directly) so the platform macros below
 * are set before tinfl's headers are processed. */
#pragma once

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ARCHIVE_APIS

/* Platform: AArch64 little-endian (what the full miniz.h detects for __aarch64__).
 * Unaligned loads are left off; they only speed up inflate slightly. */
#define MINIZ_LITTLE_ENDIAN 1
#define MINIZ_HAS_64BIT_REGISTERS 1
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0

#include "miniz_common.h"
#include "miniz_tinfl.h"
