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

// configs
static int capture_duration = 1; // seconds
static int capture_video = 1;
static int capture_audio = 1;
static int encode_video = 1;
static int encode_audio = 1;
static int upload_video = 0;
static int upload_audio = 0;

// flags
static volatile int video_captured = 0;

static int wait_video_capture() {
    return capture_video && !video_captured;
}

static int capture_enough(int64_t duration) {
    return duration >= capture_duration * AV_TIME_BASE;
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
        audio_encode_frame(pkt, s3_upload_audio);
    }

    duration += pkt->duration;
    if (capture_enough(duration)) {
        if (encode_audio) {
            audio_encode_frame(NULL, s3_upload_audio);
        }

        return 1; // stop capture
    }

    return 0; // continue capture
}

int main(int argc, char *argv[]) {
    // command line: capture_duration upload_video upload_audio
    if (argc > 1) {
        capture_duration = atoi(argv[1]);
        if (capture_duration < 1) {
            capture_duration = 1;
        }
        if (capture_duration > 3600) {
            capture_duration = 3600;
        }
    }
    if (argc > 2) {
        upload_video = atoi(argv[2]);
    }
    if (argc > 3) {
        upload_audio = atoi(argv[3]);
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

    if (s3_upload_start(s3_ak, s3_sk, s3_region, s3_bucket, s3_prefix, upload_video, upload_audio))  {
        goto __ERROR;
    }

    // capture video
    if (capture_video) {
        if (video_encode_open()) {
            goto __ERROR;
        }
        if (video_capture_start(video_capture_callback)) {
            goto __ERROR;
        }
    }

    // capture audio
    if (capture_audio) {
        if (audio_encode_open()) {
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
