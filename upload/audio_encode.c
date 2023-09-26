#include <stdio.h>

#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>

#include "common.h"
#include "bitstream.h"

//#define S3_HLS_AAC

#ifdef S3_HLS_AAC // AAC
static const enum AVCodecID audio_encode_id = AV_CODEC_ID_AAC;
#else // MP3
static const enum AVCodecID audio_encode_id = AV_CODEC_ID_MP3;
#endif

static int pack_num = 1;
static const int samples_per_pack = 128;
const enum AVSampleFormat audio_encode_format = AV_SAMPLE_FMT_FLTP;

#define ADTS_HEADER_SIZE 7
#define ADTS_MAX_FRAME_BYTES ((1 << 13) - 1)

static AVCodecContext *enc_ctx = NULL;
static SwrContext *swr_ctx = NULL;
static AVFrame *out_frame = NULL;
static AVFrame *in_frame = NULL;

static char out_path[256] = "/data/s3_upload/alsa.";
static FILE *out_file = NULL;

static AVCodecContext* open_audio_encoder(enum AVSampleFormat sample_fmt, int sample_rate, uint64_t channel_layout) {
    AVCodec *codec = avcodec_find_encoder(audio_encode_id);
    if (!codec) {
        av_log(NULL, AV_LOG_ERROR, "[codec] Not found %s encoder!\n", avcodec_get_name(audio_encode_id));
        return NULL;
    }
    av_log(NULL, AV_LOG_WARNING, "[codec] Found %s encoder: %s\n", avcodec_get_name(audio_encode_id), codec->long_name);

    enc_ctx = avcodec_alloc_context3(codec);
    if (!enc_ctx) {
        av_log(NULL, AV_LOG_ERROR, "[codec] Could not allocate %s encoder context!\n", avcodec_get_name(audio_encode_id));
        return NULL;
    }

    switch (audio_encode_id) {
    case AV_CODEC_ID_AAC:
        enc_ctx->profile = FF_PROFILE_AAC_LOW;
        break;
    default:
        break;
    }

    // 设置输入格式
    enc_ctx->sample_fmt = sample_fmt;
    enc_ctx->channel_layout = channel_layout;

    // 设置码率
    enc_ctx->bit_rate = 64000 * av_get_channel_layout_nb_channels(channel_layout); // 64 kbps per channel

    // 设置sample rate
    enc_ctx->sample_rate = sample_rate;
    enc_ctx->time_base = AV_TIME_BASE_Q; // 帧与帧之间的间隔是time_base

    int ret = avcodec_open2(enc_ctx, codec, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "[codec] Could not open %s encoder: [%d]%s!\n", avcodec_get_name(audio_encode_id), ret, av_err2str(ret));
        return NULL;
    }

    pack_num = enc_ctx->frame_size / samples_per_pack;
    av_log(NULL, AV_LOG_WARNING, "[codec] %s encoder: channels=%d, sample_rate=%d, frame_size=%d, pack_num=%d\n", avcodec_get_name(audio_encode_id), enc_ctx->channels, enc_ctx->sample_rate, enc_ctx->frame_size, pack_num);
    return enc_ctx;
}

static AVFrame *create_audio_frame(enum AVSampleFormat sample_fmt, int sample_rate, uint64_t channel_layout, int nb_samples) {
    int ret = 0;
    AVFrame *frame = NULL;

    frame = av_frame_alloc();
    if (!frame) {
        av_log(NULL, AV_LOG_ERROR, "[AE] Failed to alloc frame!\n");
        goto __ERROR;
    }

    // 设置参数
    frame->format = sample_fmt;
    frame->sample_rate = sample_rate;
    frame->channel_layout = channel_layout;
    frame->nb_samples = nb_samples;

    // alloc inner memory
    ret = av_frame_get_buffer(frame, 32);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "[AE] Failed to alloc buffer for frame: [%d]%s!\n", ret, av_err2str(ret));
        goto __ERROR;
    }

    return frame;

__ERROR:
    if (frame) {
        av_frame_free(&frame);
    }

    return NULL;
}

