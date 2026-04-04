#include "hardware/pwm.h"
#include "hardware/timer.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "file.h"

// ---------------------------------------------------------------------------
// Double-buffer constants
// Each buffer holds ~23ms at 44100 Hz 16-bit stereo, giving Core 1 ample
// time to complete an SD read before the ISR needs the next buffer.
// ---------------------------------------------------------------------------
#define AUDIO_BUF_SIZE 4096

typedef enum { BUF_EMPTY = 0, BUF_READY = 1 } BufState;

// Shared between Core 0 (timer ISR) and Core 1 (SD reader).
// All fields accessed by both cores are volatile; 32-bit aligned writes on
// Cortex-M0+ are atomic so no spinlock is needed for the state transitions.
static uint8_t           s_bufs[2][AUDIO_BUF_SIZE];
static volatile uint32_t s_buf_state[2];  // BufState
static volatile uint32_t s_buf_size[2];   // valid bytes in each buffer

static volatile int      s_active_buf;    // which buffer the ISR is consuming
static volatile uint32_t s_read_pos;      // byte offset within the active buffer

static volatile bool s_eof;              // Core 1 reached end-of-file
static volatile bool s_playback_done;   // ISR finished draining all samples
static volatile bool s_stop_core1;      // signal Core 1 to exit its loop

// WAV format info written once by play_wav_pwm before any core is launched
static uint16_t s_bits;
static uint16_t s_channels;
static uint32_t s_bytes_per_frame;
static uint     s_pwm_gpio;

static FIL      s_fil;
static uint     s_alarm_num;
static uint32_t s_sample_rate;
static uint64_t s_start_time;
static uint64_t s_sample_count;

// ---------------------------------------------------------------------------
// Hardware alarm callback (Core 0) — fires once per audio sample.
// Each sample is scheduled from an absolute start time using 64-bit
// arithmetic, so sub-microsecond rounding errors never accumulate.
// ---------------------------------------------------------------------------
static void audio_alarm_callback(uint alarm_num)
{
    int buf = s_active_buf;

    // Switch to the next buffer when the current one is exhausted
    if (s_read_pos + s_bytes_per_frame > s_buf_size[buf])
    {
        s_buf_state[buf] = BUF_EMPTY;  // release to Core 1 for refill
        buf ^= 1;
        s_active_buf = buf;
        s_read_pos = 0;

        if (s_buf_state[buf] != BUF_READY)
        {
            if (s_eof)
            {
                // Both buffers drained and no more file data — done
                s_playback_done = true;
                return;  // do not reschedule
            }
            // Core 1 hasn't finished the refill yet; output silence this tick
            pwm_set_gpio_level(s_pwm_gpio, 2048);
            s_sample_count++;
            hardware_alarm_set_target(s_alarm_num,
                from_us_since_boot(s_start_time + s_sample_count * 1000000ULL / s_sample_rate));
            return;
        }
    }

    // Output one frame from the active buffer
    const uint8_t *p = s_bufs[buf] + s_read_pos;
    if (s_bits == 16)
    {
        int16_t L = (int16_t)(p[0] | (p[1] << 8));
        s_read_pos += 2;
        if (s_channels == 2)
        {
            int16_t R = (int16_t)(p[2] | (p[3] << 8));
            s_read_pos += 2;
            L = (L + R) / 2;
        }
        pwm_set_gpio_level(s_pwm_gpio, ((L >> 4) + 2048) & 0xFFF);
    }
    else
    {
        uint8_t L = p[0];
        s_read_pos++;
        if (s_channels == 2)
        {
            L = (L + p[1]) / 2;
            s_read_pos++;
        }
        pwm_set_gpio_level(s_pwm_gpio, (uint16_t)((L << 4) | (L >> 4)));
    }

    // Schedule the next sample at exactly the right absolute time
    s_sample_count++;
    hardware_alarm_set_target(s_alarm_num,
        from_us_since_boot(s_start_time + s_sample_count * 1000000ULL / s_sample_rate));
}

