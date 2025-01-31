/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdio.h>

#define DEBUG_NET

#if defined(DEBUG_NET)
#define LOG_NET(...) do{ fprintf(stderr, "UIO(NET): "); fprintf(stderr, __VA_ARGS__); }while(0)
#else
#define LOG_NET(...) do{}while(0)
#endif

#define LOG_NET_ERR(...) do{ fprintf(stderr, "UIO(NET)|ERROR: "); fprintf(stderr, __VA_ARGS__); }while(0)
#define LOG_NET_WARN(...) do{ fprintf(stderr, "UIO(NET)|WARN: "); fprintf(stderr, __VA_ARGS__); }while(0)
