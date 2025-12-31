#include "dc/sound/aica_comm.h"

extern int snd_sh4_to_aica(void *packet, uint32_t size);
struct snd_effect;
typedef struct snd_effect
{
    uint32_t locl, locr;
    uint32_t len;
    uint32_t rate;
    uint32_t used;
    uint32_t fmt;
    uint16_t stereo;

    LIST_ENTRY(snd_effect)
    list;
} snd_effect_t;

sfxhnd_t handles[16];
sfx_play_data_t pdata[32];
int instrument_map[16];
int uic = 0;

// Any value of numChannels set
// by the defaults code in M_misc is now clobbered by I_InitSound().
// number of channels available for sound effects
int numChannels;

#define fnpre "/pc"
/**********************************************************************/

typedef unsigned int ULONG;
typedef unsigned short UWORD;
typedef unsigned char UBYTE;
typedef char BYTE;

#define SAMPLERATE 11025

#define MUS_VOICES 16

typedef struct MUSheader
{
    // identifier "MUS" 0x1A
    char ID[4];
    UWORD scoreLen;
    UWORD scoreStart;
    // count of primary channels
    UWORD channels;
    // count of secondary channels
    UWORD sec_channels;
    UWORD instrCnt;
    UWORD dummy;

    // variable-length part starts here
    UWORD instruments[];
} MUSheader_t;

struct Channel
{
    float pitch;
    float pan;
    float vol;
    float ltvol;
    float rtvol;
    int instrument;
    int map[128];
};

struct midiHdr
{
    ULONG rate;
    ULONG loop;     // 16.16 fixed pt
    ULONG length;   // 16.16 fixed pt
    UWORD base;     // note base of sample
    BYTE sample[8]; // actual length varies
};

typedef struct Voice
{
    sfxhnd_t wave;
    float index;
    float step;
    ULONG loop;
    ULONG length;
    float ltvol;
    // float rtvol;
    int pan;
    UWORD base;
    UWORD flags;
    int chan;
    int start_new;
    int stop_new;
} Voice_t;

static int __attribute__((aligned(8))) voice_in_use[MUS_VOICES];

static struct Channel __attribute__((aligned(8))) mus_channel[16];

static struct Voice __attribute__((aligned(8))) audVoice[MUS_VOICES];
static struct Voice __attribute__((aligned(8))) midiVoice[256];
static ULONG BEATS_PER_PASS = 4; // 4 = 35Hz, 2 = 70Hz, 1 = 140Hz
volatile int snd_ticks;          // advanced by sound thread

static int changepitch;

/* sampling freq (Hz) for each pitch step when changepitch is TRUE
   calculated from (2^((i-128)/64))*11025
   I'm not sure if this is the right formula */
static UWORD __attribute__((aligned(8))) freqs[256] =
    {
        2756, 2786, 2817, 2847, 2878, 2910, 2941, 2973,
        3006, 3038, 3072, 3105, 3139, 3173, 3208, 3242,
        3278, 3313, 3350, 3386, 3423, 3460, 3498, 3536,
        3574, 3613, 3653, 3692, 3733, 3773, 3814, 3856,
        3898, 3940, 3983, 4027, 4071, 4115, 4160, 4205,
        4251, 4297, 4344, 4391, 4439, 4487, 4536, 4586,
        4635, 4686, 4737, 4789, 4841, 4893, 4947, 5001,
        5055, 5110, 5166, 5222, 5279, 5336, 5394, 5453,
        5513, 5573, 5633, 5695, 5757, 5819, 5883, 5947,
        6011, 6077, 6143, 6210, 6278, 6346, 6415, 6485,
        6556, 6627, 6699, 6772, 6846, 6920, 6996, 7072,
        7149, 7227, 7305, 7385, 7465, 7547, 7629, 7712,
        7796, 7881, 7967, 8053, 8141, 8230, 8319, 8410,
        8501, 8594, 8688, 8782, 8878, 8975, 9072, 9171,
        9271, 9372, 9474, 9577, 9681, 9787, 9893, 10001,
        10110, 10220, 10331, 10444, 10558, 10673, 10789, 10906,
        11025, 11145, 11266, 11389, 11513, 11638, 11765, 11893,
        12023, 12154, 12286, 12420, 12555, 12692, 12830, 12970,
        13111, 13254, 13398, 13544, 13691, 13841, 13991, 14144,
        14298, 14453, 14611, 14770, 14931, 15093, 15258, 15424,
        15592, 15761, 15933, 16107, 16282, 16459, 16639, 16820,
        17003, 17188, 17375, 17564, 17756, 17949, 18144, 18342,
        18542, 18744, 18948, 19154, 19363, 19574, 19787, 20002,
        20220, 20440, 20663, 20888, 21115, 21345, 21578, 21812,
        22050, 22290, 22533, 22778, 23026, 23277, 23530, 23787,
        24046, 24308, 24572, 24840, 25110, 25384, 25660, 25940,
        26222, 26508, 26796, 27088, 27383, 27681, 27983, 28287,
        28595, 28907, 29221, 29540, 29861, 30187, 30515, 30848,
        31183, 31523, 31866, 32213, 32564, 32919, 33277, 33639,
        34006, 34376, 34750, 35129, 35511, 35898, 36289, 36684,
        37084, 37487, 37896, 38308, 38725, 39147, 39573, 40004,
        40440, 40880, 41325, 41775, 42230, 42690, 43155, 43625};

