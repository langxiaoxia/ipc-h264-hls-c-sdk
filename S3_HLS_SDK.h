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


#ifndef __S3_HLS_SDK_H__
#define __S3_HLS_SDK_H__

#include "stdint.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* End of #ifdef __cplusplus */

#define S3_HLS_SIMPLE_PUT_MAX_FRAME_PER_PACK        4

typedef struct s3_hls_frame_item_s {
    uint8_t* first_part_start;      // start of the buffer address
    uint32_t first_part_length;     // the length of the first part video buffer

    uint8_t* second_part_start;     // when using ring buffer there might have second part of video buffer
    uint32_t second_part_length;    // if not using ring buffer, just set second_part_start to NULL and set second_part_length to 0

    uint64_t timestamp;
} S3_HLS_FRAME_ITEM;

typedef struct frame_packs_s {
    S3_HLS_FRAME_ITEM   items[S3_HLS_SIMPLE_PUT_MAX_FRAME_PER_PACK];
    uint32_t            item_count;
} S3_HLS_FRAME_PACK;

/*
 * Initialize S3 client
 * Parameters:
 *   region - provide the region code like "us-east-1" where the bucket is created
 *   bucket - name of video bucket
 *   prefix - path to store the video in the bucket. Usually this is the certificate iD of the IPC when using AWS IoT Things Management
 *   endpoint - optional parameter, if using default endpiont then can set this parameter to NULL
 *
 * Note:
 *   These paremeters are not allowed to change after initialized.
 */
int32_t S3_HLS_SDK_Initialize(uint32_t buffer_size, char* region, char* bucket, char* prefix, char* endpint, uint64_t seq, int audio);

/*
 * Update Credential used to connect to S3
 * The credential is locked during generating request headers for SIgnature V4. And will release the lock during uploading.
 * Parameter:
 *   ak - Access Key
 *   sk - Secret Access Key
 *   token - token generated by STS for temporary credential
 *
 * Note:
 *   Suggest to use this SDK with AWS IoT Things Management. JITP will be a good choice.
 *   Suggest to rotate credential several minutes/seconds before old credential expires to avoid unsuccessful upload
 */
int32_t S3_HLS_SDK_Set_Credential(char* ak, char* sk, char* token);

/*
 *
 */
int32_t S3_HLS_SDK_Set_Tag(char* object_tag);

/*
 * Finalize will release resources allocated
 * Note: Finalize will not free input parameter like ak, sk, token, region, bucket, prefix, endpoint etc.
 */
int32_t S3_HLS_SDK_Finalize();

/*
 * Start a back ground thread for uploading
 */
int32_t S3_HLS_SDK_Start_Upload();

/*
 * User call this method to put video stream into buffer
 * The pack contains an array of H264 frames.
 * For most of the time, each image pack will contain only one frame
 * But usually SPS/PPS/SEI frames comes together with I frame within a pack
 * In that case, the pack will contains 4 frames
 */
int32_t S3_HLS_SDK_Put_Video_Frame(S3_HLS_FRAME_PACK* pack);

/*
 * User call this method to put audio stream into buffer
 * Currently the only supported audio frame type is AAC encoded frame
 */
int32_t S3_HLS_SDK_Put_Audio_Frame(S3_HLS_FRAME_PACK* pack);

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* End of #ifdef __cplusplus */

#endif
