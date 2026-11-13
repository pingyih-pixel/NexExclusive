#ifndef NEX_AUDIO_LINK_H
#define NEX_AUDIO_LINK_H

#define NEX_AUDIO_MAGIC          0x4E455841u /* NEXA */
#define NEX_AUDIO_VERSION       2u
#define NEX_AUDIO_RING_FRAMES   16384u
#define NEX_AUDIO_ENDPOINT_CHARS 256u

#define NEX_AUDIO_INIT           0L
#define NEX_AUDIO_READY          1L
#define NEX_AUDIO_FAILED        -1L
#define NEX_AUDIO_CLOSING        2L

/* Shared between nexsink.dll and NexAudioHost.exe. Positions are monotonic;
   the low 32 bits are used only for the byte offset in the bounded ring. */
typedef struct NexAudioIpc {
    unsigned long magic;
    unsigned long version;
    volatile long state;
    volatile long error;
    unsigned long rate;
    unsigned long channels;
    unsigned long container_bits;
    unsigned long valid_bits;
    unsigned long is_float;
    unsigned long block;
    unsigned long capacity;
    unsigned long channel_mask;
    volatile long long write_pos;
    volatile long long read_pos;
    volatile unsigned long long client_frames;
    volatile unsigned long long endpoint_frames;
    volatile unsigned long long changed_bytes;
    volatile unsigned long long dropped_frames;
    volatile unsigned long long underrun_events;
    volatile unsigned long long client_hash;
    volatile unsigned long long endpoint_hash;
    wchar_t endpoint_id[NEX_AUDIO_ENDPOINT_CHARS];
    unsigned char data[NEX_AUDIO_RING_FRAMES * 32u];
} NexAudioIpc;

#endif
