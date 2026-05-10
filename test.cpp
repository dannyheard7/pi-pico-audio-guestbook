#include <stdio.h>
#include <string>
#include <vector>
#include <memory>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "sd_card.h"
#include "ff.h"
#include "hw_config.h"
#include "file_handlers.h"
#include "audio.h"

using std::string;
using std::unique_ptr;
using std::vector;

// GPIO for the phone hook switch.
// Wire: one terminal → GPIO 22, other terminal → GND.
// Internal pull-up is enabled: LOW = on hook (circuit closed to GND),
// HIGH = off hook (circuit open, pulled up to 3.3 V).
static const uint PHONE_HOOK_GPIO = 22;
// GPIO connected to the analogue microphone output (ADC0)
static const uint MIC_ADC_GPIO = 26;
// GPIO for PWM audio output
static const uint AUDIO_OUT_GPIO = 18;
// Sample rate used for recording
static const uint32_t RECORD_SAMPLE_RATE = 16000;
// Milliseconds to wait after the handset is lifted before playing the greeting
static const uint32_t PICKUP_DELAY_MS = 1000;
// Volume of the ready tone: 0.0 (silent) to 1.0 (full scale)
static const float TONE_VOLUME = 0.2f;
// Directory on the SD card that stores incoming recordings
static const char *RECORDINGS_DIR = "0:/recordings";
// Directory on the SD card that contains the greeting prompt(s)
static const char *PROMPTS_DIR = "0:/prompts";

static inline bool is_on_hook()
{
    return gpio_get(PHONE_HOOK_GPIO);
}

// Interrupt handler for the phone hook GPIO.
// Fires on both edges so we can track the current hook state at all times,
// including stopping an in-progress recording the moment the phone is replaced.
static void phone_hook_irq(uint gpio, uint32_t events)
{
    if (gpio != PHONE_HOOK_GPIO)
        return;

    if (is_on_hook())
    {
        // Transitioned to on hook – abort any in-progress playback/recording.
        interrupt_playback();
        interrupt_recording();
    }
}

// Return the path to use for the next recording file.
// Scans for the first unused sequential name (recording_1.wav, recording_2.wav, …)
// so that existing recordings are never overwritten even if earlier files are deleted.
static string next_recording_path()
{
    size_t n = 1;
    string path;
    FILINFO finfo;
    do
    {
        path = string(RECORDINGS_DIR) + "/recording_" + std::to_string(n) + ".wav";
        n++;
    } while (f_stat(path.c_str(), &finfo) == FR_OK);
    return path;
}

int main()
{
    stdio_init_all();

    // ------------------------------------------------------------------
    // Phone hook GPIO – internal pull-up, active-low.
    // ------------------------------------------------------------------
    gpio_init(PHONE_HOOK_GPIO);
    gpio_set_dir(PHONE_HOOK_GPIO, GPIO_IN);
    gpio_pull_up(PHONE_HOOK_GPIO);

    gpio_set_irq_enabled_with_callback(
        PHONE_HOOK_GPIO,
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
        true,
        &phone_hook_irq);

    sd_card_t *pSD = sd_get_by_num(0);
    FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
    if (fr != FR_OK)
    {
        printf("ERROR: Could not mount filesystem (%d)\n", fr);
        return -1;
    }

    // Create the recordings directory if it does not exist yet.
    f_mkdir(RECORDINGS_DIR);

    // ------------------------------------------------------------------
    // Main guestbook loop – runs continuously until power is removed.
    // ------------------------------------------------------------------
    while (true)
    {
        // Wait until the handset is lifted.
        // __wfi() puts the CPU into a low-power sleep until the next interrupt
        // fires (i.e. the GPIO IRQ from the hook switch), consuming far less
        // energy than polling with sleep_ms().
        while (is_on_hook())
        {
            __wfi();
        }

        // Brief pause so the caller has a moment to bring the handset to
        // their ear before the greeting begins.
        sleep_ms(PICKUP_DELAY_MS);      
        
        if (is_on_hook())
        {
            continue;
        }

        // Play the first available greeting prompt from the SD card.
        vector<unique_ptr<File>> prompts = list_files_in_folder(
            pSD, PROMPTS_DIR, std::make_unique<FileType>(FileType::Wav));

        if (!prompts.empty())
        {
            WavFile *wav = static_cast<WavFile *>(prompts[0].get());
            play_wav_pwm(*wav, AUDIO_OUT_GPIO);
        }

        // Only start recording if the handset is still off the hook after
        // the prompt has finished playing.
        if (is_on_hook())
        {
            continue;
        }

        // Play a short beep so the caller knows the guestbook is ready to
        // record their message.
        play_tone_pwm(AUDIO_OUT_GPIO, 1000, 600, TONE_VOLUME);

        // Only start recording if the handset is still off the hook after
        // the tone has finished.
        if (is_on_hook())
        {
            continue;
        }

        // Record until the handset is replaced.
        string path = next_recording_path();
        record_wav_adc(path, MIC_ADC_GPIO, RECORD_SAMPLE_RATE);
    }

    f_unmount(pSD->pcName);
    return 0;
}

