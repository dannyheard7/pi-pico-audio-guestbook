#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "file.h"
#include <cstring> // for memmove

// Double buffer system for smooth audio playback
static uint16_t audio_buffer_a[4096];
static uint16_t audio_buffer_b[4096];
static volatile bool buffer_a_ready = false;
static volatile bool buffer_b_ready = false;
static volatile bool playback_active = false;

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

void play_wav_pwm(const WavFile &wav, const uint pwm_gpio)
{
    setup_pwm(pwm_gpio, wav.header.bitsPerSample, wav.header.SamplesPerSec);

    // Debug: print WAV file info
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

    // Seek to audio data start
    f_lseek(&fil, wav.dataStartPosition);

    UINT br;
    uint32_t bytes_per_frame = (wav.header.bitsPerSample / 8) * wav.header.NumOfChannels;
    uint32_t total_frames = wav.dataSize / bytes_per_frame;
    
    // Calculate sample timing - use hardware timer for precise playback
    uint32_t sample_period_us = 1000000 / wav.header.SamplesPerSec;
    
    // Use much larger buffers - 8KB each for double buffering
    const uint32_t BUFFER_SIZE = 8192;
    uint8_t* read_buffer = (uint8_t*)malloc(BUFFER_SIZE);
    if (!read_buffer) {
        printf("ERROR: Could not allocate buffer\n");
        f_close(&fil);
        return;
    }
    
    uint32_t buffer_index = BUFFER_SIZE; // Start empty
    uint32_t bytes_in_buffer = 0;
    
    // Use absolute timing to prevent drift
    uint64_t start_time = time_us_64();
    uint64_t next_sample_time = start_time;
    
    // Pre-fill buffer before starting
    fr = f_read(&fil, read_buffer, BUFFER_SIZE, &br);
    bytes_in_buffer = br;
    buffer_index = 0;
    
    printf("Starting playback...\n");
    
    // Stream audio frames with improved buffering
    for (uint32_t frame = 0; frame < total_frames && fr == FR_OK; frame++)
    {
        // Refill buffer when we're running low (not empty)
        // This gives us time to read without disrupting playback
        if (buffer_index + bytes_per_frame * 512 > bytes_in_buffer && bytes_in_buffer == BUFFER_SIZE) 
        {
            // Copy remaining data to start of buffer
            uint32_t remaining = bytes_in_buffer - buffer_index;
            if (remaining > 0) {
                memmove(read_buffer, read_buffer + buffer_index, remaining);
            }
            
            // Read more data
            fr = f_read(&fil, read_buffer + remaining, BUFFER_SIZE - remaining, &br);
            bytes_in_buffer = remaining + br;
            buffer_index = 0;
        }
        
        if (wav.header.bitsPerSample == 16)
        {
            if (buffer_index + bytes_per_frame <= bytes_in_buffer)
            {
                // Read 16-bit signed sample (little-endian)
                int16_t sample = (int16_t)(read_buffer[buffer_index] | (read_buffer[buffer_index + 1] << 8));
                buffer_index += 2;
                
                // If stereo, average both channels for better quality
                if (wav.header.NumOfChannels == 2) {
                    int16_t sample_r = (int16_t)(read_buffer[buffer_index] | (read_buffer[buffer_index + 1] << 8));
                    buffer_index += 2;
                    sample = (sample + sample_r) / 2; // Average the channels
                }
                
                // Convert 16-bit signed (-32768 to 32767) to 12-bit PWM (0 to 4095)
                uint16_t pwm_value = ((sample >> 4) + 2048) & 0xFFF;
                pwm_set_gpio_level(pwm_gpio, pwm_value);
            }
            else
            {
                // Silence if we run out of data
                pwm_set_gpio_level(pwm_gpio, 2048);
            }
        }
        else if (wav.header.bitsPerSample == 8)
        {
            if (buffer_index < bytes_in_buffer)
            {
                // 8-bit unsigned PCM (0-255)
                uint8_t sample = read_buffer[buffer_index];
                buffer_index++;
                
                // If stereo, average both channels
                if (wav.header.NumOfChannels == 2 && buffer_index < bytes_in_buffer) {
                    uint8_t sample_r = read_buffer[buffer_index];
                    buffer_index++;
                    sample = (sample + sample_r) / 2;
                }
                
                // Convert 8-bit (0-255) to 12-bit (0-4095)
                uint16_t pwm_value = (sample << 4) | (sample >> 4); // Expand to 12-bit
                pwm_set_gpio_level(pwm_gpio, pwm_value);
            }
            else
            {
                // Silence
                pwm_set_gpio_level(pwm_gpio, 2048);
            }
        }
        
        // Calculate next sample time
        next_sample_time = start_time + ((frame + 1) * 1000000ULL / wav.header.SamplesPerSec);
        
        // Wait for the right time - but don't wait if we're already late
        uint64_t current_time = time_us_64();
        if (current_time < next_sample_time) {
            // Only sleep if we have more than 10us to wait
            int64_t wait_time = next_sample_time - current_time;
            if (wait_time > 10) {
                sleep_us(wait_time - 5); // Sleep most of it, then busy-wait
            }
            // Busy-wait for the last few microseconds for precision
            while (time_us_64() < next_sample_time) {
                tight_loop_contents();
            }
        } else if (current_time > next_sample_time + 1000) {
            // We're falling behind significantly - report it
            if (frame % 1000 == 0) {
                printf("Warning: audio timing drift at frame %lu\n", frame);
            }
        }
    }

    // Output center (silence) when done
    pwm_set_gpio_level(pwm_gpio, 2048);
    
    free(read_buffer);
    f_close(&fil);
    
    printf("Playback complete\n");
}