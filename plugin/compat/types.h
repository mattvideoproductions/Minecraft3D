#pragma once
// Fallback for CTRPluginFramework headers, which #include "types.h" and use
// the old libctru macro names. The devkitPro libctrpf package ships no
// types.h, and libctru 2.7+ renamed these macros (CTR_ALIGN, CTR_PACKED, ...)
// and dropped NORETURN. The Makefile adds this folder with -idirafter, so a
// CTRPF that brings its own types.h still takes precedence.
#include <3ds/types.h>

#ifndef NORETURN
#define NORETURN __attribute__((noreturn))
#endif
#ifndef PACKED
#define PACKED __attribute__((packed))
#endif
#ifndef ALIGN
#define ALIGN(m) __attribute__((aligned(m)))
#endif
#ifndef DEPRECATED
#define DEPRECATED __attribute__((deprecated))
#endif
