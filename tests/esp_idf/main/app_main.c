/* ESP-IDF runner for the host Unity suite (tests/test_list.c). Prints a
 * sentinel line when done, so a QEMU run can be stopped by a script. */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "test_list.h"

#include <inttypes.h>
#include <stdio.h>

void app_main(void) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* On hardware, give the host time to reopen the port after the reset. */
    vTaskDelay(pdMS_TO_TICKS(2000));
#else
    vTaskDelay(pdMS_TO_TICKS(100)); /* let the boot log drain */
#endif
    int64_t t0 = esp_timer_get_time();
    int fails = epi_run_all_tests();
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    printf("EPI_TESTS_DONE fails=%d ms=%" PRId64 "\n", fails, ms);
    fflush(stdout);
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}
