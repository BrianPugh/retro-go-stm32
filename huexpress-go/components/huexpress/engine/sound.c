// sound.c - Sound emulation
//
#include "sound.h"
#include "pce.h"

static const uint8_t vol_tbl[32] = {
    100 >> 8, 451 >> 8, 508 >> 8, 573 >> 8, 646 >> 8, 728 >> 8, 821 >> 8, 925 >> 8,
    1043 >> 8, 1175 >> 8, 1325 >> 8, 1493 >> 8, 1683 >> 8, 1898 >> 8, 2139 >> 8, 2411 >> 8,
    2718 >> 8, 3064 >> 8, 3454 >> 8, 3893 >> 8, 4388 >> 8, 4947 >> 8, 5576 >> 8, 6285 >> 8,
    7085 >> 8, 7986 >> 8, 9002 >> 8, 10148 >> 8, 11439 >> 8, 12894 >> 8, 14535 >> 8, 16384 >> 8
};

static uint32_t da_index[PSG_CHANNELS];
static uint32_t fixed_n[PSG_CHANNELS];
static uint32_t noise_k[PSG_CHANNELS];
static uint32_t noise_rand[PSG_CHANNELS];
static int32_t noise_level[PSG_CHANNELS];

// The buffer should be signed but it seems to sound better
// unsigned. I am still reviewing the implementation bellow.
static uint8_t mix_buffer[48000 / 60 * 2];
// static int8_t mix_buffer[48000 / 60 * 2];


