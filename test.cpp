#include <stdio.h>
#include <vector>
#include "pico/stdlib.h"
#include "sd_card.h"
#include "ff.h"
#include "hw_config.h"
#include "file_handlers.h"
#include "audio.h"

using std::vector;

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

    vector<unique_ptr<File>> wav_files = list_files_in_folder(pSD, "prompts", std::make_unique<FileType>(FileType::Wav));

    if(wav_files.size() > 0)
    {
        WavFile* wav_file = static_cast<WavFile*>(wav_files[0].get());

        WAV_HEADER header = wav_file->header;

        play_wav_pwm(*wav_file, 18);
    }

    // Unmount drive
    f_unmount(pSD->pcName);
    return 0;
}
