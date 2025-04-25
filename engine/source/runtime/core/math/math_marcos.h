#pragma once

#define ZENTIA_MIN(x, y) (((x) < (y)) ? (x) : (y))
#define ZENTIA_MAX(x, y) (((x) > (y)) ? (x) : (y))
#define ZENTIA_PIN(a, min_value, max_value) ZENTIA_MIN(max_value, ZENTIA_MAX(a, min_value))

#define ZENTIA_VALID_INDEX(idx, range) (((idx) >= 0) && ((idx) < (range)))
#define ZENTIA_PIN_INDEX(idx, range) ZENTIA_PIN(idx, 0, (range)-1)

#define ZENTIA_SIGN(x) ((((x) > 0.0f) ? 1.0f : 0.0f) + (((x) < 0.0f) ? -1.0f : 0.0f))
