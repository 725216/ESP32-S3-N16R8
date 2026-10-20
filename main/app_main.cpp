#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    while(1)
    {
        printf("ESP32-S3 N16R8 OPI Test OK!\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
