#ifndef FILTER_H
#define FILTER_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define FILTER_SIZE 10

typedef struct {
    int32_t buffer[FILTER_SIZE];
    uint8_t index;
    bool is_filled;
    int64_t running_sum;     // Chạy tổng được duy trì để đạt độ phức tạp O(1)
    SemaphoreHandle_t mutex; // Thêm Mutex bảo vệ tránh race condition
} MovingAverage_t;

void init_moving_average(MovingAverage_t *filter);
int32_t apply_moving_average(MovingAverage_t *filter, int32_t new_val);

#endif // FILTER_H