static int __attribute__((aligned(8))) note_table[128] =
    {
        65536 / 64, 69433 / 64, 73562 / 64, 77936 / 64, 82570 / 64, 87480 / 64, 92682 / 64, 98193 / 64, 104032 / 64, 110218 / 64, 116772 / 64, 123715 / 64,
        65536 / 32, 69433 / 32, 73562 / 32, 77936 / 32, 82570 / 32, 87480 / 32, 92682 / 32, 98193 / 32, 104032 / 32, 110218 / 32, 116772 / 32, 123715 / 32,
        65536 / 16, 69433 / 16, 73562 / 16, 77936 / 16, 82570 / 16, 87480 / 16, 92682 / 16, 98193 / 16, 104032 / 16, 110218 / 16, 116772 / 16, 123715 / 16,
        65536 / 8, 69433 / 8, 73562 / 8, 77936 / 8, 82570 / 8, 87480 / 8, 92682 / 8, 98193 / 8, 104032 / 8, 110218 / 8, 116772 / 8, 123715 / 8,
        65536 / 4, 69433 / 4, 73562 / 4, 77936 / 4, 82570 / 4, 87480 / 4, 92682 / 4, 98193 / 4, 104032 / 4, 110218 / 4, 116772 / 4, 123715 / 4,
        65536 / 2, 69433 / 2, 73562 / 2, 77936 / 2, 82570 / 2, 87480 / 2, 92682 / 2, 98193 / 2, 104032 / 2, 110218 / 2, 116772 / 2, 123715 / 2,
        65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715,
        65536 * 2, 69433 * 2, 73562 * 2, 77936 * 2, 82570 * 2, 87480 * 2, 92682 * 2, 98193 * 2, 104032 * 2, 110218 * 2, 116772 * 2, 123715 * 2,
        65536 * 4, 69433 * 4, 73562 * 4, 77936 * 4, 82570 * 4, 87480 * 4, 92682 * 4, 98193 * 4, 104032 * 4, 110218 * 4, 116772 * 4, 123715 * 4,
        65536 * 8, 69433 * 8, 73562 * 8, 77936 * 8, 82570 * 8, 87480 * 8, 92682 * 8, 98193 * 8, 104032 * 8, 110218 * 8, 116772 * 8, 123715 * 8,
        65536 * 16, 69433 * 16, 73562 * 16, 77936 * 16, 82570 * 16, 87480 * 16, 92682 * 16, 98193 * 16};

static float __attribute__((aligned(8))) pitch_table[256];

static float master_vol = 64.0f;

static int musicdies = -1;
static int music_okay = 0;

void *midi_pointers = NULL;

static UWORD score_len, score_start, inst_cnt;
static void *score_data;
static UBYTE *score_ptr;

static int mus_delay = 0;
static int mus_looping = 0;
static float mus_volume = 1.0f;
static int mus_playing = 0;

void __attribute__((aligned(8))) * sample_buffers[256];
int __attribute__((aligned(8))) used_instruments[256];

int used_instrument_count = -1;

