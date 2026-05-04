#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/adc.h"
#include "pico/stdlib.h"
#include "file.h"
#include "audio.h"
#include <cstring>

using std::string;

// RP2040 ADC runs at 48 MHz; trigger period = (1 + clkdiv) ADC clock cycles.
static const uint32_t ADC_CLOCK_HZ = 48000000;

// Two equally-sized capture buffers that DMA alternates between.
// Each buffer holds RECORD_BUFFER_SAMPLES *output* samples worth of raw ADC
// data (RECORD_BUFFER_SAMPLES × OVERSAMPLE raw 16-bit words).
// At 22050 Hz mono with 4× oversampling the Pico fills one buffer every
// ~93 ms, giving the CPU ample time to write the other buffer to the SD card.
static const uint32_t RECORD_BUFFER_SAMPLES = 4096;

// Oversampling factor: the ADC captures OVERSAMPLE raw readings per output
// sample.  Averaging them reduces the noise floor by ~3 dB per doubling, so
// 4× oversampling yields roughly +6 dB SNR improvement over the raw ADC.
// OVERSAMPLE=16 requires 128 KB of static DMA buffers which exhausts the
// RP2040's 264 KB SRAM alongside FatFS and SD card buffers, corrupting
// DMA timing and causing the recorded pitch to be shifted downward.
// OVERSAMPLE=4 uses only 32 KB and keeps the ADC clock divider in a stable
// range (clkdiv≈4.67 at 22050 Hz, vs ≈0.42 with OVERSAMPLE=16).
static const uint32_t OVERSAMPLE = 4;
static const uint32_t RAW_BUFFER_SAMPLES = RECORD_BUFFER_SAMPLES * OVERSAMPLE;
static uint16_t rec_buffer_a[RAW_BUFFER_SAMPLES];
static uint16_t rec_buffer_b[RAW_BUFFER_SAMPLES];

// Scratch buffer used to convert 12-bit unsigned ADC values to 16-bit signed
// PCM before writing to the file (avoids a large stack allocation).
static int16_t rec_convert_buf[RECORD_BUFFER_SAMPLES];
// Running estimate of the ADC DC midpoint in Q8 fixed-point.
// MAX4466 boards bias the output near VCC/2, but the exact midpoint varies
// by module, gain setting, and supply voltage.
static int32_t rec_dc_midpoint_q8 = (2048 << 8);

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
    if (rec_dma_chan_a >= 0 && (dma_hw->ints1 & (1u << rec_dma_chan_a)))
    {
        dma_hw->ints1 = (1u << rec_dma_chan_a); // clear interrupt
        rec_buf_a_ready = true;
        // Reset write address and transfer count so channel A is ready for
        // its next activation by channel B's chain_to.
        dma_channel_set_write_addr(rec_dma_chan_a, rec_buffer_a, false);
        dma_channel_set_trans_count(rec_dma_chan_a, RAW_BUFFER_SAMPLES, false);
    }
    if (rec_dma_chan_b >= 0 && (dma_hw->ints1 & (1u << rec_dma_chan_b)))
    {
        dma_hw->ints1 = (1u << rec_dma_chan_b);
        rec_buf_b_ready = true;
        dma_channel_set_write_addr(rec_dma_chan_b, rec_buffer_b, false);
        dma_channel_set_trans_count(rec_dma_chan_b, RAW_BUFFER_SAMPLES, false);
    }
}