static int adts_write_frame_header(uint8_t *buf, uint16_t size) {
    if (size > (ADTS_MAX_FRAME_BYTES - ADTS_HEADER_SIZE)) {
		av_log(NULL, AV_LOG_ERROR, "[AE] ADTS frame size too large: %hu (max %hu)\n", size, ADTS_MAX_FRAME_BYTES);
        return -1;
    }

    struct bitstream_writer_t bsw;
    bitstream_writer_init(&bsw, buf);

    /* adts_fixed_header */
    bitstream_writer_write_u64_bits(&bsw, 0xfff, 12);   /* syncword (always 0xFFF) */
    bitstream_writer_write_u64_bits(&bsw, 1, 1);        /* ID (0: MPEG-4, 1: MPEG-2) */
    bitstream_writer_write_u64_bits(&bsw, 0, 2);        /* layer (always 00) */
    bitstream_writer_write_u64_bits(&bsw, 1, 1);        /* protection_absent */
    bitstream_writer_write_u64_bits(&bsw, enc_ctx->profile, 2);        /* profile_objecttype (1: AAC LC) */
    switch (A_ENCODE_SAMPLE_RATE) {
        case 48000:
            bitstream_writer_write_u64_bits(&bsw, 3, 4);        /* sample_rate_index (3: 48000 Hz, 4: 44100 Hz, 11: 8000 Hz) */
            break;
        case 44100:
            bitstream_writer_write_u64_bits(&bsw, 4, 4);        /* sample_rate_index (3: 48000 Hz, 4: 44100 Hz, 11: 8000 Hz) */
            break;
        case 8000:
            bitstream_writer_write_u64_bits(&bsw, 11, 4);        /* sample_rate_index (3: 48000 Hz, 4: 44100 Hz, 11: 8000 Hz) */
            break;
        default:
            break;
    }
    bitstream_writer_write_u64_bits(&bsw, 0, 1);        /* private_bit */
    bitstream_writer_write_u64_bits(&bsw, A_ENCODE_CHANNELS, 3);        /* channel_configuration (1: mono, 2: stereo) */
    bitstream_writer_write_u64_bits(&bsw, 0, 1);        /* original_copy */
    bitstream_writer_write_u64_bits(&bsw, 0, 1);        /* home */

    /* adts_variable_header */
    bitstream_writer_write_u64_bits(&bsw, 0, 1);        /* copyright_identification_bit */
    bitstream_writer_write_u64_bits(&bsw, 0, 1);        /* copyright_identification_start */
    bitstream_writer_write_u64_bits(&bsw, ADTS_HEADER_SIZE + size, 13);	/* aac_frame_length */
    bitstream_writer_write_u64_bits(&bsw, 0x7ff, 11);   /* adts_buffer_fullness */
    bitstream_writer_write_u64_bits(&bsw, 0, 2);        /* number_of_raw_data_blocks_in_frame */

    return 0;
}

