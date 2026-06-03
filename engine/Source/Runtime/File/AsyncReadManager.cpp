#define UNSIGNED_FLAGS_1(FLAG1_) static_cast<unsigned int>(FLAG1_)
#define UNSIGNED_FLAGS(...)      PP_VARG_SELECT_OVERLOAD(UNSIGNED_FLAGS_, (__VA_ARGS__))

#define PP_VARG_COUNT(...)                   \
    DETAIL_PP_EXPAND_2(DETAIL_PP_VARG_COUNT, \
                       (__VA_ARGS__, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1))

#define PP_VARG_SELECT_OVERLOAD(NAME_, ARGS_) \
    DETAIL_PP_EXPAND_2(DETAIL_PP_VARG_CONCAT(NAME_, PP_VARG_COUNT ARGS_), ARGS_)

#define DETAIL_PP_EXPAND_2(A_, B_) A_ B_

#define DETAIL_PP_VARG_COUNT(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, N, ...) N

#define DETAIL_PP_VARG_CONCAT_Y(A_, B_) A_##B_
#define DETAIL_PP_VARG_CONCAT_X(A_, B_) DETAIL_PP_VARG_CONCAT_Y(A_, B_)
#define DETAIL_PP_VARG_CONCAT(A_, B_)   DETAIL_PP_VARG_CONCAT_X(A_, B_)