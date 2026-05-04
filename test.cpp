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
// Directory on the SD card that stores incoming recordings
static const char *RECORDINGS_DIR = "0:/recordings";
// Directory on the SD card that contains the greeting prompt(s)
static const char *PROMPTS_DIR = "0:/prompts";

// True while the handset is off the hook (phone in use).
static volatile bool phone_off_hook = false;

// Interrupt handler for the phone hook GPIO.
// Fires on both edges so we can track the current hook state at all times,
// including stopping an in-progress recording the moment the phone is replaced.
static void phone_hook_irq(uint gpio, uint32_t events)
{
    if (gpio != PHONE_HOOK_GPIO)
        return;

    if (events & GPIO_IRQ_EDGE_RISE)
    {
        // Circuit opens → handset replaced (on hook)
        phone_off_hook = false;
        stop_playback();  // abort any in-progress greeting
        stop_recording(); // safe to call from an interrupt handler
    }
    if (events & GPIO_IRQ_EDGE_FALL)
    {
        // Circuit closes → handset lifted (off hook)
        phone_off_hook = true;
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
    printf("Pi Pico Audio Guestbook starting\n");

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

    // Read the current hook state *after* enabling the interrupt so that any
    // edge that occurred during initialisation is not missed.
    phone_off_hook = !gpio_get(PHONE_HOOK_GPIO);

    // ------------------------------------------------------------------
    // SD card
    // ------------------------------------------------------------------
    sd_card_t *pSD = sd_get_by_num(0);
    FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
    if (fr != FR_OK)
    {
        printf("ERROR: Could not mount filesystem (%d)\n", fr);
        return -1;
    }

    // Create the recordings directory if it does not exist yet.
    f_mkdir(RECORDINGS_DIR);

    printf("Guestbook ready – waiting for phone pick-up\n");

    // ------------------------------------------------------------------
    // Main guestbook loop – runs continuously until power is removed.
    // ------------------------------------------------------------------
    while (true)
    {
        // Wait until the handset is lifted.
        // __wfi() puts the CPU into a low-power sleep until the next interrupt
        // fires (i.e. the GPIO IRQ from the hook switch), consuming far less
        // energy than polling with sleep_ms().
        while (!phone_off_hook)
        {
            __wfi();
        }

        printf("Phone off hook – playing greeting\n");

        // Play the first available greeting prompt from the SD card.
        vector<unique_ptr<File>> prompts = list_files_in_folder(
            pSD, PROMPTS_DIR, std::make_unique<FileType>(FileType::Wav));
        if (!prompts.empty())
        {
            WavFile *wav = static_cast<WavFile *>(prompts[0].get());
            play_wav_pwm(*wav, AUDIO_OUT_GPIO);
        }
        else
        {
            printf("No prompt found in '%s' – skipping to record\n", PROMPTS_DIR);
        }

        // Only start recording if the handset is still off the hook after
        // the prompt has finished playing.
        if (!phone_off_hook)
        {
            printf("Phone replaced before recording started\n");
            continue;
        }

        // Record until the handset is replaced.
        // phone_hook_irq() calls stop_recording() on the rising edge,
        // which causes record_wav_adc() to finalise the file and return.
        string path = next_recording_path();
        printf("Recording to: %s\n", path.c_str());
        record_wav_adc(path, MIC_ADC_GPIO, RECORD_SAMPLE_RATE);
        printf("Recording saved: %s\n", path.c_str());

        // Wait for the handset to be fully replaced before listening for
        // the next pick-up event.  __wfi() keeps the CPU in a low-power
        // sleep state until the hook-switch GPIO IRQ fires.
        while (phone_off_hook)
        {
            __wfi();
        }
    }

    // Unreachable, but included for completeness.
    f_unmount(pSD->pcName);
    return 0;
}
