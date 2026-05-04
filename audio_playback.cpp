#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "file.h"
#include <algorithm> // for std::clamp
#include <cstring>   // for memmove
#include <cmath>     // for sinf

// Double-buffer for SD reads (core 1) and PWM playback (core 0)
static const uint32_t BUFFER_SIZE = 8192;
static uint8_t audio_buffer_a[BUFFER_SIZE];
static uint8_t audio_buffer_b[BUFFER_SIZE];
static volatile uint32_t buffer_a_bytes = 0;
static volatile uint32_t buffer_b_bytes = 0;
static FIL *g_playback_fil = nullptr;

void setup_pwm(const uint gpio, const uint bitsPerSample, uint32_t sampleRate)
{
    gpio_set_function(gpio, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(gpio);
    
    // Use 12-bit resolution for better audio quality
    // 125MHz / 4096 = ~30.5kHz PWM frequency (above audible range)
    pwm_set_wrap(slice_num, 4095); // 12-bit resolution (0-4095)
    pwm_set_clkdiv(slice_num, 1.0f); // Full speed
    
    // Start at center for 12-bit
    pwm_set_gpio_level(gpio, 2048);
    pwm_set_enabled(slice_num, true);
}

// Core 1 entry: handles all SD reads during playback so core 0 is never
// stalled for the duration of an f_read call.
//
// Protocol (inter-core FIFO):
//   core 0 → core 1: 1 = fill audio_buffer_a
//                    2 = fill audio_buffer_b
//                    0 = stop (core 1 acks with 0 then returns)
//   core 1 → core 0: echoes the cmd number when the buffer is ready
static void core1_sd_reader()
{
    while (true)
    {
        uint32_t cmd = multicore_fifo_pop_blocking();
        if (cmd == 0)
        {
            multicore_fifo_push_blocking(0); // ack
            return;
        }
        UINT br;
        if (cmd == 1)
        {
            f_read(g_playback_fil, audio_buffer_a, BUFFER_SIZE, &br);
            buffer_a_bytes = br;
        }
        else
        {
            f_read(g_playback_fil, audio_buffer_b, BUFFER_SIZE, &br);
            buffer_b_bytes = br;
        }
        multicore_fifo_push_blocking(cmd); // signal core 0: buffer is ready
    }
}

void play_wav_pwm(const WavFile &wav, const uint pwm_gpio)
{
    setup_pwm(pwm_gpio, wav.header.bitsPerSample, wav.header.SamplesPerSec);

    printf("Playing WAV: %d Hz, %d-bit, %d channels\n",
           wav.header.SamplesPerSec, wav.header.bitsPerSample, wav.header.NumOfChannels);
    printf("Data size: %d bytes\n", wav.dataSize);

    FIL fil;
    FRESULT fr = f_open(&fil, wav.path.c_str(), FA_READ);
    if (fr != FR_OK)
    {
        printf("ERROR: Could not open file\n");
        return;
    }

    f_lseek(&fil, wav.dataStartPosition);

    UINT br;
    uint32_t bytes_per_frame = (wav.header.bitsPerSample / 8) * wav.header.NumOfChannels;
    uint32_t total_frames    = wav.dataSize / bytes_per_frame;

    // Pre-fill buffer A on core 0 before launching core 1
    fr = f_read(&fil, audio_buffer_a, BUFFER_SIZE, &br);
    buffer_a_bytes = br;
    if (fr != FR_OK || buffer_a_bytes == 0)
    {
        pwm_set_gpio_level(pwm_gpio, 2048);
        f_close(&fil);
        return;
    }

    // Hand the file handle to core 1 and start it
    g_playback_fil = &fil;
    multicore_reset_core1();
    multicore_launch_core1(core1_sd_reader);

    // Ask core 1 to fill buffer B immediately while we play buffer A
    multicore_fifo_push_blocking(2);

    uint64_t start_time = time_us_64();

    uint8_t  *active_buf          = audio_buffer_a;
    uint32_t  active_bytes        = buffer_a_bytes;
    uint8_t  *pending_buf         = audio_buffer_b;
    bool      pending_read_active = true; // core 1 is filling pending_buf
    uint32_t  buf_offset          = 0;

    printf("Starting playback...\n");

    for (uint32_t frame = 0; frame < total_frames; frame++)
    {
        // If the active buffer is exhausted, swap in the pending one
        if (buf_offset + bytes_per_frame > active_bytes)
        {
            if (!pending_read_active)
                break;

            // Block until core 1 has finished filling the pending buffer.
            // Core 1 had a full buffer duration (~256 ms at 16 kHz) of headroom
            // so this almost never stalls.
            multicore_fifo_pop_blocking();

            uint32_t pending_bytes = (pending_buf == audio_buffer_b)
                                     ? buffer_b_bytes : buffer_a_bytes;

            if (pending_bytes == BUFFER_SIZE)
            {
                // More data exists: ask core 1 to refill the just-played buffer
                uint32_t refill_cmd = (active_buf == audio_buffer_a) ? 1 : 2;
                multicore_fifo_push_blocking(refill_cmd);
            }
            else
            {
                pending_read_active = false; // last chunk
            }

            uint8_t *old_active = active_buf;
            active_buf   = pending_buf;
            active_bytes = pending_bytes;
            pending_buf  = old_active;
            buf_offset   = 0;

            if (active_bytes == 0)
                break;
        }

        // Output one sample from the active buffer
        if (wav.header.bitsPerSample == 16)
        {
            if (buf_offset + bytes_per_frame <= active_bytes)
            {
                int16_t sample = (int16_t)(active_buf[buf_offset] | (active_buf[buf_offset + 1] << 8));
                buf_offset += 2;

                if (wav.header.NumOfChannels == 2)
                {
                    int16_t sample_r = (int16_t)(active_buf[buf_offset] | (active_buf[buf_offset + 1] << 8));
                    buf_offset += 2;
                    sample = (sample + sample_r) / 2;
                }

                uint16_t pwm_value = ((sample >> 4) + 2048) & 0xFFF;
                pwm_set_gpio_level(pwm_gpio, pwm_value);
            }
            else
            {
                pwm_set_gpio_level(pwm_gpio, 2048);
            }
        }
        else if (wav.header.bitsPerSample == 8)
        {
            if (buf_offset < active_bytes)
            {
                uint8_t sample = active_buf[buf_offset++];

                if (wav.header.NumOfChannels == 2 && buf_offset < active_bytes)
                {
                    uint8_t sample_r = active_buf[buf_offset++];
                    sample = (sample + sample_r) / 2;
                }

                uint16_t pwm_value = (sample << 4) | (sample >> 4);
                pwm_set_gpio_level(pwm_gpio, pwm_value);
            }
            else
            {
                pwm_set_gpio_level(pwm_gpio, 2048);
            }
        }

        // Absolute timing prevents cumulative drift
        uint64_t next_sample_time = start_time + ((uint64_t)(frame + 1) * 1000000ULL / wav.header.SamplesPerSec);
        while (time_us_64() < next_sample_time)
            tight_loop_contents();
    }

    // Drain any pending core 1 response before sending the stop command,
    // otherwise both cores would deadlock trying to push simultaneously.
    if (pending_read_active)
        multicore_fifo_pop_blocking();

    multicore_fifo_push_blocking(0); // stop core 1
    multicore_fifo_pop_blocking();   // wait for ack
    multicore_reset_core1();

    pwm_set_gpio_level(pwm_gpio, 2048);
    // Disable the PWM slice so the ~30.5 kHz carrier stops switching.
    // Leaving it running couples HF noise into the 3.3 V supply and from
    // there into the MAX4466 / ADC during recording.
    pwm_set_enabled(pwm_gpio_to_slice_num(pwm_gpio), false);
    // Return the pin to a plain low output so it doesn't float.
    gpio_set_function(pwm_gpio, GPIO_FUNC_SIO);
    gpio_set_dir(pwm_gpio, GPIO_OUT);
    gpio_put(pwm_gpio, 0);
    f_close(&fil);

    printf("Playback complete\n");
}

void play_tone_pwm(const uint pwm_gpio, const uint32_t frequency_hz, const uint32_t duration_ms, float volume)
{
    // Clamp volume to [0.0, 1.0].
    volume = std::clamp(volume, 0.0f, 1.0f);

    // Configure PWM the same way as audio playback: 12-bit resolution,
    // full system-clock speed, centre value = 2048.
    gpio_set_function(pwm_gpio, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(pwm_gpio);
    pwm_set_wrap(slice_num, 4095);
    pwm_set_clkdiv(slice_num, 1.0f);
    pwm_set_gpio_level(pwm_gpio, 2048);
    pwm_set_enabled(slice_num, true);

    // Synthesise a sine wave at the requested frequency.
    const uint32_t sample_rate   = 16000;
    const uint32_t total_samples = (sample_rate * duration_ms) / 1000;
    const float    two_pi_f      = 2.0f * (float)M_PI * (float)frequency_hz;
    const float    amplitude     = 2047.0f * volume;

    uint64_t start_time = time_us_64();

    for (uint32_t i = 0; i < total_samples; i++)
    {
        float    t         = (float)i / (float)sample_rate;
        float    sin_val   = sinf(two_pi_f * t);
        uint16_t pwm_value = (uint16_t)(2048.0f + amplitude * sin_val);
        pwm_set_gpio_level(pwm_gpio, pwm_value);

        // Absolute timing to prevent cumulative drift.
        uint64_t next_time = start_time + ((uint64_t)(i + 1) * 1000000ULL / sample_rate);
        while (time_us_64() < next_time)
            tight_loop_contents();
    }

    // Shut down PWM cleanly – same pattern as play_wav_pwm().
    pwm_set_gpio_level(pwm_gpio, 2048);
    pwm_set_enabled(slice_num, false);
    gpio_set_function(pwm_gpio, GPIO_FUNC_SIO);
    gpio_set_dir(pwm_gpio, GPIO_OUT);
    gpio_put(pwm_gpio, 0);

    printf("Tone complete\n");
}