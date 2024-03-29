#include <pthread.h>

#include <libavutil/log.h>
#include <libavutil/time.h>

#include "common.h"

#include "S3_HLS_SDK.h"
#include "S3_HLS_Return_Code.h"

// configs
static int put_video = 0;
static int put_audio = 0;

// stats
static int64_t video_count = 0;
static int64_t audio_count = 0;

// packet queue
#define MAX_PACKET_QUEUE_LENGTH (50 * 3) // 3s

typedef struct packet_queue_s {
    AVPacket *queue[MAX_PACKET_QUEUE_LENGTH];
    int queue_size;
    int queue_pos;
    int queue_length;

    pthread_mutex_t queue_lock;
} PACKET_QUEUE_CTX;

static PACKET_QUEUE_CTX *audio_queue_ctx = NULL;
static volatile int video_uploaded = 0;
static volatile int audio_uploaded = 0;
static volatile int64_t video_last_time = 0;
static volatile int64_t audio_last_time = 0;

static PACKET_QUEUE_CTX *packet_queue_malloc();
static void packet_queue_free(PACKET_QUEUE_CTX *ctx);
static int packet_queue_empty(PACKET_QUEUE_CTX *ctx);
static int packet_queue_full(PACKET_QUEUE_CTX *ctx);
static int packet_queue_push(PACKET_QUEUE_CTX *ctx, AVPacket *pkt);
static AVPacket *packet_queue_pop(PACKET_QUEUE_CTX *ctx);
static void packet_queue_flush(PACKET_QUEUE_CTX *ctx);
static void process_audio_queue();
static void s3_upload_audio_internal(AVPacket *pkt);

// av sync
static PACKET_QUEUE_CTX *packet_queue_malloc() {
    PACKET_QUEUE_CTX* ctx = (PACKET_QUEUE_CTX *)malloc(sizeof(PACKET_QUEUE_CTX));
    if(NULL == ctx) {
        av_log(NULL, AV_LOG_ERROR, "[sync] Failed to allocate queue context!\n");
        return NULL;
    }

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if(pthread_mutex_init(&ctx->queue_lock, &attr)) {
        av_log(NULL, AV_LOG_ERROR, "[sync] Failed to initialize queue lock!\n");
        free(ctx);
        return NULL;
    }

    ctx->queue_size = MAX_PACKET_QUEUE_LENGTH;
    ctx->queue_pos = 0;
    ctx->queue_length = 0;
    for(int i = 0; i < ctx->queue_size; i++) {
        ctx->queue[i] = NULL;
    }
    return ctx;
}

static void packet_queue_free(PACKET_QUEUE_CTX *ctx) {
    if (NULL == ctx) {
        return;
    }

    packet_queue_flush(ctx);
    if (pthread_mutex_destroy(&ctx->queue_lock)) {
        av_log(NULL, AV_LOG_ERROR, "[sync] Failed to destroy queue lock!\n");
    }
    free(ctx);
}

static int packet_queue_empty(PACKET_QUEUE_CTX *ctx) {
    pthread_mutex_lock(&ctx->queue_lock);
    int empty = (0 == ctx->queue_length);
    pthread_mutex_unlock(&ctx->queue_lock);
    return empty;
}

static int packet_queue_full(PACKET_QUEUE_CTX *ctx) {
    pthread_mutex_lock(&ctx->queue_lock);
    int full = (ctx->queue_size == ctx->queue_length);
    pthread_mutex_unlock(&ctx->queue_lock);
    return full;
}

static int packet_queue_push(PACKET_QUEUE_CTX *ctx, AVPacket *pkt) {
    pthread_mutex_lock(&ctx->queue_lock);
    if (packet_queue_full(ctx)) {
        av_log(NULL, AV_LOG_ERROR, "[sync] packet queue is full\n");
        pthread_mutex_unlock(&ctx->queue_lock);
        return -1;
    }

    int current_pos = (ctx->queue_pos + ctx->queue_length) % ctx->queue_size;
    ctx->queue[current_pos] = av_packet_clone(pkt);
    ctx->queue_length++;
    pthread_mutex_unlock(&ctx->queue_lock);
    return 0;
}

static AVPacket *packet_queue_pop(PACKET_QUEUE_CTX *ctx) {
    pthread_mutex_lock(&ctx->queue_lock);
    if (packet_queue_empty(ctx)) {
        pthread_mutex_unlock(&ctx->queue_lock);
        return NULL;
    }

    AVPacket *pkt = ctx->queue[ctx->queue_pos];
    ctx->queue_pos++;
    ctx->queue_pos = ctx->queue_pos % ctx->queue_size;
    ctx->queue_length--;

    pthread_mutex_unlock(&ctx->queue_lock);
    return pkt;
}

static void packet_queue_flush(PACKET_QUEUE_CTX *ctx) {
    pthread_mutex_lock(&ctx->queue_lock);
    av_log(NULL, AV_LOG_WARNING, "[sync] audio_last_time=%ld, video_last_time=%ld, drop=%d\n", audio_last_time / 1000, video_last_time / 1000, ctx->queue_length);
    AVPacket *pkt = packet_queue_pop(ctx);
    while (pkt) {
        av_packet_free(&pkt);
        pkt = packet_queue_pop(ctx);
    }
    pthread_mutex_unlock(&ctx->queue_lock);
}

