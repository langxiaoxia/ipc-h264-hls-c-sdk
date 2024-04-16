#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

#include <libavutil/avutil.h>
#include <libavutil/time.h>

#include "common.h"

// starts
int64_t relative_start_time = 0;
int64_t absolute_start_time = 0;

// command line parameters
static int ts_duration = 1; // seconds
static enum AVCodecID audio_encode_id = AV_CODEC_ID_NONE;
static uint64_t last_seq = 0;

// debug options
static const int capture_duration = 0; // seconds, 0 for infinite
static const int capture_video = 1;
static const int capture_audio = 1;
static const int encode_video = 1;
static const int encode_audio = 1;
static const int upload_video = 1;
static const int upload_audio = 1;

// flags
static volatile int video_captured = 0;

static int wait_video_capture() {
    return capture_video && !video_captured;
}

static int capture_enough(int64_t duration) {
    return capture_duration && duration >= capture_duration * AV_TIME_BASE;
}

static int video_capture_callback(AVPacket *pkt) {
    static int64_t count = 0;
    static int64_t duration = 0;

    av_log(NULL, AV_LOG_DEBUG, "VC[%ld] read video frame: pts=%ld, duration=%ld, size=%d\n", count, pkt->pts, pkt->duration, pkt->size);
    count++;

    if (!video_captured) {
        av_log(NULL, AV_LOG_WARNING, "[sync]@%ld set video captured!\n", (av_gettime_relative() - relative_start_time) / 1000);
        video_captured = 1;
    }

    pkt->pts -= relative_start_time;
    pkt->dts = pkt->pts;
    if (encode_video) {
        video_encode_frame(pkt, s3_upload_video);
    }

    duration += pkt->duration;
    if (capture_enough(duration)) {
        if (encode_video) {
            video_encode_frame(NULL, s3_upload_video);
        }

        return 1; // stop capture
    }

    return 0; // continue capture
}

static int audio_capture_callback(AVPacket *pkt) {
    static int64_t count = 0;
    static int64_t duration = 0;
    static int skip_count = 0;

    if (wait_video_capture()) {
        if (0 == skip_count) {
            av_log(NULL, AV_LOG_WARNING, "[sync]@%ld start skip audio frame\n", (av_gettime_relative() - relative_start_time) / 1000);
        }
        skip_count++;
        return 0; // continue capture
    }

    if (0 == count) {
        av_log(NULL, AV_LOG_WARNING, "[sync]@%ld total skip %d audio frames\n", (av_gettime_relative() - relative_start_time) / 1000, skip_count);
    }

    av_log(NULL, AV_LOG_DEBUG, "AC[%ld] read audio frame: pts=%ld, duration=%ld, size=%d\n", count, pkt->pts, pkt->duration, pkt->size);
    count++;

    pkt->pts -= absolute_start_time;
    pkt->dts = pkt->pts;
    if (encode_audio) {
        audio_encode_frame(pkt, audio_encode_id, s3_upload_audio);
    }

    duration += pkt->duration;
    if (capture_enough(duration)) {
        if (encode_audio) {
            audio_encode_frame(NULL, audio_encode_id, s3_upload_audio);
        }

        return 1; // stop capture
    }

    return 0; // continue capture
}

int main(int argc, char *argv[]) {
    // command line: video_gop, audio_fmt last_seq
    if (argc > 1) {
        ts_duration = atoi(argv[1]);
        if (ts_duration < 1) {
          ts_duration = 1;
        } else if (ts_duration > 60) {
          ts_duration = 60;
        }
    }
    printf("ts duration %d(s)\n", ts_duration);

    int audio_fmt = 0;
    if (argc > 2) {
        audio_fmt = atoi(argv[2]);
        switch (audio_fmt) {
          case 1:
            audio_encode_id = AV_CODEC_ID_AAC;
            printf("ts with audio aac\n");
            break;
          case 2:
            audio_encode_id = AV_CODEC_ID_MP3;
            printf("ts with audio mp3\n");
            break;
          default:
            audio_fmt = 0;
            audio_encode_id = AV_CODEC_ID_NONE;
            printf("ts without audio\n");
            break;
        }
    }

    if (argc > 3) {
        last_seq = strtoul(argv[3], NULL, 10);
        printf("set last seq %lu\n", last_seq);
    } else {
        printf("default last seq %lu\n", last_seq);
    }

    // av init
    av_input_init();
    if (av_gettime_relative_is_monotonic()) {
        av_log(NULL, AV_LOG_WARNING, "[sync] time source is monotonic!\n");
    } else {
        av_log(NULL, AV_LOG_WARNING, "[sync] time source is NOT monotonic!\n");
    }
    relative_start_time = av_gettime_relative();
    absolute_start_time = av_gettime();

    // s3 init
    char *s3_ak = getenv("AWS_ACCESS_KEY_ID");
    char *s3_sk = getenv("AWS_SECRET_ACCESS_KEY");
    if (!s3_ak || !s3_sk) {
        av_log(NULL, AV_LOG_ERROR, "Not found AK/SK!\n");
        return -1;
    }

    char *s3_region = getenv("AWS_S3_REGION");
    char *s3_bucket = getenv("AWS_S3_BUCKET");
    char *s3_prefix = getenv("AWS_S3_PREFIX");
    if (!s3_region || !s3_bucket || !s3_prefix) {
        av_log(NULL, AV_LOG_ERROR, "Not found REGION/BUCKET/PREFIX!\n");
        return -1;
    }

    if (s3_upload_start(last_seq + 1, audio_fmt, s3_ak, s3_sk, s3_region, s3_bucket, s3_prefix, upload_video, upload_audio))  {
        goto __ERROR;
    }

    // capture video
    if (capture_video) {
        if (video_encode_open(ts_duration)) {
            goto __ERROR;
        }
        if (video_capture_start(video_capture_callback)) {
            goto __ERROR;
        }
    }

    // capture audio
    if (capture_audio) {
        if (audio_encode_open(audio_encode_id)) {
            goto __ERROR;
        }
        if (audio_capture_start(audio_capture_callback)) {
            goto __ERROR;
        }
    }

__ERROR:
    video_capture_stop();
    audio_capture_stop();

    video_encode_close();
    audio_encode_close();

    s3_upload_stop();
    return 0;
}
