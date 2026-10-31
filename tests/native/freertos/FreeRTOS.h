#pragma once
#include <mutex>
using SemaphoreHandle_t = std::recursive_mutex*;
constexpr int pdTRUE = 1;
constexpr unsigned portMAX_DELAY = ~0u;
