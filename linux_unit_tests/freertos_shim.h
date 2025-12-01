// Minimal FreeRTOS shim for host-side unit testing
// Provides semaphore primitives used by RaftCore/RaftI2C when building outside ESP-IDF
#pragma once

#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <cstdint>
#include <climits>

// FreeRTOS-style basic types and constants
using BaseType_t = int;
using TickType_t = uint32_t;
#ifndef pdTRUE
#define pdTRUE 1
#endif
#ifndef pdFALSE
#define pdFALSE 0
#endif
#ifndef portMAX_DELAY
#define portMAX_DELAY 0xffffffff
#endif
#ifndef portTICK_PERIOD_MS
#define portTICK_PERIOD_MS 1
#endif
#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(x) (x)
#endif

// Map semaphore handle onto a pthread mutex pointer
using SemaphoreHandle_t = pthread_mutex_t*;

inline SemaphoreHandle_t xSemaphoreCreateMutex()
{
    auto* mutex = new pthread_mutex_t;
    pthread_mutex_init(mutex, nullptr);
    return mutex;
}

inline BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeoutTicks)
{
    if (!mutex)
        return pdFALSE;

    // Immediate try-lock when timeout is zero
    if (timeoutTicks == 0)
        return pthread_mutex_trylock(mutex) == 0 ? pdTRUE : pdFALSE;

    // Simplified blocking lock for host tests
    (void)timeoutTicks;
    return pthread_mutex_lock(mutex) == 0 ? pdTRUE : pdFALSE;
}

inline BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    if (!mutex)
        return pdFALSE;
    return pthread_mutex_unlock(mutex) == 0 ? pdTRUE : pdFALSE;
}

inline void vSemaphoreDelete(SemaphoreHandle_t mutex)
{
    if (!mutex)
        return;
    pthread_mutex_destroy(mutex);
    delete mutex;
}

// Missing utility helpers when building off-target
#ifndef ULONG_LONG_MAX
#define ULONG_LONG_MAX ULLONG_MAX
#endif

namespace Raft
{
inline uint16_t getBEUInt16(const uint8_t* pBuf, uint32_t offset)
{
    return (static_cast<uint16_t>(pBuf[offset]) << 8) | pBuf[offset + 1];
}

inline uint32_t getBEUInt32(const uint8_t* pBuf, uint32_t offset)
{
    return (static_cast<uint32_t>(pBuf[offset]) << 24) |
           (static_cast<uint32_t>(pBuf[offset + 1]) << 16) |
           (static_cast<uint32_t>(pBuf[offset + 2]) << 8) |
           static_cast<uint32_t>(pBuf[offset + 3]);
}
}
