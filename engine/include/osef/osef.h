#pragma once
// OSEF — Operating System Engine Framework. Umbrella header.
//
// Include it right after your OSEF_HARDWARE(...) declaration (see
// osef/hardware.h). Everything here is freestanding, so the same OS source
// builds as a hosted app and as a bare-metal kernel.

#include <osef/core.h>
#include <osef/hardware.h>

#include <osef/interrupts.h>
#include <osef/ipc.h>
#include <osef/kernel.h>
#include <osef/module.h>
#include <osef/platform.h>
#include <osef/process.h>
#include <osef/program.h>
#include <osef/scheduler.h>
#include <osef/shell.h>
#include <osef/syscall.h>
#include <osef/vfs.h>

#if OSEF_HOSTED
#  include <osef/hostfs.h>
#endif

#define OSEF_VERSION_MAJOR 0
#define OSEF_VERSION_MINOR 1
#define OSEF_VERSION_PATCH 0
