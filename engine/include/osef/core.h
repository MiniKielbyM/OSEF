#pragma once
// OSEF core library: freestanding replacements for the parts of the C++
// standard library the engine uses. Compiles hosted (next to std headers)
// and bare metal (-ffreestanding -nostdinc++).

#include <osef/core/base.h>
#include <osef/core/memory.h>
#include <osef/core/string.h>
#include <osef/core/vector.h>
#include <osef/core/map.h>
#include <osef/core/deque.h>
#include <osef/core/function.h>
#include <osef/core/format.h>
#include <osef/core/algorithm.h>
#include <osef/core/random.h>