// upload
int s3_upload_start(uint64_t seq, char *ak, char *sk, char *s3_region, char *s3_bucket, char *s3_prefix, int upload_video, int upload_audio) {
    printf("upload start seq %lu\n", seq);
    put_video = upload_video;
    put_audio = upload_audio;

    const uint32_t s3_buffer_size = 4 * 1024 * 1024;
    char *s3_endpoint = NULL;

    if (S3_HLS_OK != S3_HLS_SDK_Initialize(s3_buffer_size, s3_region, s3_bucket, s3_prefix, s3_endpoint, seq) ) {
        av_log(NULL, AV_LOG_ERROR, "S3_HLS_SDK_Initialize failed!\n");
        goto __ERROR;
    }

    if (S3_HLS_OK != S3_HLS_SDK_Start_Upload()) {
        av_log(NULL, AV_LOG_ERROR, "S3_HLS_SDK_Start_Upload failed!\n");
        goto __ERROR;
    }

    if (S3_HLS_OK != S3_HLS_SDK_Set_Credential(ak, sk, NULL)) {
        av_log(NULL, AV_LOG_ERROR, "S3_HLS_SDK_Set_Credential failed!\n");
        goto __ERROR;
    }

    audio_queue_ctx = packet_queue_malloc();
    return 0;

 __ERROR:
    s3_upload_stop();
    return -1;
}

void s3_upload_stop() {
    av_log(NULL, AV_LOG_WARNING, "[sync]@%ld stop upload: video_uploaded=%lu, audio_uploaded=%lu\n", (av_gettime_relative() - relative_start_time) / 1000, video_count, audio_count);
    packet_queue_free(audio_queue_ctx);
    S3_HLS_SDK_Finalize();
}

void s3_upload_video(AVPacket *pkt) {
    if (!pkt || !pkt->data || !pkt->size) {
        return;
    }

    process_audio_queue();

    S3_HLS_FRAME_PACK s3_frame_pack;
    s3_frame_pack.item_count = 1;
    s3_frame_pack.items[0].timestamp = pkt->pts; // use timestamp generated by encoder

    s3_frame_pack.items[0].first_part_start = pkt->data;
    s3_frame_pack.items[0].first_part_length = pkt->size;
    s3_frame_pack.items[0].second_part_start = NULL;
    s3_frame_pack.items[0].second_part_length = 0;

    av_log(NULL, AV_LOG_DEBUG, "VU[%ld] upload video packet: pts=%ld(%ld), duration=%ld, size=%d\n", video_count, pkt->pts, pkt->pts / 1000, pkt->duration, pkt->size);
    if (put_video) {
        S3_HLS_SDK_Put_Video_Frame(&s3_frame_pack);
    }
    video_last_time = pkt->pts;
    video_count++;
    if (!video_uploaded) {
        av_log(NULL, AV_LOG_WARNING, "[sync]@%ld set video uploaded!\n", (av_gettime_relative() - relative_start_time) / 1000);
        video_uploaded = 1;
    }

    process_audio_queue();
}

static void s3_upload_audio_internal(AVPacket *pkt) {
    if (!pkt || !pkt->data || !pkt->size) {
        return;
    }

    S3_HLS_FRAME_PACK s3_frame_pack;
    s3_frame_pack.item_count = 1;
    s3_frame_pack.items[0].timestamp = pkt->pts; // use timestamp generated by encoder

    int extra_size = 0;
    uint8_t *extra_data = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, &extra_size);
    if (extra_data && extra_size > 0) {
        s3_frame_pack.items[0].first_part_start = extra_data;
        s3_frame_pack.items[0].first_part_length = extra_size;
        s3_frame_pack.items[0].second_part_start = pkt->data;
        s3_frame_pack.items[0].second_part_length = pkt->size;
    } else {
        s3_frame_pack.items[0].first_part_start = pkt->data;
        s3_frame_pack.items[0].first_part_length = pkt->size;
        s3_frame_pack.items[0].second_part_start = NULL;
        s3_frame_pack.items[0].second_part_length = 0;
    }

    av_log(NULL, AV_LOG_DEBUG, "AU[%ld] upload audio packet: pts=%ld(%ld), duration=%ld, size=%d\n", audio_count, pkt->pts, pkt->pts / 1000, pkt->duration, pkt->size);
    if (put_audio) {
        S3_HLS_SDK_Put_Audio_Frame(&s3_frame_pack);
    }
    audio_last_time = pkt->pts;
    audio_count++;
    if (!audio_uploaded) {
        av_log(NULL, AV_LOG_WARNING, "[sync]@%ld set audio uploaded!\n", (av_gettime_relative() - relative_start_time) / 1000);
        audio_uploaded = 1;
    }
}

static void process_audio_queue() {
    int loop = 0;
    while (video_uploaded && !packet_queue_empty(audio_queue_ctx)) { // need pop from queue
        if (audio_last_time > video_last_time) {
            av_log(NULL, AV_LOG_DEBUG, "[sync] audio_last_time=%ld, video_last_time=%ld, loop=%d\n", audio_last_time / 1000, video_last_time / 1000, loop);
            break;
        }
        AVPacket *pkt = packet_queue_pop(audio_queue_ctx);
        s3_upload_audio_internal(pkt);
        av_packet_free(&pkt);
        loop++;
    }
}

void s3_upload_audio(AVPacket *pkt) {
    if (!pkt || !pkt->data || !pkt->size) {
        return;
    }

    // Upload packets in the queue.
    process_audio_queue();

    // Push this packet to the queue if need wait.
    if (!video_uploaded || !packet_queue_empty(audio_queue_ctx)) {
        if (packet_queue_full(audio_queue_ctx)) {
            av_log(NULL, AV_LOG_WARNING, "[sync] audio packet queue is full\n");
            return;
        }

        av_log(NULL, AV_LOG_DEBUG, "[sync] audio_last_time=%ld, video_last_time=%ld, pts=%ld\n", audio_last_time / 1000, video_last_time / 1000, pkt->pts / 1000);
        packet_queue_push(audio_queue_ctx, pkt);
        return;
    }

    // Upload this packet immediately if need not wait.
    s3_upload_audio_internal(pkt);
}