void audio_encode_frame(AVPacket *pkt, AV_ENCODE_CALLBACK cb) {
    static int64_t encode_count = 0;
    static int pack_count = 0;

    AVFrame *frame = NULL;

    if (pkt) {
        int nb_samples = 0;
        if (pack_num > 1) {
            nb_samples = pkt->size / av_get_bytes_per_sample(audio_capture_format);
            if (nb_samples > samples_per_pack) {
                av_log(NULL, AV_LOG_ERROR, "[AE] capture samples %d > samples_per_pack %d\n", nb_samples, samples_per_pack);
                return;
            }

            if (pack_count == 0) {
                out_frame->pts = pkt->pts;
                out_frame->pkt_duration = 0;
            }
            out_frame->pkt_duration += pkt->duration;

            memcpy(*in_frame->extended_data + pack_count * pkt->size, pkt->data, pkt->size);

            pack_count++;
            if (pack_count < pack_num) {
                return;
            }
            pack_count = 0;

            nb_samples = swr_convert(swr_ctx, out_frame->extended_data, out_frame->nb_samples, (const uint8_t **)in_frame->extended_data, in_frame->nb_samples);
        } else {
            out_frame->pts = pkt->pts;
            out_frame->pkt_duration = pkt->duration;

            memcpy(*in_frame->extended_data, pkt->data, pkt->size);

            nb_samples = swr_convert(swr_ctx, out_frame->extended_data, out_frame->nb_samples, (const uint8_t **)in_frame->extended_data, pkt->size / 2);
        }

        frame = out_frame;
        av_log(NULL, AV_LOG_DEBUG, "[AE] send audio frame: pts=%ld(%ld), duration=%ld, nb_samples=%d\n", frame->pts, frame->pts / 1000, frame->pkt_duration, nb_samples);
    } else {
        av_log(NULL, AV_LOG_WARNING, "[sync]@%ld send flush audio frame\n", (av_gettime_relative() - relative_start_time) / 1000);
    }

    // 送原始数据给编码器进行编码
    int ret = avcodec_send_frame(enc_ctx, frame);
    if (ret) {
        av_log(NULL, AV_LOG_ERROR, "[AE] Failed to send a audio frame to the encoder: [%d]%s!\n", ret, av_err2str(ret));
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
            av_log(NULL, AV_LOG_DEBUG, "AE[%ld] receive audio packet: pts=%ld(%ld), duration=%ld, size=%d\n", encode_count, enc_pkt.pts, enc_pkt.pts / 1000, enc_pkt.duration, enc_pkt.size);
            encode_count++;
            break;

        case AVERROR(EAGAIN):
            av_log(NULL, AV_LOG_TRACE, "[AE] audio encoder output is not available\n");
            return;

        case AVERROR_EOF:
            av_log(NULL, AV_LOG_WARNING, "[sync]@%ld audio encoder has been fully flushed: encode_count=%ld\n", (av_gettime_relative() - relative_start_time) / 1000, encode_count);
            return;

        default:
            av_log(NULL, AV_LOG_ERROR, "[AE] audio encoder error: [%d]%s!\n", ret, av_err2str(ret));
            return;
        }

        if (AV_CODEC_ID_AAC == audio_encode_id) {
            uint8_t *adts_header = av_packet_new_side_data(&enc_pkt, AV_PKT_DATA_NEW_EXTRADATA, ADTS_HEADER_SIZE);
            adts_write_frame_header(adts_header, enc_pkt.size);
        }

        int extra_size = 0;
        uint8_t *extra_data = av_packet_get_side_data(&enc_pkt, AV_PKT_DATA_NEW_EXTRADATA, &extra_size);
        if (1 == encode_count) {
            av_log(NULL, AV_LOG_WARNING, "[AE] audio packet extra_size=%d\n", extra_size);
        }
        if (extra_data && extra_size > 0) {
            fwrite(extra_data, 1, extra_size, out_file);
        }
        fwrite(enc_pkt.data, 1, enc_pkt.size, out_file);
        fflush(out_file);

        if (cb) {
            cb(&enc_pkt);
        }

        av_packet_unref(&enc_pkt);
    }
}

int audio_encode_open() {
    if (enc_ctx) {
        return 0;
    }

    strcat(out_path, avcodec_get_name(audio_encode_id));
    out_file = fopen(out_path, "wb+");
    if (!out_file) {
        goto __ERROR;
    }

    enc_ctx = open_audio_encoder(audio_encode_format, A_ENCODE_SAMPLE_RATE, A_ENCODE_CHANNEL_LAYOUT);
    if (!enc_ctx) {
        goto __ERROR;
    }

    swr_ctx = swr_alloc_set_opts(NULL, 
        A_ENCODE_CHANNEL_LAYOUT, audio_encode_format, A_ENCODE_SAMPLE_RATE, // output
        A_CAPTURE_CHANNEL_LAYOUT, audio_capture_format, A_CAPTURE_SAMPLE_RATE, // input
        0, NULL);
    if (!swr_ctx) {
        goto __ERROR;
    }
    if (swr_init(swr_ctx) < 0) {
        goto __ERROR;
    }

    out_frame = create_audio_frame(audio_encode_format, A_ENCODE_SAMPLE_RATE, A_ENCODE_CHANNEL_LAYOUT, enc_ctx->frame_size);
    if (!out_frame) {
        goto __ERROR;
    }
    in_frame = create_audio_frame(audio_capture_format, A_CAPTURE_SAMPLE_RATE, A_CAPTURE_CHANNEL_LAYOUT, enc_ctx->frame_size);
    if (!in_frame) {
        goto __ERROR;
    }

    return 0;

__ERROR:
    audio_encode_close();
    return -1;
}

void audio_encode_close() {
    if (in_frame) {
        av_frame_free(&in_frame);
    }
    if (out_frame) {
        av_frame_free(&out_frame);
    }

    if (swr_ctx) {
        swr_free(&swr_ctx);
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
