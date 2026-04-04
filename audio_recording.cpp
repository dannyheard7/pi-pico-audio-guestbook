#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/adc.h"
#include "pico/stdlib.h"
#include "file.h"
#include "audio.h"
#include <cstring>

using std::string;

// ---------------------------------------------------------------------------
// Analogue microphone recording via ADC + ping-pong DMA
// ---------------------------------------------------------------------------

// RP2040 ADC runs at 48 MHz; each conversion takes 96 ADC clock cycles.
static const uint32_t ADC_CLOCK_HZ              = 48000000;
static const uint32_t ADC_CYCLES_PER_CONVERSION = 96;

// Two equally-sized capture buffers that DMA alternates between.
// At 16 kHz mono the Pico fills one buffer every ~128 ms, giving the CPU
// ample time to write the other buffer to the SD card.
static const uint32_t RECORD_BUFFER_SAMPLES = 2048;
static uint16_t rec_buffer_a[RECORD_BUFFER_SAMPLES];
static uint16_t rec_buffer_b[RECORD_BUFFER_SAMPLES];

// Scratch buffer used to convert 12-bit unsigned ADC values to 16-bit signed
// PCM before writing to the file (avoids a large stack allocation).
static int16_t rec_convert_buf[RECORD_BUFFER_SAMPLES];

// DMA channel handles (-1 = not claimed).
static int rec_dma_chan_a = -1;
static int rec_dma_chan_b = -1;

// Flags set by the DMA IRQ when each buffer has been filled.
static volatile bool rec_buf_a_ready = false;
static volatile bool rec_buf_b_ready = false;

// Set to false to ask record_wav_adc() to stop.
static volatile bool recording_active = false;

// DMA IRQ handler – fires when either channel completes a buffer.
// Re-arms the completed channel so it is ready for its next turn in the
// ping-pong chain.
static void __isr adc_dma_irq_handler()
{
    if (rec_dma_chan_a >= 0 && (dma_hw->ints0 & (1u << rec_dma_chan_a)))
    {
        dma_hw->ints0 = (1u << rec_dma_chan_a); // clear interrupt
        rec_buf_a_ready = true;
        // Reset write address and transfer count so channel A is ready for
        // its next activation by channel B's chain_to.
        dma_channel_set_write_addr(rec_dma_chan_a, rec_buffer_a, false);
        dma_channel_set_trans_count(rec_dma_chan_a, RECORD_BUFFER_SAMPLES, false);
    }
    if (rec_dma_chan_b >= 0 && (dma_hw->ints0 & (1u << rec_dma_chan_b)))
    {
        dma_hw->ints0 = (1u << rec_dma_chan_b);
        rec_buf_b_ready = true;
        dma_channel_set_write_addr(rec_dma_chan_b, rec_buffer_b, false);
        dma_channel_set_trans_count(rec_dma_chan_b, RECORD_BUFFER_SAMPLES, false);
    }
}

// Convert a buffer of 12-bit unsigned ADC samples to 16-bit signed PCM and
// append them to the open WAV file.  Updates *data_size with bytes written.
static void write_adc_samples(FIL *file, const uint16_t *samples,
                               uint32_t count, uint32_t *data_size)
{
    for (uint32_t i = 0; i < count; i++)
    {
        // ADC result is 12-bit right-aligned (bits 11:0), value 0-4095.
        // Centre at 2048, then scale to signed 16-bit PCM:
        //   range: (0-2048)*16 = -32768  to  (4095-2048)*16 = 32752
        // Both extremes fit within int16_t (-32768 to 32767).
        rec_convert_buf[i] = (int16_t)(((int32_t)(samples[i] & 0x0FFF) - 2048) * 16);
    }
    UINT bw;
    f_write(file, rec_convert_buf, count * sizeof(int16_t), &bw);
    *data_size += bw;
}

void stop_recording()
{
    recording_active = false;
}

