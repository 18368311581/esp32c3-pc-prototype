#pragma once

/* Public demonstration keys: not secrets. Use matching private 16-byte keys
 * on both boards for your own setup; this header is compiled into firmware.
 * The published demo keys intentionally differ from the original test boards.
 */
#define WIRELESS_CHANNEL 1
#define WIRELESS_PMK "DEMO_PMK_0123456"
#define WIRELESS_LMK "DEMO_LMK_0123456"

_Static_assert(sizeof(WIRELESS_PMK) == 17, "PMK must contain exactly 16 bytes");
_Static_assert(sizeof(WIRELESS_LMK) == 17, "LMK must contain exactly 16 bytes");
