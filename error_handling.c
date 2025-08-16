#include "pico/stdlib.h"

const uint LED_PIN = 25;

void initialiseErrorHandling()
{
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
}

void flashErrorLED()
{
    // Flash the error LED to indicate an error
    gpio_put(LED_PIN, 1);
    sleep_ms(5000);
    gpio_put(LED_PIN, 0);
}