// Convert a buffer of oversampled 12-bit ADC readings to 16-bit signed PCM
// and append them to the open WAV file.  Every OVERSAMPLE consecutive raw
// readings are averaged into a single output sample, improving SNR.
// Updates *data_size with the number of bytes written.
static void write_adc_samples(FIL *file, const uint16_t *raw,
                               uint32_t raw_count, uint32_t *data_size)
{
    uint32_t out_count = raw_count / OVERSAMPLE;
    for (uint32_t i = 0; i < out_count; i++)
    {
        // Sum OVERSAMPLE consecutive 12-bit ADC readings
        int32_t sum = 0;
        for (uint32_t j = 0; j < OVERSAMPLE; j++)
            sum += (int32_t)(raw[i * OVERSAMPLE + j] & 0x0FFF);

        // Average and remove DC bias before scaling to signed 16-bit PCM.
        // Track the midpoint with a light low-pass filter so MAX4466 DC offset
        // changes do not get baked into the recording.
        int32_t avg = sum / (int32_t)OVERSAMPLE;
        int32_t avg_q8 = avg << 8;
        rec_dc_midpoint_q8 += (avg_q8 - rec_dc_midpoint_q8) >> 8;
        int32_t centred_q8 = avg_q8 - rec_dc_midpoint_q8;
        // Shift Q8 fixed-point down by 4 to scale the 12-bit ADC range (±2048)
        // to the full 16-bit signed PCM range (±32767).
        int32_t sample = centred_q8 >> 4;
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        rec_convert_buf[i] = (int16_t)sample;
    }
    UINT bw;
    f_write(file, rec_convert_buf, out_count * sizeof(int16_t), &bw);
    *data_size += bw;
}

void stop_recording()
{
    recording_active = false;
}

// Initialise ADC, DMA, IRQ and open the WAV file with a placeholder header.
// Returns true on success and leaves the file open; on failure any claimed
// DMA channels and IRQs are released and the function returns false.
static bool init_recording(const string &output_path,
                           const uint adc_gpio, uint32_t sample_rate,
                           FIL *file, WAV_HEADER *header, uint32_t *data_size)
{
    rec_dc_midpoint_q8 = (2048 << 8);

    adc_init();
    adc_set_temp_sensor_enabled(false);
    adc_gpio_init(adc_gpio);
    adc_select_input(adc_gpio - 26u);

    adc_fifo_setup(true, true, 1, false, false);
    adc_fifo_drain();

    float clkdiv = (float)ADC_CLOCK_HZ /
                   ((float)sample_rate * (float)OVERSAMPLE) - 1.0f;
    if (clkdiv < 0.0f) clkdiv = 0.0f;
    adc_set_clkdiv(clkdiv);

    rec_dma_chan_a = dma_claim_unused_channel(true);
    rec_dma_chan_b = dma_claim_unused_channel(true);

    dma_channel_config cfg_a = dma_channel_get_default_config(rec_dma_chan_a);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg_a, false);
    channel_config_set_write_increment(&cfg_a, true);
    channel_config_set_dreq(&cfg_a, DREQ_ADC);
    channel_config_set_chain_to(&cfg_a, rec_dma_chan_b);

    dma_channel_config cfg_b = dma_channel_get_default_config(rec_dma_chan_b);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg_b, false);
    channel_config_set_write_increment(&cfg_b, true);
    channel_config_set_dreq(&cfg_b, DREQ_ADC);
    channel_config_set_chain_to(&cfg_b, rec_dma_chan_a);

    dma_channel_set_irq1_enabled(rec_dma_chan_a, true);
    dma_channel_set_irq1_enabled(rec_dma_chan_b, true);
    irq_add_shared_handler(DMA_IRQ_1, adc_dma_irq_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_1, true);

    dma_channel_configure(rec_dma_chan_a, &cfg_a,
                          rec_buffer_a, &adc_hw->fifo,
                          RAW_BUFFER_SAMPLES, false);
    dma_channel_configure(rec_dma_chan_b, &cfg_b,
                          rec_buffer_b, &adc_hw->fifo,
                          RAW_BUFFER_SAMPLES, false);

    FRESULT fr = f_open(file, output_path.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK)
    {
        printf("ERROR: Could not create recording file: %s (%d)\n",
               output_path.c_str(), fr);
        irq_set_enabled(DMA_IRQ_1, false);
        irq_remove_handler(DMA_IRQ_1, adc_dma_irq_handler);
        dma_channel_set_irq1_enabled(rec_dma_chan_a, false);
        dma_channel_set_irq1_enabled(rec_dma_chan_b, false);
        dma_channel_unclaim(rec_dma_chan_a);
        dma_channel_unclaim(rec_dma_chan_b);
        rec_dma_chan_a = rec_dma_chan_b = -1;
        return false;
    }

    const uint16_t num_channels     = 1;
    const uint16_t bits_per_sample  = 16;

    memcpy(header->RIFF, "RIFF", 4);
    header->ChunkSize     = 0;
    memcpy(header->WAVE, "WAVE", 4);
    memcpy(header->fmt,  "fmt ", 4);
    header->Subchunk1Size = 16;
    header->AudioFormat   = 1;
    header->NumOfChannels = num_channels;
    header->SamplesPerSec = sample_rate;
    header->blockAlign    = num_channels * bits_per_sample / 8;
    header->bytesPerSec   = sample_rate * header->blockAlign;
    header->bitsPerSample = bits_per_sample;

    UINT bw;
    f_write(file, header, sizeof(WAV_HEADER), &bw);
    f_write(file, "data", 4, &bw);
    *data_size = 0;
    f_write(file, data_size, sizeof(*data_size), &bw);

    rec_buf_a_ready  = false;
    rec_buf_b_ready  = false;
    recording_active = true;

    dma_channel_start(rec_dma_chan_a);
    adc_run(true);

    return true;
}

