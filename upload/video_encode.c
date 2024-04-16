#include <stdio.h>
#include <pthread.h>

#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#include "common.h"

static const enum AVCodecID video_encode_id = AV_CODEC_ID_H264;
const enum AVPixelFormat video_encode_format = AV_PIX_FMT_YUV420P;

static AVCodecContext *enc_ctx = NULL;
static struct SwsContext *sws_ctx = NULL;
static AVFrame *out_frame = NULL;
static AVFrame *in_frame = NULL;

static char out_path[256] = "/data/s3_upload/v4l2.";
static FILE *out_file = NULL;

#define V_WIDTH 640
#define V_HEIGHT 480
#define V_FPS 30

static AVCodecContext* open_video_encoder(int width, int height, int gop, int fps) {
    AVCodec *codec = avcodec_find_encoder(video_encode_id);
    if (!codec) {
        av_log(NULL, AV_LOG_ERROR, "[codec] Not found %s encoder!\n", avcodec_get_name(video_encode_id));
        return NULL;
    }
    av_log(NULL, AV_LOG_WARNING, "[codec] Found %s encoder: %s\n", avcodec_get_name(video_encode_id), codec->long_name);

    enc_ctx = avcodec_alloc_context3(codec);
    if (!enc_ctx) {
        av_log(NULL, AV_LOG_ERROR, "[codec] Could not allocate %s encoder context!\n", avcodec_get_name(video_encode_id));
        return NULL;
    }

    // SPS/PPS
    switch (video_encode_id) {
    case AV_CODEC_ID_H264:
        enc_ctx->profile = FF_PROFILE_H264_MAIN;
        enc_ctx->level = 30; //表示LEVEL是3.0
        break;
    default:
        break;
    }

    //设置输入YUV格式
    enc_ctx->pix_fmt = video_encode_format;

    //设置分辫率
    enc_ctx->width = width;
    enc_ctx->height = height;

    // GOP
    enc_ctx->gop_size = gop; // frames per ts
    enc_ctx->keyint_min = fps; // option

    //设置B帧数据
    enc_ctx->max_b_frames = 0; // option
    enc_ctx->has_b_frames = 0; // option

    //参考帧的数量
    enc_ctx->refs = 0; // option

    //设置码率
    enc_ctx->bit_rate = 768000;

    //设置帧率
    enc_ctx->framerate = (AVRational){fps, 1}; // 帧率
    enc_ctx->time_base = AV_TIME_BASE_Q; // 帧与帧之间的间隔是time_base

    int ret = avcodec_open2(enc_ctx, codec, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "[codec] Could not open %s encoder: [%d]%s!\n", avcodec_get_name(video_encode_id), ret, av_err2str(ret));
        return NULL;
    }

    return enc_ctx;
}

static AVFrame *create_video_frame(enum AVPixelFormat pix_fmt, int width, int height) {
    int ret = 0;
    AVFrame *frame = NULL;

    frame = av_frame_alloc();
    if (!frame) {
        av_log(NULL, AV_LOG_ERROR, "[VE] Failed to alloc frame!\n");
        goto __ERROR;
    }

    // 设置参数
    frame->format = pix_fmt;
    frame->width = width;
    frame->height = height;

    // alloc inner memory
    ret = av_frame_get_buffer(frame, 32); // 按 32 位对齐
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "[VE] Failed to alloc buffer for frame: [%d]%s!\n", ret, av_err2str(ret));
        goto __ERROR;
    }

    return frame;

__ERROR:
    if (frame) {
        av_frame_free(&frame);
    }

    return NULL;
}

static void yuyv422ToYuv420p(AVFrame *frame, AVPacket *pkt) {
    // YUYVYUYVYUYVYUYV YUYV422
    // YYYYYYYYUUVV YUV420
    int i = 0;
    int yuv422_length = V_WIDTH * V_HEIGHT * 2;
    int y_index = 0;
    // copy all y
    for (i = 0; i < yuv422_length; i += 2) {
        frame->data[0][y_index] = pkt->data[i];
        y_index++;
    }

    // copy u and v
    int line_start = 0;
    int u_index = 0;
    int v_index = 0;
    // copy u, v per line. skip a line once
    for (i = 0; i < V_HEIGHT; i += 2) {
        // line i offset
        line_start = i * V_WIDTH * 2;
        for (int j = line_start + 1; j < line_start + V_WIDTH * 2; j += 4) {
            frame->data[1][u_index] = pkt->data[j];
            u_index++;
            frame->data[2][v_index] = pkt->data[j + 2];
            v_index++;
        }
    }
}

