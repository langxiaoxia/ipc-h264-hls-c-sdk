#include <libavutil/log.h>
#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>

#include "common.h"

void av_input_init() {
    av_log_set_level(AV_LOG_INFO);
    avdevice_register_all();
    avcodec_register_all();
    av_log(NULL, AV_LOG_WARNING, "[codec] video %s => %s\n", av_get_pix_fmt_name(video_capture_format), av_get_pix_fmt_name(video_encode_format));
    av_log(NULL, AV_LOG_WARNING, "[codec] audio %s(%d) => %s(%d)\n", 
        av_get_sample_fmt_name(audio_capture_format), av_get_bytes_per_sample(audio_capture_format), 
        av_get_sample_fmt_name(audio_encode_format), av_get_bytes_per_sample(audio_encode_format));
}