void reset_midiVoices(void)
{
    int i;

    for (i = 0; i < 256; i++)
    {
        //        if (NULL != (void*)midiVoice[i].wave)
        //      {
        //        free(midiVoice[i].wave);
        //  }

        midiVoice[i].wave = -1; // NULL;
        midiVoice[i].index = 0.0f;
        midiVoice[i].step = 1.0f;
        midiVoice[i].loop = 0;
        midiVoice[i].length = 2000 << 16;
        midiVoice[i].ltvol = 0.0f;
        // midiVoice[i].rtvol = 0.0f;
        midiVoice[i].base = 60;
        midiVoice[i].flags = 0x00;
        midiVoice[i].pan = 127;
        midiVoice[i].start_new = midiVoice[i].stop_new = 0;
        used_instruments[i] = -1;
        sample_buffers[i] = NULL;
    }

    used_instrument_count = -1;
}

#define WSWAP(x) __builtin_bswap16((x))
#define LSWAP(x) __builtin_bswap32((x))

void *musdata = NULL;

void I_InitMusic(void)
{
    int i;
    // fill in pitch wheel table
    for (i = 0; i < 128; i++)
    {
        pitch_table[i] = 1.0f + (-3678.0f * (float)(128 - i) / 64.0f) / 65536.0f;
    }
    for (i = 0; i < 128; i++)
    {
        pitch_table[i + 128] = 1.0f + (3897.0f * (float)i / 64.0f) / 65536.0f;
    }
    for (i = 0; i < 256; i++)
    {
        midiVoice[i].wave = -1; // NULL;
        midiVoice[i].index = 0.0f;
        midiVoice[i].step = 1.0f;
        midiVoice[i].loop = 0;
        midiVoice[i].length = 2000 << 16;
        midiVoice[i].ltvol = 0.0f;
        //		midiVoice[i].rtvol = 0.0f;
        midiVoice[i].base = 60;
        midiVoice[i].flags = 0x00;
        midiVoice[i].pan = 127;
        midiVoice[i].start_new = midiVoice[i].stop_new = 0;
    }

    if (!midi_pointers)
    {
        music_okay = 0;

        midi_pointers = malloc(sizeof(ULONG) * 182);
        file_t mphnd = fs_open("/pc/MIDI_Instruments", O_RDONLY);

        fs_read(mphnd, midi_pointers, sizeof(ULONG) * 182);

        fs_close(mphnd);

        printf("I_InitMusic: Pre-cached all MUS instrument headers.\n");

        music_okay = 1;
    }

    int f;
    for (f = 0; f < 256; f++)
    {
        sample_buffers[f] = NULL;
    }
}

int instrument_used(int instrument)
{
    int i;
    for (i = 0; i < 256; i++)
    {
        if (used_instruments[i] == instrument)
        {
            return 1;
        }
    }

    return 0;
}

void Mus_Play(int handle, int looping)
{
    if (!handle)
    {
        return;
    }

    mus_looping = looping;
    mus_playing = 2; // 2 = play from start
}

void Mus_Stop(int handle)
{
    int ix;

    if (mus_playing)
    {
        mus_playing = -1; // stop playing
    }

    // disable instruments in score (just disable them all)
    for (ix = 0; ix < MUS_VOICES; ix++)
    {
        audVoice[ix].flags = 0x00; // disable voice
        audVoice[ix].start_new = audVoice[ix].stop_new = 0;
    }

    mus_looping = 0;
    mus_delay = 0;
}

void Mus_Unregister(int handle)
{
    Mus_Stop(handle);
    // music won't start playing until mus_playing set at this point

    score_data = 0;
    score_len = 0;
    score_start = 0;
    inst_cnt = 0;
}

void snd_sfx_update_ex(sfx_play_data_t *data)
{
    int size;
    snd_effect_t *t = (snd_effect_t *)data->idx;
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);

    size = t->len;

    if (size >= 65535)
        size = 65534;

    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = data->chn;

    chan->cmd = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_FREQ | AICA_CH_UPDATE_SET_PAN | AICA_CH_UPDATE_SET_VOL;
    chan->base = t->locl;
    chan->type = t->fmt;
    chan->length = size;

    chan->loop = data->loop;
    chan->loopstart = data->loopstart;
    chan->loopend = data->loopend ? data->loopend : size;
    chan->freq = data->freq > 0 ? data->freq : t->rate;
    chan->vol = data->vol;
    chan->pan = data->pan;

    snd_sh4_to_aica(tmp, cmd->size);
}

