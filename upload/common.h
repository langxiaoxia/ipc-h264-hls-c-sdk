#ifndef COMMON_H
#define COMMON_H

#include <libavcodec/avcodec.h>

// starts
extern int64_t relative_start_time;
extern int64_t absolute_start_time;

// video const
extern const enum AVPixelFormat video_capture_format;
extern const enum AVPixelFormat video_encode_format;

// audio const
extern const enum AVSampleFormat audio_capture_format;
extern const enum AVSampleFormat audio_encode_format;
#define A_CAPTURE_CHANNEL_LAYOUT AV_CH_LAYOUT_MONO
#define A_ENCODE_CHANNEL_LAYOUT AV_CH_LAYOUT_MONO
#define A_CAPTURE_CHANNELS 1
#define A_ENCODE_CHANNELS 1
#define A_CAPTURE_SAMPLE_RATE 48000
#define A_ENCODE_SAMPLE_RATE 48000

// helper
#define STR1(x) (#x)
#define STR2(x) (STR1(x))

// upload
extern int s3_upload_start(uint64_t seq, int audio, char *ak, char *sk, char *s3_region, char *s3_bucket, char *s3_prefix, int upload_video, int upload_audio);
extern void s3_upload_stop();
extern void s3_upload_video(AVPacket *pkt);
extern void s3_upload_audio(AVPacket *pkt);


extern void av_input_init();

typedef int (*AV_CAPTURE_CALLBACK)(AVPacket *pkt);
typedef void (*AV_ENCODE_CALLBACK)(AVPacket *pkt);

// video
extern int video_capture_start(AV_CAPTURE_CALLBACK cb);
extern void video_capture_stop();
extern int video_encode_open(int ts_duration);
extern void video_encode_frame(AVPacket *pkt, AV_ENCODE_CALLBACK cb);
extern void video_encode_close();

// audio
extern int audio_capture_start(AV_CAPTURE_CALLBACK cb);
extern void audio_capture_stop();
extern int audio_encode_open(enum AVCodecID audio_encode_id);
extern void audio_encode_frame(AVPacket *pkt, enum AVCodecID audio_encode_id, AV_ENCODE_CALLBACK cb);
extern void audio_encode_close();

#endif // COMMON_H
