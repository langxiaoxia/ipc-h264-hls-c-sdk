#include <stdio.h>
#include <pthread.h>

#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>

#include "common.h"

const enum AVPixelFormat video_capture_format = AV_PIX_FMT_YUYV422;

static AVFormatContext *fmt_ctx = NULL;
static AV_CAPTURE_CALLBACK video_capture_cb = NULL;
static pthread_t thread_id;
static int thread_started = 0;

static AVFormatContext *open_video_capture() {
    // sudo apt install v4l-utils
    // v4l2-ctl --list-devices
    const char *format_name = "v4l2";
    const char *device_name = getenv("AWS_UPLOAD_VIDEO_DEVICE"); // "/dev/video0"

    AVInputFormat *iformat = av_find_input_format(format_name);
    if (NULL == iformat) {
        av_log(NULL, AV_LOG_ERROR, "[VC] Failed to find format %s\n", format_name);
        return NULL;
    }

    AVFormatContext *ctx = avformat_alloc_context();
    if (NULL == ctx) {
        return NULL;
    }

    ctx->flags = AVFMT_FLAG_GENPTS | AVFMT_FLAG_FLUSH_PACKETS;

    AVDictionary *options = NULL;
    av_dict_set(&options, "pixel_format", av_get_pix_fmt_name(video_capture_format), 0);
    av_dict_set(&options, "video_size", "640x480", 0);
    av_dict_set(&options, "framerate", "30", 0);

    int ret = avformat_open_input(&ctx, device_name, iformat, &options);
    av_dict_free(&options);
    if (ret < 0) {
        avformat_free_context(ctx);
        av_log(NULL, AV_LOG_ERROR, "[VC] Failed to open device %s, [%d]%s\n", device_name, ret, av_err2str(ret));
        return NULL;
    }

    av_dump_format(ctx, 0, device_name, 0);
    return ctx;
}

void video_capture_loop() {
    av_log(NULL, AV_LOG_WARNING, "[thread] video_capture_loop start!\n");

    int i = 0;
    AVPacket pkt;
    while (av_read_frame(fmt_ctx, &pkt) == 0) {
        if (video_capture_cb && video_capture_cb(&pkt)) {
            av_packet_unref(&pkt);
            break;
        }
        av_packet_unref(&pkt);
        i++;
    }

    av_log(NULL, AV_LOG_WARNING, "[thread] video_capture_loop finish!\n");
}

int video_capture_start(AV_CAPTURE_CALLBACK cb) {
    if (fmt_ctx) {
        return 0;
    }

    fmt_ctx = open_video_capture();
    if (!fmt_ctx) {
        goto __ERROR;
    }

	if(0 != pthread_create(&thread_id, NULL, (void*)video_capture_loop, NULL)) {
        goto __ERROR;
	}

    thread_started = 1;
    video_capture_cb = cb;
    return 0;

__ERROR:
    video_capture_stop();
    return -1;
}

void video_capture_stop() {
    if (thread_started) {
        pthread_join(thread_id, NULL);
        thread_started = 0;
    }

    if (fmt_ctx) {
        avformat_close_input(&fmt_ctx);
    }
}