int Mus_Register(char *filename)
{
    int i;
    file_t hnd;

    ULONG *miptr;
    uic = 0;
    struct midiHdr *mhdr;

    //    for (i=0;i<30;i++) {
    //        memset()
    //    }
    memset(instrument_map, 0, sizeof(int) * 16);
    memset(handles, 0, sizeof(sfxhnd_t) * 16);
    memset(pdata, 0, sizeof(sfx_play_data_t) * 16);
    for (i = 0; i < 32; i++)
    {
        pdata[i].chn = snd_sfx_chn_alloc();
    }

    file_t mus_hnd = fs_open(filename, O_RDONLY);
    ssize_t mus_size = fs_total(mus_hnd);
    printf("mus_size == %d\n", mus_size);
    if (musdata)
    {
        free(musdata);
        musdata = NULL;
    }
    musdata = malloc(mus_size);
    if (musdata == NULL)
    {
        exit(-1);
    }
    fs_read(mus_hnd, musdata, mus_size);
    fs_close(mus_hnd);

    ULONG *lptr = (ULONG *)musdata;
    UWORD *wptr = (UWORD *)musdata;

    Mus_Unregister(1);
    reset_midiVoices();
    // music won't start playing until mus_playing set at this point

    if (lptr[0] != 0x1a53554d) // 0x4d55531a
    {
        return 0; // "MUS",26 always starts a vaild MUS file
    }

    score_len = wptr[2]; // score length
    if (!score_len)
    {
        return 0; // illegal score length
    }

    score_start = wptr[3]; // score start
    if (score_start < 18)
    {
        return 0; // illegal score start offset
    }

    inst_cnt = wptr[6]; // instrument count
    if (!inst_cnt)
    {
        return 0; // illegal instrument count
    }

    // okay, MUS data seems to check out

    // if the instrument pointers haven't been set up previously, return here, can't do anything
    if (!midi_pointers)
    {
        return 0;
    }

    music_okay = 0;

    score_data = musdata;
    MUSheader_t *musheader = (MUSheader_t *)score_data;

    used_instrument_count = inst_cnt;
    for (i = 0; i < inst_cnt; i++)
    {
        used_instruments[i] = musheader->instruments[i];
    }

    miptr = (ULONG *)midi_pointers;

    // iterating over all instruments
    for (i = 0; i < 182; i++)
    {
        // current instrument is used
        if (instrument_used(i))
        {
            // get the pointer into the instrument data struct
            ULONG ptr = LSWAP(miptr[i]);

            // make sure it doesn't point to NULL
            if (ptr != 0)
            {
                instrument_map[uic] = i;
                uic += 1;

                // allocate some space for the header out of Doom's heap
                mhdr = (struct midiHdr *)malloc(sizeof(struct midiHdr));
                if (!mhdr)
                {
                    continue;
                }

                // open MIDI Instrument Set file from ROM
                hnd = fs_open("/pc/MIDI_Instruments", O_RDONLY);
                if (hnd < 0)
                {
                    continue;
                }

                fs_seek(hnd, ptr, SEEK_SET);
                fs_read(hnd, (void *)mhdr, sizeof(struct midiHdr));

                ULONG length = LSWAP(mhdr->length) >> 16;

                void *sample = (void *)malloc(length);
                if (!sample)
                {
                    free(mhdr);
                    fs_close(hnd);
                    continue;
                }
                else
                {
                    sample_buffers[i] = sample;
                }

                fs_seek(hnd, ptr + 4 + 4 + 4 + 2, SEEK_SET);
                fs_read(hnd, sample, length);

                midiVoice[i].index = 0.0f;
                midiVoice[i].step = 1.0f;
                midiVoice[i].loop = LSWAP(mhdr->loop);
                midiVoice[i].length = LSWAP(mhdr->length);
                midiVoice[i].ltvol = 0.0f;
                midiVoice[i].base = WSWAP(mhdr->base);
                midiVoice[i].flags = 0x00;
                midiVoice[i].pan = 127;
                midiVoice[i].start_new = midiVoice[i].stop_new = 0;

                handles[(uic - 1)] = snd_sfx_load_raw_buf(sample, midiVoice[i].length >> 16, 11025, 8, 1);
                midiVoice[i].wave = handles[(uic - 1)];
                free(mhdr);
                fs_close(hnd);
            }
        }
    }

    printf("song %s uses %d instruments\n", filename, uic);

    music_okay = 1;
    return 1;
}

