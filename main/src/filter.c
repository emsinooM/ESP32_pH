#include "filter.h"
#include <stddef.h>

// Hàm khởi tạo bộ lọc và Mutex
void init_moving_average(MovingAverage_t *filter) {
    for (int i = 0; i < FILTER_SIZE; i++) {
        filter->buffer[i] = 0;
    }
    filter->index = 0;
    filter->is_filled = false;
    filter->running_sum = 0;
    filter->mutex = xSemaphoreCreateMutex();
}

int32_t apply_moving_average(MovingAverage_t *filter, int32_t new_val) {
    int32_t avg = 0;

    // Khóa Mutex trước khi can thiệp vào buffer
    if (filter->mutex != NULL) {
        xSemaphoreTake(filter->mutex, portMAX_DELAY);
    }

    if (filter->is_filled) {
        filter->running_sum -= filter->buffer[filter->index];
    }
    filter->running_sum += new_val;
    filter->buffer[filter->index] = new_val;
    
    filter->index++;
    if (filter->index >= FILTER_SIZE) {
        filter->index = 0;
        filter->is_filled = true;
    }

    int count = filter->is_filled ? FILTER_SIZE : filter->index;
    avg = (int32_t)(filter->running_sum / count);

    // Mở khóa Mutex sau khi hoàn tất
    if (filter->mutex != NULL) {
        xSemaphoreGive(filter->mutex);
    }

    return avg;
}
