#pragma once

#define ZENGINE_MIN(x, y)                    (((x) < (y)) ? (x) : (y))
#define ZENGINE_MAX(x, y)                    (((x) > (y)) ? (x) : (y))
#define ZENGINE_PIN(a, min_value, max_value) ZENGINE_MIN(max_value, ZENGINE_MAX(a, min_value))

#define ZENGINE_VALID_INDEX(idx, range) (((idx) >= 0) && ((idx) < (range)))
#define ZENGINE_PIN_INDEX(idx, range)   ZENGINE_PIN(idx, 0, (range) - 1)

#define ZENGINE_SIGN(x) ((((x) > 0.0f) ? 1.0f : 0.0f) + (((x) < 0.0f) ? -1.0f : 0.0f))