int get_mapped_index(int instrnum)
{
    for (int i = 0; i < 16; i++)
    {
        if (instrument_map[i] == instrnum)
        {
            return i;
        }
    }
    return -1;
}

void MUS_UPDATE(void)
{
    float index;
    float step;
    float ltvol;
    float rtvol;
    float sample;
    int ix;
    int iy;
    ULONG loop;
    ULONG length;

    // process music if playing
    if (mus_playing)
    {
        if (mus_playing < 0)
        {
            // music now off
            mus_playing = 0;
        }
        else
        {
            if (mus_playing > 1)
            {
                mus_playing = 1;
                // start music from beginning
                score_ptr = (UBYTE *)((ULONG)score_data + (ULONG)score_start);
            }

            // 1=140Hz, 2=70Hz, 4=35Hz
            mus_delay -= BEATS_PER_PASS;
            if (mus_delay <= 0)
            {
                UBYTE event;
                UBYTE note;
                UBYTE time;
                UBYTE ctrl;
                UBYTE value;
                int next_time;
                int channel;
                int voice;
                int inst;
                float volume;
                float pan;

            nextEvent: // next event
                do
                {
                    event = *score_ptr++;

                    switch ((event >> 4) & 7)
                    {
                    // Release
                    case 0:
                    {
                        channel = (int)(event & 15);
                        note = *score_ptr++;
                        note &= 0x7f;
                        voice = mus_channel[channel].map[(ULONG)note] - 1;
                        if (voice >= 0)
                        {
                            // clear mapping
                            mus_channel[channel].map[(ULONG)note] = 0;
                            // voice available
                            voice_in_use[voice] = 0;
                            // voice releasing
                            audVoice[voice].flags |= 0x02;
                        }
                        break;
                    }
                    // Play note
                    case 1:
                    {
                        channel = (int)(event & 15);
                        note = *score_ptr++;
                        volume = -1.0f;
                        if (note & 0x80)
                        {
                            // set volume as well
                            note &= 0x7f;
                            volume = (float)*score_ptr++;
                        }
                        for (voice = 0; voice < MUS_VOICES; voice++)
                        {
                            if (!voice_in_use[voice])
                            {
                                // found free voice
                                break;
                            }
                        }
                        if (voice < MUS_VOICES)
                        {
                            // in use
                            voice_in_use[voice] = 1;
                            mus_channel[channel].map[(ULONG)note] = voice + 1;
                            audVoice[voice].start_new = 1;
                            if (volume >= 0.0f)
                            {
                            mus_channel[channel].vol = volume;
                            pan = mus_channel[channel].pan;
                            audVoice[voice].ltvol = volume;
                            audVoice[voice].pan = pan + 127;
                            }
                            if (channel != 15)
                            {
                                inst = mus_channel[channel].instrument;
                                // back link for pitch wheel
                                audVoice[voice].chan = channel;
                                audVoice[voice].wave = midiVoice[inst].wave;
                                audVoice[voice].index = 0.0f;
                                audVoice[voice].step = (float)note_table[(72 - midiVoice[inst].base + (ULONG)note) & 0x7f] / 65536.0f;
                                audVoice[voice].loop = midiVoice[inst].loop >> 16;
                                audVoice[voice].length = midiVoice[inst].length >> 16;
                                // enable voice
                                audVoice[voice].flags = 0x01;
                            }
                            else
                            {
                                // percussion channel - note is percussion instrument
                                inst = (ULONG)note + 100;
                                // back link for pitch wheel
                                audVoice[voice].chan = channel;
                                audVoice[voice].wave = midiVoice[inst].wave;
                                audVoice[voice].index = 0.0f;
                                audVoice[voice].step = 1.0f;
                                audVoice[voice].loop = midiVoice[inst].loop >> 16;
                                audVoice[voice].length = midiVoice[inst].length >> 16;
                                // enable voice
                                audVoice[voice].flags = 0x01;
                            }
                        }
                        break;
                    }
                    // Pitch
                    case 2:
                    {
                        channel = (int)(event & 15);
                        mus_channel[channel].pitch = pitch_table[(ULONG)*score_ptr++ & 0xff];
                        break;
                    }
                    // Tempo
                    case 3:
                    {
                        // skip value - not supported
                        score_ptr++;
                        break;
                    }
                    // Change control
                    case 4:
                    {
                        channel = (int)(event & 15);
                        ctrl = *score_ptr++;
                        value = *score_ptr++;

                        switch (ctrl)
                        {
                        case 0:
                        {
                            // set channel instrument
                            mus_channel[channel].instrument = (ULONG)value;
                            mus_channel[channel].pitch = 1.0f;
                            break;
                        }
                        case 3:
                        {
                            // set channel volume
                            mus_channel[channel].vol = volume = (float)value;
                            break;
                        }
                        case 4:
                        {
                            // set channel pan
                            mus_channel[channel].pan = pan = (float)value;
                            break;
                        }
                        }

                        break;
                    }
                    // End score
                    case 6:
                    {
                        if (mus_looping)
                        {
                            score_ptr = (UBYTE *)((ULONG)score_data + (ULONG)score_start);
                        }
                        else
                        {
                            for (voice = 0; voice < MUS_VOICES; voice++)
                            {
                                audVoice[voice].flags = 0;
                                voice_in_use[voice] = 0;
                            }
                            mus_delay = 0;
                            mus_playing = 0;
                            goto mix;
                        }
                        break;
                    }
                    }
                }
                // not last event
                while (!(event & 0x80));

                // now get the time until the next event(s)
                next_time = 0;
                time = *score_ptr++;

                while (time & 0x80)
                {
                    // while msb set, accumulate 7 bits
                    next_time |= (ULONG)(time & 0x7f);
                    next_time <<= 7;
                    time = *score_ptr++;
                }

                next_time |= (ULONG)time;
                mus_delay += next_time;

                if (mus_delay <= 0)
                {
                    goto nextEvent;
                }
            }
        }
    } // endif musplaying
mix:
    // mix enabled voices
    for (ix = 0; ix < MUS_VOICES; ix++)
    {
        if (audVoice[ix].start_new == 1)
        {
            pdata[ix].idx = audVoice[ix].wave;
            pdata[ix].loop = audVoice[ix].loop != 0;
            pdata[ix].loopstart = pdata[ix].loop ? audVoice[ix].loop : 0;
            pdata[ix].loopend = 0;
            pdata[ix].vol = audVoice[ix].ltvol * 2 / 3;// * 128;
            pdata[ix].pan = audVoice[ix].pan * 2;
            pdata[ix].freq = 11025.0f * audVoice[ix].step;

            snd_sfx_play_ex(&pdata[ix]);
            audVoice[ix].start_new = 0;
        }

#if 1
        if (audVoice[ix].flags & 0x01)
        {
            step = audVoice[ix].step;
            ltvol = audVoice[ix].ltvol;

                // special handling for instrument
                if (audVoice[ix].flags & 0x02)
                {
                    // releasing
                    ltvol *= 0.90f;
                    audVoice[ix].ltvol = ltvol;

                    if (ltvol <= 0.02f)
                    {
                        // disable voice
                        audVoice[ix].flags = 0;

                        pdata[ix].idx = audVoice[ix].wave;
                        pdata[ix].vol = 0;
                        pdata[ix].pan = 127;
                        pdata[ix].freq = 11025.0f * audVoice[ix].step;
                        snd_sfx_update_ex(&pdata[ix]);
                        // next voice
                        continue;
                    }
                }

                step *= mus_channel[audVoice[ix].chan & 15].pitch;
        
#endif
        pdata[ix].idx = audVoice[ix].wave;
        pdata[ix].loop = audVoice[ix].loop != 0;
        pdata[ix].loopstart = pdata[ix].loop ? audVoice[ix].loop : 0;
        pdata[ix].loopend = 0;
        pdata[ix].vol = audVoice[ix].ltvol * 2 / 3;
        pdata[ix].pan = audVoice[ix].pan * 2;
        pdata[ix].freq = 11025.0f * step;
        snd_sfx_update_ex(&pdata[ix]);
        //}
    }
}
}
