// ไฟกะพริบ: LED บนบอร์ดติดครึ่งวินาที ดับครึ่งวินาที วนไปเรื่อย ๆ
// ESP32 DevKit ส่วนใหญ่ต่อ LED บนบอร์ดไว้ที่ GPIO 2 ถ้าบอร์ดของคุณเป็นขาอื่น แก้ LED_PIN ข้างล่าง
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdp_app.h"

#define LED_PIN GPIO_NUM_2

void app_start(void)
{
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    for (;;) {
        gpio_set_level(LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
