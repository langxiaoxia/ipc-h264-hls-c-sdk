#include <stdio.h>
#include <pthread.h>

#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>

#include "common.h"

const enum AVSampleFormat audio_capture_format = AV_SAMPLE_FMT_S16;

static AVFormatContext *fmt_ctx = NULL;
static AV_CAPTURE_CALLBACK audio_capture_cb = NULL;
static pthread_t thread_id;
static int thread_started = 0;

static AVFormatContext *open_audio_capture() {
    // arecord -l
    // card [1]: CAMERA, device [0]: USB Audio ==> hw:[1],[0]
    const char *format_name = "alsa";
    const char *device_name = getenv("AWS_UPLOAD_AUDIO_DEVICE"); // "hw:1,0"

    AVInputFormat *iformat = av_find_input_format(format_name);
    if (NULL == iformat) {
        av_log(NULL, AV_LOG_ERROR, "[AC] Failed to find format %s\n", format_name);
        return NULL;
    }

    AVFormatContext *ctx = avformat_alloc_context();
    if (NULL == ctx) {
        return NULL;
    }

    ctx->flags = AVFMT_FLAG_GENPTS | AVFMT_FLAG_FLUSH_PACKETS;

    AVDictionary *options = NULL;
    av_dict_set(&options, "sample_fmt", "pcm_s16le", 0);
    av_dict_set(&options, "sample_rate", STR2(A_CAPTURE_SAMPLE_RATE), 0);
    av_dict_set(&options, "channels", STR2(A_CAPTURE_CHANNELS), 0);
//    av_dict_set(&options, "thread_queue_size", "2048", 0);

    int ret = avformat_open_input(&ctx, device_name, iformat, &options);
    av_dict_free(&options);
    if (ret < 0) {
        avformat_free_context(ctx);
        av_log(NULL, AV_LOG_ERROR, "[AC] Failed to open device %s, [%d]%s\n", device_name, ret, av_err2str(ret));
        return NULL;
    }

    av_dump_format(ctx, 0, device_name, 0);
    return ctx;
}

void audio_capture_loop() {
    av_log(NULL, AV_LOG_WARNING, "[thread] audio_capture_loop start!\n");

    int i = 0;
    AVPacket pkt;
    while (av_read_frame(fmt_ctx, &pkt) == 0) {
        if (audio_capture_cb && audio_capture_cb(&pkt)) {
            av_packet_unref(&pkt);
            break;
        }
        av_packet_unref(&pkt);
        i++;
    }

    av_log(NULL, AV_LOG_WARNING, "[thread] audio_capture_loop finish!\n");
}

int audio_capture_start(AV_CAPTURE_CALLBACK cb) {
    if (fmt_ctx) {
        return 0;
    }

    fmt_ctx = open_audio_capture();
    if (!fmt_ctx) {
        goto __ERROR;
    }

    audio_capture_cb = cb;
	if(0 != pthread_create(&thread_id, NULL, (void*)audio_capture_loop, NULL)) {
        audio_capture_cb = NULL;
        goto __ERROR;
	}

    thread_started = 1;
    return 0;

__ERROR:
    audio_capture_stop();
    return -1;
}

void audio_capture_stop() {
    if (thread_started) {
        pthread_join(thread_id, NULL);
        thread_started = 0;
    }

    if (fmt_ctx) {
        avformat_close_input(&fmt_ctx);
    }
}
