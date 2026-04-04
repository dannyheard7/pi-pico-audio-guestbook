#include <stdio.h>
#include <vector>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "sd_card.h"
#include "ff.h"
#include "hw_config.h"
#include "file_handlers.h"
#include "audio.h"

using std::vector;

// GPIO connected to the analogue microphone output (ADC0)
static const uint MIC_ADC_GPIO   = 26;
// Sample rate used for recording
static const uint32_t RECORD_SAMPLE_RATE = 16000;
// Recording duration in milliseconds
static const uint32_t RECORD_DURATION_MS = 5000;
// Output path for the recording on the SD card
static const char *RECORDING_PATH = "0:/recordings/guestbook.wav";

// One-shot alarm callback – stops the recording after the configured duration.
static int64_t stop_recording_alarm_cb(alarm_id_t id, void *user_data)
{
    (void)id;
    (void)user_data;
    stop_recording();
    return 0; // do not reschedule
}

int main()
{
    // Initialize chosen serial port
    if (!stdio_init_all())
    {
        printf("ERROR: Could not initialize serial port\r\n");
        return -1;
    }

    sd_card_t *pSD = sd_get_by_num(0);
    FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
    if (fr != FR_OK)
    {
        printf("ERROR: Could not mount filesystem (%d)\r\n", fr);
        return -1;
    }

    // ------------------------------------------------------------------
    // Record from the analogue microphone for RECORD_DURATION_MS ms.
    // ------------------------------------------------------------------
    // Create the recordings directory if it does not exist yet.
    f_mkdir("0:/recordings");

    // Schedule a one-shot alarm to stop recording after the desired duration.
    alarm_id_t alarm = add_alarm_in_ms(RECORD_DURATION_MS,
                                       stop_recording_alarm_cb,
                                       NULL,
                                       true);

    record_wav_adc(pSD, RECORDING_PATH, MIC_ADC_GPIO, RECORD_SAMPLE_RATE);

    // Cancel the alarm in case recording was stopped early by another means.
    if (alarm > 0)
        cancel_alarm(alarm);

    // ------------------------------------------------------------------
    // Play back the recording that was just made.
    // ------------------------------------------------------------------
    unique_ptr<File> rec_file = File::Create(RECORDING_PATH);
    if (rec_file && rec_file->getType() == FileType::Wav)
    {
        WavFile *wav = static_cast<WavFile *>(rec_file.get());
        play_wav_pwm(*wav, 18);
    }
    else
    {
        printf("Could not open recording for playback: %s\n", RECORDING_PATH);
    }

    // ------------------------------------------------------------------
    // Play any existing prompts from the SD card.
    // ------------------------------------------------------------------
    vector<unique_ptr<File>> wav_files = list_files_in_folder(
        pSD, "prompts", std::make_unique<FileType>(FileType::Wav));

    if (wav_files.size() > 0)
    {
        WavFile *wav_file = static_cast<WavFile *>(wav_files[0].get());
        play_wav_pwm(*wav_file, 18);
    }

    // Unmount drive
    f_unmount(pSD->pcName);
    return 0;
}