void record_wav_adc(const string output_path,
                    const uint adc_gpio, uint32_t sample_rate)
{
    printf("ADC recording: GPIO %u, %lu Hz -> %s\n",
           adc_gpio, (unsigned long)sample_rate, output_path.c_str());

    // ------------------------------------------------------------------
    // ADC setup
    // adc_gpio must be one of GPIO 26 (ADC0), 27 (ADC1), or 28 (ADC2).
    // ------------------------------------------------------------------
    adc_init();
    adc_gpio_init(adc_gpio);
    adc_select_input(adc_gpio - 26u);

    // Enable the FIFO and wire it to DMA.  No error bit, no 8-bit shift
    // (keep full 12-bit result right-aligned in a 16-bit word).
    adc_fifo_setup(true, true, 1, false, false);

    // Clock divider: clkdiv = (ADC_CLOCK_HZ / (sample_rate * ADC_CYCLES_PER_CONVERSION)) - 1
    float clkdiv = (float)ADC_CLOCK_HZ /
                   ((float)sample_rate * (float)ADC_CYCLES_PER_CONVERSION) - 1.0f;
    if (clkdiv < 0.0f) clkdiv = 0.0f;
    adc_set_clkdiv(clkdiv);

    // ------------------------------------------------------------------
    // Ping-pong DMA setup (channels A and B chain to each other)
    // ------------------------------------------------------------------
    rec_dma_chan_a = dma_claim_unused_channel(true);
    rec_dma_chan_b = dma_claim_unused_channel(true);

    // Channel A: fills rec_buffer_a, then triggers channel B
    dma_channel_config cfg_a = dma_channel_get_default_config(rec_dma_chan_a);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg_a, false);  // ADC FIFO is a fixed address
    channel_config_set_write_increment(&cfg_a, true);
    channel_config_set_dreq(&cfg_a, DREQ_ADC);
    channel_config_set_chain_to(&cfg_a, rec_dma_chan_b);

    // Channel B: fills rec_buffer_b, then triggers channel A
    dma_channel_config cfg_b = dma_channel_get_default_config(rec_dma_chan_b);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg_b, false);
    channel_config_set_write_increment(&cfg_b, true);
    channel_config_set_dreq(&cfg_b, DREQ_ADC);
    channel_config_set_chain_to(&cfg_b, rec_dma_chan_a);

    // Enable DMA IRQ0 for both channels and register the handler
    dma_channel_set_irq0_enabled(rec_dma_chan_a, true);
    dma_channel_set_irq0_enabled(rec_dma_chan_b, true);
    irq_set_exclusive_handler(DMA_IRQ_0, adc_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    // Pre-configure both channels (do not start yet)
    dma_channel_configure(rec_dma_chan_a, &cfg_a,
                          rec_buffer_a, &adc_hw->fifo,
                          RECORD_BUFFER_SAMPLES, false);
    dma_channel_configure(rec_dma_chan_b, &cfg_b,
                          rec_buffer_b, &adc_hw->fifo,
                          RECORD_BUFFER_SAMPLES, false);

    // ------------------------------------------------------------------
    // Create output WAV file and write placeholder header
    // ------------------------------------------------------------------
    FIL file;
    FRESULT fr = f_open(&file, output_path.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK)
    {
        printf("ERROR: Could not create recording file: %s (%d)\n",
               output_path.c_str(), fr);
        irq_set_enabled(DMA_IRQ_0, false);
        dma_channel_set_irq0_enabled(rec_dma_chan_a, false);
        dma_channel_set_irq0_enabled(rec_dma_chan_b, false);
        dma_channel_unclaim(rec_dma_chan_a);
        dma_channel_unclaim(rec_dma_chan_b);
        rec_dma_chan_a = rec_dma_chan_b = -1;
        return;
    }

    const uint16_t num_channels     = 1;
    const uint16_t bits_per_sample  = 16;

    WAV_HEADER header;
    memcpy(header.RIFF, "RIFF", 4);
    header.ChunkSize     = 0; // filled in at the end
    memcpy(header.WAVE, "WAVE", 4);
    memcpy(header.fmt,  "fmt ", 4);
    header.Subchunk1Size = 16;
    header.AudioFormat   = 1;  // PCM
    header.NumOfChannels = num_channels;
    header.SamplesPerSec = sample_rate;
    header.blockAlign    = num_channels * bits_per_sample / 8;
    header.bytesPerSec   = sample_rate * header.blockAlign;
    header.bitsPerSample = bits_per_sample;

    UINT bw;
    f_write(&file, &header, sizeof(WAV_HEADER), &bw);
    // "data" sub-chunk ID + placeholder size
    f_write(&file, "data", 4, &bw);
    uint32_t data_size = 0;
    f_write(&file, &data_size, sizeof(data_size), &bw);

    // ------------------------------------------------------------------
    // Start recording loop
    // ------------------------------------------------------------------
    rec_buf_a_ready  = false;
    rec_buf_b_ready  = false;
    recording_active = true;

    dma_channel_start(rec_dma_chan_a); // B will be chained automatically
    adc_run(true);

    printf("Recording started. Call stop_recording() to stop.\n");

    while (recording_active)
    {
        if (rec_buf_a_ready)
        {
            rec_buf_a_ready = false;
            write_adc_samples(&file, rec_buffer_a, RECORD_BUFFER_SAMPLES, &data_size);
        }
        if (rec_buf_b_ready)
        {
            rec_buf_b_ready = false;
            write_adc_samples(&file, rec_buffer_b, RECORD_BUFFER_SAMPLES, &data_size);
        }
        tight_loop_contents();
    }

    // ------------------------------------------------------------------
    // Stop ADC and DMA, release resources
    // ------------------------------------------------------------------
    adc_run(false);
    adc_fifo_drain();
    dma_channel_abort(rec_dma_chan_a);
    dma_channel_abort(rec_dma_chan_b);

    irq_set_enabled(DMA_IRQ_0, false);
    dma_channel_set_irq0_enabled(rec_dma_chan_a, false);
    dma_channel_set_irq0_enabled(rec_dma_chan_b, false);
    dma_channel_unclaim(rec_dma_chan_a);
    dma_channel_unclaim(rec_dma_chan_b);
    rec_dma_chan_a = rec_dma_chan_b = -1;

    // ------------------------------------------------------------------
    // Patch the WAV header with the final sizes and close the file
    // ------------------------------------------------------------------
    // ChunkSize = 4 ("WAVE") + 8 (fmt chunk header) + 16 (fmt data)
    //           + 8 ("data" chunk header) + data_size
    //           = 36 + data_size
    header.ChunkSize = 36 + data_size;
    f_lseek(&file, 0);
    f_write(&file, &header, sizeof(WAV_HEADER), &bw);

    // Patch the data sub-chunk size field (4 bytes after the "data" tag)
    f_lseek(&file, sizeof(WAV_HEADER) + 4);
    f_write(&file, &data_size, sizeof(data_size), &bw);

    f_close(&file);

    printf("Recording saved: %lu bytes of audio data\n", (unsigned long)data_size);
}