// ---------------------------------------------------------------------------
// Core 1 entry — SD card reader (producer)
// ---------------------------------------------------------------------------
static void core1_sd_reader()
{
    while (!s_stop_core1)
    {
        // Find an empty buffer slot to refill
        int i = -1;
        if      (s_buf_state[0] == BUF_EMPTY) i = 0;
        else if (s_buf_state[1] == BUF_EMPTY) i = 1;

        if (i < 0)
        {
            tight_loop_contents();  // both buffers full — wait
            continue;
        }

        UINT br;
        f_read(&s_fil, s_bufs[i], AUDIO_BUF_SIZE, &br);
        s_buf_size[i] = br;

        if (br == 0)
        {
            s_eof = true;
            break;
        }

        s_buf_state[i] = BUF_READY;

        if (br < AUDIO_BUF_SIZE)
        {
            s_eof = true;  // partial read means end of file
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// PWM setup
// ---------------------------------------------------------------------------
void setup_pwm(const uint gpio, const uint bitsPerSample, uint32_t sampleRate)
{
    gpio_set_function(gpio, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(gpio);

    // 125 MHz / 4096 ≈ 30.5 kHz PWM carrier — above the audible range.
    // An RC low-pass filter on the GPIO pin (~1 kΩ + 10–100 nF) is required
    // to convert the modulated square wave into a smooth audio signal.
    pwm_set_wrap(slice_num, 4095);    // 12-bit resolution
    pwm_set_clkdiv(slice_num, 1.0f);
    pwm_set_gpio_level(gpio, 2048);   // centre / silence
    pwm_set_enabled(slice_num, true);
}

// ---------------------------------------------------------------------------
// Public playback entry point
// ---------------------------------------------------------------------------
void play_wav_pwm(const WavFile &wav, const uint pwm_gpio)
{
    setup_pwm(pwm_gpio, wav.header.bitsPerSample, wav.header.SamplesPerSec);

    printf("Playing WAV: %u Hz, %u-bit, %u channels\n",
           wav.header.SamplesPerSec, wav.header.bitsPerSample, wav.header.NumOfChannels);
    printf("Data size: %u bytes\n", wav.dataSize);

    FRESULT fr = f_open(&s_fil, wav.path.c_str(), FA_READ);
    if (fr != FR_OK)
    {
        printf("ERROR: Could not open file\n");
        return;
    }
    f_lseek(&s_fil, wav.dataStartPosition);

    // Initialise shared state before launching Core 1 or the timer
    s_bits            = wav.header.bitsPerSample;
    s_channels        = wav.header.NumOfChannels;
    s_bytes_per_frame = (wav.header.bitsPerSample / 8) * wav.header.NumOfChannels;
    s_pwm_gpio        = pwm_gpio;
    s_active_buf      = 0;
    s_read_pos        = 0;
    s_eof             = false;
    s_playback_done   = false;
    s_stop_core1      = false;
    s_buf_state[0]    = BUF_EMPTY;
    s_buf_state[1]    = BUF_EMPTY;
    s_buf_size[0]     = 0;
    s_buf_size[1]     = 0;

    // Pre-fill both buffers from Core 0 so the ISR has data immediately
    for (int i = 0; i < 2 && !s_eof; i++)
    {
        UINT br;
        f_read(&s_fil, s_bufs[i], AUDIO_BUF_SIZE, &br);
        s_buf_size[i]  = br;
        s_buf_state[i] = (br > 0) ? BUF_READY : BUF_EMPTY;
        if (br < AUDIO_BUF_SIZE) s_eof = true;
    }

    // Core 1 keeps refilling whichever buffer the ISR has just emptied
    multicore_launch_core1(core1_sd_reader);

    printf("Starting playback...\n");

    // Claim a hardware alarm and schedule the first sample.
    // Subsequent samples self-schedule inside audio_alarm_callback using
    // absolute time: start + N * 1000000 / sample_rate, so sub-microsecond
    // rounding errors never accumulate across samples.
    s_sample_rate = wav.header.SamplesPerSec;
    s_sample_count = 1;
    s_alarm_num = hardware_alarm_claim_unused(true);
    hardware_alarm_set_callback(s_alarm_num, audio_alarm_callback);
    s_start_time = time_us_64();
    hardware_alarm_set_target(s_alarm_num,
        from_us_since_boot(s_start_time + 1000000ULL / s_sample_rate));

    // Spin on Core 0 — the audio ISR fires as a hardware interrupt here
    while (!s_playback_done)
    {
        tight_loop_contents();
    }

    hardware_alarm_unclaim(s_alarm_num);

    s_stop_core1 = true;
    multicore_reset_core1();

    pwm_set_gpio_level(pwm_gpio, 2048);  // silence
    f_close(&s_fil);

    printf("Playback complete\n");
}