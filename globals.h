/*
SPDX-License-Identifier: MPL-2.0
SPDX-FileCopyrightText: 2016 Martin Cerveny <martin@c-home.cz>
*/

#ifndef _GLOBALS_H_
#define _GLOBALS_H_

#include <assert.h>
#include <stdio.h>

// CALL&CHECK

#define A(__what) assert(__what)
#define CA(__call, __check)    \
    do                         \
    {                          \
        int __ret;             \
        __ret = __call;        \
        assert(__ret __check); \
    } while (0);
#define CAZ(__call) CA(__call, == 0)
#define CAP(__call) CA(__call, > 0)
#define CAZP(__call) CA(__call, >= 0)
#define CAV(__var, __call, __check) \
    do                              \
    {                               \
        __var = __call;             \
        assert(__var __check);      \
    } while (0);
#define CAVZ(__var, __call) CAV(__var, __call, == 0)
#define CAVNZ(__var, __call) CAV(__var, __call, != 0)
#define CAVP(__var, __call) CAV(__var, __call, > 0)
#define CAVZP(__var, __call) CAV(__var, __call, >= 0)

// DEBUG

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define POSITION __FILE__ ":" TOSTRING(__LINE__) ": "

#define ERRC(...) printf(__VA_ARGS__)
#define LOGC(...) printf(__VA_ARGS__)
#define DBGC(...) printf(__VA_ARGS__)

#define ERR(...) printf(POSITION "ERROR: " __VA_ARGS__)
#define LOG(...) printf(POSITION __VA_ARGS__)
#define DBG(...) printf(POSITION __VA_ARGS__)

#endif
