#include "hardware/pwm.h"
#include "pico/stdlib.h"
#include "file.h"

const int16_t NOISE_THRESHOLD = 10000; // adjust as needed


void setup_pwm(const uint gpio, const uint bitsPerSample)
{
    gpio_set_function(gpio, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(gpio);
    pwm_set_enabled(slice_num, true);
    pwm_set_wrap(slice_num, (1 << bitsPerSample) - 1);
}

void play_wav_pwm(const WavFile &wav, const uint pwm_gpio)
{
    setup_pwm(pwm_gpio, wav.header.bitsPerSample);
    uint slice_num = pwm_gpio_to_slice_num(pwm_gpio);

    FIL fil;
    FRESULT fr = f_open(&fil, wav.path.c_str(), FA_READ);
    if (fr != FR_OK)
        return;

    // Skip header
    UINT br;
    f_lseek(&fil, wav.dataStartPosition);

    // Stream audio
    pwm_set_gpio_level(pwm_gpio, 0);

    for (uint32_t i = 0; i < wav.dataSize; i++)
    {
        if (wav.header.bitsPerSample == 8)
        {
            uint8_t sample;
            f_read(&fil, &sample, 1, &br);
            pwm_set_gpio_level(pwm_gpio, sample);
        }
        else if (wav.header.bitsPerSample == 16)
        {
            uint8_t bytes[2];
            f_read(&fil, bytes, 2, &br);
            uint16_t sample = bytes[0] | bytes[1]; //bytes[0] | (bytes[1] << 8); // Little-endian
          if (abs(sample) < NOISE_THRESHOLD) {
    pwm_set_gpio_level(pwm_gpio, 0); // output silence (center value)
} else {
    pwm_set_gpio_level(pwm_gpio, abs(sample));
}
        }
        sleep_us(1000000 / wav.header.SamplesPerSec); // crude timing
    }

    pwm_set_gpio_level(pwm_gpio, 0);

    f_close(&fil);
}