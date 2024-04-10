/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: MIT-0
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 *. Format:
 *
 *  TS Header               4 bytes
 *      0x47        sync
 *      0x50        payload_start + PID 0x1000
 *      0x00        pid
 *      0x30        contains payload data (0x10 always exists) and adaption field (0x20)
 *      0x00        adaption length field
 *
 *. PMT Header
 *      0x02        table_id
 *      0xb0        0b1 (0x80) - section _syntax_indicator, 0b0 (0x40) - zero bit, 0b11 (0x30) - reserved always 0b11, 0b0000 - frist 4 bits of section length
 *      0x17/12     remaining 8 bits of section length
 *      0x00        first 8 bits of program number
 *      0x01        last 8 bits of program number
 *      0xc1        0b11 (0xC0) - reserved always 0b11, version number - 0b00000 when pmt change add 1, current_next_indicator 0b1 (0x01) when 1 current program map is available
 *      0x00        0x00 section_number fix
 *      0x00        0x00 last_section_number fix
 *      0xe1        0b111 (0xe0) 0x1F (first 5 bits of PID that contains PCR, value is 0x01)
 *      0x00        0x00 last 8 bits for PID
 *      0xf0        reserved 0xf0, 4 bits for program_info_length (first 4 bits)
 *      0x00        last 8 bits of program_info_length
 *      0x1b        stream_type, 0x1b for H264 video, 0x0f for ISO13818-7 (AAC LC?), 0x03 for ISO 11172-3 (mp3?)
 *      0xe1        0xe0 reserved 0x01 first 5 bits of PID
 *      0x00        remaining 8 bits of PID
 *      0xf0        0xf0 reserved 0x00 first 4 bits of es info length
 *      0x00        0x00 remaining 8 bits of es info length
 *
 *      // audio part start
 *      0x0f        stream_type
 *      0xe1        0xe0 reserved 0x01 first 5 bits of PID
 *      0x01        remaining 8 bits of PID
 *      0xf0        0xf0 reserved 0x00 first 4 bits of es info length
 *      0x00        0x00 remaining 8 bits of es info length
 *
 *      // CRC start
 *      0x2f
 *      0x44
 *      0xb9
 *      0x9b
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "S3_HLS_Pmt.h"
#include "S3_HLS_Buffer_Mgr.h"
#include "S3_HLS_Return_Code.h"

//#define S3_HLS_PMT_DEBUG

#ifdef S3_HLS_PMT_DEBUG
#define PMT_DEBUG(x, ...) printf(x, ##__VA_ARGS__)
#else
#define PMT_DEBUG(x, ...)
#endif

#define S3_HLS_PMT_HEADER_LENGTH        188

// Video + Audio AAC
uint8_t const_pmt_aac[31] = { 0x47, 0x50, 0x00, 0x10, 0x00, 0x02, 0xb0, 0x17, // length
                              0x00, 0x01, 0xc1, 0x00, 0x00, 0xe1, 0x00, 0xf0, // PCR PID = 0x0100,
                              0x00, 0x1b, 0xe1, 0x00, 0xf0, 0x00, 0x0f, 0xe1, // PID 0x100 is video, PID 0x101 is audio
                              0x01, 0xf0, 0x00, 0x2f, 0x44, 0xb9, 0x9b    }; // last 4 bytes are crc

// // Video + Audio MP3? need generate crc
uint8_t const_pmt_mp3[31] = { 0x47, 0x50, 0x00, 0x10, 0x00, 0x02, 0xb0, 0x17, // length
                              0x00, 0x01, 0xc1, 0x00, 0x00, 0xe1, 0x00, 0xf0, // PCR PID = 0x0100,
                              0x00, 0x1b, 0xe1, 0x00, 0xf0, 0x00, 0x03, 0xe1, // PID 0x100 is video, PID 0x101 is audio
                              0x01, 0xf0, 0x00, 0x4e, 0x59, 0x3d, 0x1e    }; // last 4 bytes are crc

// Video Only
uint8_t const_pmt_video[26] = { 0x47, 0x50, 0x00, 0x10, 0x00, 0x02, 0xb0, 0x12, // length
                                0x00, 0x01, 0xc1, 0x00, 0x00, 0xe1, 0x00, 0xf0, // PCR PID = 0x0100,
                                0x00, 0x1b, 0xe1, 0x00, 0xf0, 0x00, 0x15, 0xbd,
                                0x4d, 0x56  }; // last 4 bytes are crc

uint8_t *const_pmt = const_pmt_video;
uint32_t pmt_size = sizeof(const_pmt_video);

int8_t m_pmt_counter = 0;

int32_t S3_HLS_H264_PMT_Write_To_Buffer(S3_HLS_BUFFER_CTX* buffer_ctx) {
    PMT_DEBUG("Writing PMT!\n");
    int32_t ret;

    const_pmt[S3_HLS_TS_COUNTER_INDEX] &= 0xF0;
    const_pmt[S3_HLS_TS_COUNTER_INDEX] |= (m_pmt_counter & 0x0F);

    PMT_DEBUG("Put PMT to buffer!\n");
    ret = S3_HLS_Put_To_Buffer(buffer_ctx, const_pmt, pmt_size);
    if(0 > ret)
        return ret;

    ret = S3_HLS_Put_To_Buffer(buffer_ctx, const_fill_word, S3_HLS_PMT_HEADER_LENGTH - pmt_size);
    if(0 > ret)
        return ret;

    m_pmt_counter++;

    return S3_HLS_PMT_HEADER_LENGTH;
}

void S3_HLS_PMT_Reset_Counter() {
    m_pmt_counter = 0;
}

void S3_HLS_PMT_Set_Audio(int audio) {
  switch (audio) {
    case 1:
      const_pmt = const_pmt_aac;
      pmt_size = sizeof(const_pmt_aac);
      printf("pmt aac\n");
      break;
    case 2:
      const_pmt = const_pmt_mp3;
      pmt_size = sizeof(const_pmt_mp3);
      printf("pmt mp3\n");
      break;
    default:
      const_pmt = const_pmt_video;
      pmt_size = sizeof(const_pmt_video);
      printf("pmt video\n");
      break;
  }
}