// Finalise the recording: stop ADC/DMA, release resources and patch WAV header,
// then close the file.
static void finalize_recording(FIL *file, WAV_HEADER *header, uint32_t data_size)
{
    adc_run(false);
    adc_fifo_drain();
    dma_channel_abort(rec_dma_chan_a);
    dma_channel_abort(rec_dma_chan_b);

    irq_set_enabled(DMA_IRQ_1, false);
    irq_remove_handler(DMA_IRQ_1, adc_dma_irq_handler);
    dma_channel_set_irq1_enabled(rec_dma_chan_a, false);
    dma_channel_set_irq1_enabled(rec_dma_chan_b, false);
    dma_channel_unclaim(rec_dma_chan_a);
    dma_channel_unclaim(rec_dma_chan_b);
    rec_dma_chan_a = rec_dma_chan_b = -1;

    header->ChunkSize = 36 + data_size;
    f_lseek(file, 0);
    UINT bw;
    f_write(file, header, sizeof(WAV_HEADER), &bw);

    f_lseek(file, sizeof(WAV_HEADER) + 4);
    f_write(file, &data_size, sizeof(data_size), &bw);

    f_close(file);
}

void record_wav_adc(const string output_path,
                    const uint adc_gpio, uint32_t sample_rate)
{

    FIL file;
    WAV_HEADER header;
    uint32_t data_size = 0;

    if (!init_recording(output_path, adc_gpio, sample_rate, &file, &header, &data_size))
        return;

    // Discard the first two buffers so the DC tracking filter can settle
    // before any audio is written to file.  This avoids a loud thump at the
    // start of every recording caused by the filter initialising from 2048
    // when the actual MAX4466 midpoint may differ slightly.
    uint32_t discard_buffers = 2;

    while (recording_active)
    {
        if (rec_buf_a_ready)
        {
            rec_buf_a_ready = false;
            if (discard_buffers > 0)
                --discard_buffers;
            else
                write_adc_samples(&file, rec_buffer_a, RAW_BUFFER_SAMPLES, &data_size);
        }
        if (rec_buf_b_ready)
        {
            rec_buf_b_ready = false;
            if (discard_buffers > 0)
                --discard_buffers;
            else
                write_adc_samples(&file, rec_buffer_b, RAW_BUFFER_SAMPLES, &data_size);
        }
        // __wfi() puts the CPU into a low-power sleep until the DMA IRQ
        // (or the phone-hook GPIO IRQ that calls stop_recording()) fires,
        // avoiding a busy-spin that would otherwise keep the CPU at 100%.
        __wfi();
    }

    finalize_recording(&file, &header, data_size);
}
