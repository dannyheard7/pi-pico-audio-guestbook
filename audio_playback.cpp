#include "hardware/pwm.h"
#include "hardware/timer.h"
#include "pico/stdlib.h"
#include "file.h"

// ---------------------------------------------------------------------------
// Double-buffer constants.
// Each buffer holds ~46 ms at 44100 Hz 16-bit stereo (8192 / 4 / 44100 s).
// The main loop refills whichever buffer the ISR has just finished consuming,
// while the ISR plays uninterrupted from the other buffer.
// ---------------------------------------------------------------------------
#define AUDIO_BUF_SIZE 8192

typedef enum { BUF_EMPTY = 0, BUF_READY = 1 } BufState;

// Shared between the hardware-alarm ISR and the main loop on Core 0.
// 32-bit aligned writes on Cortex-M0+ are atomic; no spinlock needed.
static uint8_t           s_bufs[2][AUDIO_BUF_SIZE];
static volatile uint32_t s_buf_state[2];  // BufState
static volatile uint32_t s_buf_size[2];   // valid bytes in each buffer

static volatile int      s_active_buf;    // which buffer the ISR is consuming
static volatile uint32_t s_read_pos;      // byte offset within the active buffer

static volatile bool s_eof;              // main loop reached end-of-file
static volatile bool s_playback_done;   // ISR finished draining all samples

// WAV format info written once by play_wav_pwm before the alarm is started
static uint16_t s_bits;
static uint16_t s_channels;
static uint32_t s_bytes_per_frame;
static uint     s_pwm_gpio;

static uint     s_alarm_num;
static uint32_t s_sample_rate;
static uint64_t s_start_time;
static uint64_t s_sample_count;

// ---------------------------------------------------------------------------
// Hardware-alarm ISR (Core 0) — fires once per audio sample.
//
// Because this is a hardware interrupt it preempts the main loop's f_read()
// busy-wait at each sample tick, so audio output is perfectly timed even
// while the SD card is being read in the background.  Using an absolute
// start time prevents sub-microsecond rounding errors from accumulating.
// ---------------------------------------------------------------------------
static void audio_alarm_callback(uint alarm_num)
{
    int buf = s_active_buf;

    // Switch to the next buffer when the current one is exhausted
    if (s_read_pos + s_bytes_per_frame > s_buf_size[buf])
    {
        s_buf_state[buf] = BUF_EMPTY;  // release to main loop for refill
        buf ^= 1;
        s_active_buf = buf;
        s_read_pos   = 0;

        if (s_buf_state[buf] != BUF_READY)
        {
            if (s_eof)
            {
                // All data consumed — signal main loop to exit
                s_playback_done = true;
                return;  // do not reschedule
            }
            // Main loop hasn't finished the refill yet — output silence
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

    FIL fil;
    FRESULT fr = f_open(&fil, wav.path.c_str(), FA_READ);
    if (fr != FR_OK)
    {
        printf("ERROR: Could not open file\n");
        return;
    }
    f_lseek(&fil, wav.dataStartPosition);

    // Initialise shared state
    s_bits            = wav.header.bitsPerSample;
    s_channels        = wav.header.NumOfChannels;
    s_bytes_per_frame = (wav.header.bitsPerSample / 8) * wav.header.NumOfChannels;
    s_pwm_gpio        = pwm_gpio;
    s_active_buf      = 0;
    s_read_pos        = 0;
    s_eof             = false;
    s_playback_done   = false;
    s_buf_state[0]    = BUF_EMPTY;
    s_buf_state[1]    = BUF_EMPTY;
    s_buf_size[0]     = 0;
    s_buf_size[1]     = 0;

    // Pre-fill both buffers before starting the alarm so the ISR has data
    // immediately without any underrun on the very first sample
    for (int i = 0; i < 2 && !s_eof; i++)
    {
        UINT br;
        f_read(&fil, s_bufs[i], AUDIO_BUF_SIZE, &br);
        s_buf_size[i]  = br;
        s_buf_state[i] = (br > 0) ? BUF_READY : BUF_EMPTY;
        if (br < AUDIO_BUF_SIZE) s_eof = true;
    }

    printf("Starting playback...\n");

    // Claim a hardware alarm and fire the first sample callback.
    // All subsequent samples are self-scheduled inside audio_alarm_callback
    // using absolute time (start + N × 1000000 / sample_rate), so rounding
    // errors never accumulate.
    s_sample_rate  = wav.header.SamplesPerSec;
    s_sample_count = 1;
    s_alarm_num    = hardware_alarm_claim_unused(true);
    hardware_alarm_set_callback(s_alarm_num, audio_alarm_callback);
    s_start_time = time_us_64();
    hardware_alarm_set_target(s_alarm_num,
        from_us_since_boot(s_start_time + 1000000ULL / s_sample_rate));

    // Main loop: refill empty buffers on Core 0.
    // The hardware-alarm ISR preempts f_read()'s SPI busy-wait at every
    // sample tick, so audio output is never stalled by SD card reads.
    while (!s_playback_done)
    {
        for (int i = 0; i < 2; i++)
        {
            if (!s_eof && s_buf_state[i] == BUF_EMPTY)
            {
                UINT br;
                f_read(&fil, s_bufs[i], AUDIO_BUF_SIZE, &br);
                s_buf_size[i] = br;
                if (br < AUDIO_BUF_SIZE) s_eof = true;
                // DMB ensures s_buf_size is visible to the ISR before BUF_READY
                __dmb();
                // Mark ready last so the ISR never sees a half-filled buffer
                s_buf_state[i] = (br > 0) ? BUF_READY : BUF_EMPTY;
            }
        }
        tight_loop_contents();
    }

    hardware_alarm_unclaim(s_alarm_num);

    pwm_set_gpio_level(pwm_gpio, 2048);  // silence
    f_close(&fil);

    printf("Playback complete\n");
}
