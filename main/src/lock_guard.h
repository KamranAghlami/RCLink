#pragma once

#include <freertos/semphr.h>

template <typename T>
class lock_guard
{
public:
    lock_guard(T &semaphore) : m_semaphore(semaphore)
    {
        xSemaphoreTake(m_semaphore, portMAX_DELAY);
    }

    ~lock_guard()
    {
        xSemaphoreGive(m_semaphore);
    }

private:
    T &m_semaphore;
};
