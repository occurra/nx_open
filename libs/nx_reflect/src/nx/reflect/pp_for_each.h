// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

// Preprocessor FOR_EACH using C++20 __VA_OPT__ recursive expansion.
#pragma once

#define _NX_PP_CAT_I(X, Y) X##Y
#define NX_PP_CAT(X, Y) _NX_PP_CAT_I(X, Y)

#define _NX_PP_STRINGIZE_I(X) #X
#define NX_PP_STRINGIZE(X) _NX_PP_STRINGIZE_I(X)

#define NX_PP_EXPAND(...) __VA_ARGS__

// Deferred call trick: PARENS expands to (), enabling one more round of macro expansion.
#define _NX_PP_PARENS ()

// Performs 64 expansion passes so the recursive FOR_EACH fully resolves (supports up to 64 args).
#define _NX_PP_RESCAN(...) \
    _NX_PP_RESCAN3(_NX_PP_RESCAN3(_NX_PP_RESCAN3(_NX_PP_RESCAN3(__VA_ARGS__))))
#define _NX_PP_RESCAN3(...) \
    _NX_PP_RESCAN2(_NX_PP_RESCAN2(_NX_PP_RESCAN2(_NX_PP_RESCAN2(__VA_ARGS__))))
#define _NX_PP_RESCAN2(...) \
    _NX_PP_RESCAN1(_NX_PP_RESCAN1(_NX_PP_RESCAN1(_NX_PP_RESCAN1(__VA_ARGS__))))
#define _NX_PP_RESCAN1(...) __VA_ARGS__

#define _NX_PP_FOR_EACH_HELPER(Macro, Data, Item, ...) \
    Macro(Data, Item) __VA_OPT__(_NX_PP_FOR_EACH_AGAIN _NX_PP_PARENS(Macro, Data, __VA_ARGS__))
#define _NX_PP_FOR_EACH_AGAIN() _NX_PP_FOR_EACH_HELPER

/**
 * Calls Macro(Data, Item) for each Item in the variadic argument list.
 * Example: NX_PP_FOR_EACH(MY_MACRO, mydata, a, b, c)
 * expands to: MY_MACRO(mydata, a) MY_MACRO(mydata, b) MY_MACRO(mydata, c)
 */
#define NX_PP_FOR_EACH(Macro, Data, ...) \
    __VA_OPT__(_NX_PP_RESCAN(_NX_PP_FOR_EACH_HELPER(Macro, Data, __VA_ARGS__)))