void video_encode_frame(AVPacket *pkt, AV_ENCODE_CALLBACK cb) {
    static int64_t encode_count = 0;

    AVFrame *frame = NULL;

    if (pkt) {
        yuyv422ToYuv420p(out_frame, pkt);
        out_frame->pts = pkt->pts;
        out_frame->pkt_duration = pkt->duration;
        frame = out_frame;
        av_log(NULL, AV_LOG_DEBUG, "[VE] send video frame: pts=%ld(%ld), duration=%ld\n", frame->pts, frame->pts / 1000, frame->pkt_duration);
    } else {
        av_log(NULL, AV_LOG_WARNING, "[sync]@%ld send flush video frame\n", (av_gettime_relative() - relative_start_time) / 1000);
    }

    // 送原始数据给编码器进行编码
    int ret = avcodec_send_frame(enc_ctx, frame);
    if (ret) {
        av_log(NULL, AV_LOG_ERROR, "[VE] Failed to send a video frame to the encoder: [%d]%s!\n", ret, av_err2str(ret));
        return;
    }

    // 从编码器获取编码好的数据
    while (ret == 0) {
        AVPacket enc_pkt;
        av_init_packet(&enc_pkt);
        enc_pkt.data = NULL;
        enc_pkt.size = 0;
        ret = avcodec_receive_packet(enc_ctx, &enc_pkt);
        switch (ret) {
        case 0:
            av_log(NULL, AV_LOG_DEBUG, "VE[%ld] receive video packet: pts=%ld(%ld), duration=%ld, size=%d\n", encode_count, enc_pkt.pts, enc_pkt.pts / 1000, enc_pkt.duration, enc_pkt.size);
            encode_count++;
            break;

        case AVERROR(EAGAIN):
            av_log(NULL, AV_LOG_TRACE, "[VE] video encoder output is not available\n");
            return;

        case AVERROR_EOF:
            av_log(NULL, AV_LOG_WARNING, "[sync]@%ld video encoder has been fully flushed: encode_count=%ld\n", (av_gettime_relative() - relative_start_time) / 1000, encode_count);
            return;

        default:
            av_log(NULL, AV_LOG_ERROR, "[VE] video encoder error: [%d]%s!\n", ret, av_err2str(ret));
            return;
        }

        fwrite(enc_pkt.data, 1, enc_pkt.size, out_file);
        fflush(out_file);

        if (cb) {
            cb(&enc_pkt);
        }

        av_packet_unref(&enc_pkt);
    }
}

int video_encode_open(int ts_duration) {
    if (enc_ctx) {
        return 0;
    }

    strcat(out_path, avcodec_get_name(video_encode_id));
    out_file = fopen(out_path, "wb+");
    if (!out_file) {
        goto __ERROR;
    }

    enc_ctx = open_video_encoder(V_WIDTH, V_HEIGHT, ts_duration * V_FPS, V_FPS);
    if (!enc_ctx) {
        goto __ERROR;
    }

    sws_ctx = sws_alloc_context();
    if (!sws_ctx) {
        goto __ERROR;
    }

    out_frame = create_video_frame(enc_ctx->pix_fmt, enc_ctx->width, enc_ctx->height);
    if (!out_frame) {
        goto __ERROR;
    }
    in_frame = create_video_frame(enc_ctx->pix_fmt, enc_ctx->width, enc_ctx->height);
    if (!out_frame) {
        goto __ERROR;
    }

    return 0;

__ERROR:
    video_encode_close();
    return -1;
}

void video_encode_close() {
    if (in_frame) {
        av_frame_free(&in_frame);
    }
    if (out_frame) {
        av_frame_free(&out_frame);
    }

    if (sws_ctx) {
        sws_freeContext(sws_ctx);
        sws_ctx = NULL;
    }

    if (enc_ctx) {
        avcodec_close(enc_ctx);
        avcodec_free_context(&enc_ctx);
    }

    if (out_file) {
        fclose(out_file);
        out_file = NULL;
    }
}