static inline void
psg_update(int8_t *buf, int ch, size_t dwSize)
{
    uint32_t Tp;
    int vol, sample;
    int8_t *buf_end = buf + dwSize;

    /*
    * Bail if there is no audio to be played on this channel.
    */
    if (!(PCE.PSG.regs[ch][PSG_DDA_REG] & PSG_DDA_ENABLE)) {
        fixed_n[ch] = 0;
        goto pad_and_return;
    }

    /*
    * We multiply the 4-bit balance values by 1.1 to get a result from (0..16.5).
    * This multiplied by the 5-bit channel volume (0..31) gives us a result of
    * (0..511).
    */
    int lbal = ((PCE.PSG.regs[ch][5] >> 4) * 1.1) * (PCE.PSG.regs[ch][4] & 0x1F);
    int rbal = ((PCE.PSG.regs[ch][5] & 0xF) * 1.1) * (PCE.PSG.regs[ch][4] & 0x1F);

    if (!host.sound.stereo) {
        lbal = (lbal + rbal) / 2;
    }

    /*
    * There is 'direct access' audio to be played.
    */
    if ((PCE.PSG.regs[ch][PSG_DDA_REG] & PSG_DDA_DIRECT_ACCESS) || PCE.PSG.da_count[ch]) {
        uint32_t index = da_index[ch] >> 16;
        /*
         * For this direct audio stuff there is no frequency provided via PSG registers 3
         * and 4.  I'm not sure if this is normal behaviour or if it's something wrong in
         * the emulation but I'm leaning toward the former.
         *
         * The 0x1FF divisor is completely arbitrary.  I adjusted it by listening to the voices
         * in Street Fighter 2 CE.  If anyone has information to improve my "seat of the pants"
         * calculations then by all means *does finger quotes* "throw me a frikkin` bone here".
         *
         * See the big comment in the final else clause for an explanation of this value
         * to the best of my knowledge.
         */
        uint32_t fixed_inc = ((CLOCK_PSG / host.sound.sample_freq) << 16) / 0x1FF;

        while ((buf < buf_end) && PCE.PSG.da_count[ch]) {
            /*
             * Make our sample data signed (-16..15) and then increment a non-negative
             * result otherwise a sample with a value of 10000b will not be reproduced,
             * which I do not believe is the correct behaviour.  Plus the increment
             * insures matching values on both sides of the wave.
             */
            if ((sample = PCE.PSG.da_data[ch][index] - 16) >= 0)
                sample++;

            /*
             * Multiply our sample value (-16..16) by our balance (0..511) and then divide
             * by 64 to get a final 8-bit output sample of (-127..127)
             */
            *buf++ = (int8_t) (sample * lbal / 64);

            if (host.sound.stereo) {
                *buf++ = (int8_t) (sample * rbal / 64);
            }

            da_index[ch] += fixed_inc;
            da_index[ch] &= 0x3FFFFFF;  /* (1023 << 16) + 0xFFFF */
            if ((da_index[ch] >> 16) != index) {
                index = da_index[ch] >> 16;
                PCE.PSG.da_count[ch]--;
            }
        }

        if (PCE.PSG.regs[ch][PSG_DDA_REG] & PSG_DDA_DIRECT_ACCESS) {
            goto pad_and_return;
        }
    }

    /*
    * PSG Noise generation (only available to PSG channels 5 and 6).
    */
    if ((ch > 3) && (PCE.PSG.regs[ch][PSG_NOISE_REG] & PSG_NOISE_ENABLE)) {
        int Np = (PCE.PSG.regs[ch][PSG_NOISE_REG] & 0x1F);

        vol = MAX((PCE.PSG.volume >> 3) & 0x1E, (PCE.PSG.volume << 1) & 0x1E) +
              (PCE.PSG.regs[ch][PSG_DDA_REG] & PSG_DDA_VOICE_VOLUME) +
              MAX((PCE.PSG.regs[ch][5] >> 3) & 0x1E, (PCE.PSG.regs[ch][5] << 1) & 0x1E);

        vol = vol_tbl[MAX(0, vol - 60)];

        while (buf < buf_end) {
            noise_k[ch] += 3000 + Np * 512;

            if ((Tp = (noise_k[ch] / host.sound.sample_freq)) >= 1) {
                if (noise_rand[ch] & 0x00080000) {
                    noise_rand[ch] = ((noise_rand[ch] ^ 0x0004) << 1) + 1;
                    noise_level[ch] = -10 * 702;
                } else {
                    noise_rand[ch] <<= 1;
                    noise_level[ch] = 10 * 702;
                }
                noise_k[ch] -= host.sound.sample_freq * Tp;
            }

            *buf++ = (int8_t) (noise_level[ch] * vol / 4096);
        }
        goto pad_and_return;
    }

    /*
    * PSG Wave generation.
    */
    if ((Tp = PCE.PSG.regs[ch][PSG_FREQ_LSB_REG] + (PCE.PSG.regs[ch][PSG_FREQ_MSB_REG] << 8)) > 0) {
        /*
         * Thank god for well commented code!  The original line of code read:
         * fixed_inc = ((uint32_t) (3.2 * 1118608 / host.sound.freq) << 16) / Tp;
         * and had nary a comment to be found.  It took a little head scratching to get
         * it figured out.  The 3.2 * 1118608 comes out to 3574595.6 which is obviously
         * meant to represent the 3.58mhz cpu clock speed used in the pc engine to
         * decrement the sound 'frequency'.  I haven't figured out why the original
         * author had the two numbers multiplied together to get the odd value instead of
         * just using 3580000.  I did some checking and the value will compute the same
         * using either value divided by any standard soundcard samplerate.  The
         * host.sound.freq is our soundcard's samplerate which is quite a bit slower than
         * the pce's cpu (3580000 vs. 22050/44100 typically).
         *
         * Taken from the PSG doc written by Paul Clifford (paul@plasma.demon.co.uk)
         * <in reference to the 12 bit frequency value in PSG registers 2 and 3>
         * "For waveform output, a copy of this value is, in effect, decremented 3,580,000
         *  times a second until zero is reached.  When this happens the PSG advances an
         *  internal pointer into the channel's waveform buffer by one."
         *
         * So all we need to do to emulate original pc engine behaviour is take our soundcard's
         * sampling rate into consideration with regard to the 3580000 effective pc engine
         * samplerate.  We use 16.16 fixed arithmetic for speed.
         */
        uint32_t fixed_inc = ((CLOCK_PSG / host.sound.sample_freq) << 16) / Tp;

        while (buf < buf_end) {
            if ((sample = (PCE.PSG.wave[ch][PCE.PSG.regs[ch][PSG_DATA_INDEX_REG]] - 16)) >= 0)
                sample++;

            *buf++ = (int8_t) (sample * lbal / 64);

            if (host.sound.stereo) {
                *buf++ = (int8_t) (sample * rbal / 64);
            }

            fixed_n[ch] += fixed_inc;
            fixed_n[ch] &= 0x1FFFFF;    /* (31 << 16) + 0xFFFF */
            PCE.PSG.regs[ch][PSG_DATA_INDEX_REG] = fixed_n[ch] >> 16;
        }
        goto pad_and_return;
    }

pad_and_return:
    if (buf < buf_end) {
        memset(buf, 0, buf_end - buf);
    }
}


int
snd_init(void)
{
    memset(&da_index, 0, sizeof(da_index));
    memset(&fixed_n, 0, sizeof(fixed_n));

    for (int i = 0; i < PSG_CHANNELS; i++) {
        noise_rand[i] = 0x51F6310 << i;
        noise_level[i] = i;
        noise_k[i] = 0;
    }

    osd_snd_init();

    return 0;
}


void
snd_term(void)
{
    osd_snd_shutdown();
}


void
snd_update(short *buffer, size_t length)
{
    int lvol = (PCE.PSG.volume >> 4);
    int rvol = (PCE.PSG.volume & 0x0F);

    if (host.sound.stereo) {
        length *= 2;
    }

    memset(buffer, 0, length * 2);

    for (int i = 0; i < PSG_CHANNELS; i++)
    {
        psg_update(mix_buffer, i, length);

        for (int j = 0; j < length; j += 2)
        {
            buffer[j] += mix_buffer[j] * lvol;
            buffer[j + 1] += mix_buffer[j + 1] * rvol;
        }
    }
}
