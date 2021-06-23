/*
 * Copyright (c) 2020, VeriSilicon Holdings Co., Ltd. All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "vpi_api.h"
#include "vpi_types.h"

typedef struct VpeDeviceContext {
    int device;
    VpiApi *func;
} VpeDeviceContext;

typedef struct VpeFrm {
    int state;
    VpiFrame frame;
} VpeFrm;

typedef struct VpeDecCtx {
    VpiCtx ctx;
    VpiApi *vpi;

    VpiDecOption *dec_setting;

    uint8_t *pp_setting;
    int transcode;

    VpiPacket *buffered_pkt;
    int initialized;
} VpeDecCtx;

typedef struct VpeEncCtx {
    VpiCtx ctx;
    VpiApi *vpi;

    VpiFrame *vpi_frame;
    VpiEncParamSet *param_list;

    int eof;

    char *preset;
    char *profile;
    char *level;
    char *enc_params;
    int crf;
    int force_idr;
    int effort;
    int lag_in_frames;
    int passes;
    int bit_rate;

    void *enc_cfg;
    int initialized;
} VpeEncCtx;

typedef struct VpePPCtx {
    VpiCtx ctx;
    VpiApi *vpi;

    int nb_outputs;
    int force_10bit;
    char *low_res;
    VpiPPOption *option;
    int initialized;
} VpePPCtx;

typedef struct VideoInfo {
    int width;
    int height;
    char *src_pix_fmt;
    uint8_t *data[3];
    int size[3];
    int linesize[3];
} VideoInfo;

typedef struct VpeContext {
    VpeDeviceContext dev_ctx;
    VpiSysInfo *sys_info;
    VpeDecCtx dec_ctx;
    VpeEncCtx enc_ctx;
    VpePPCtx pp_ctx;
    VpiFrame frame_ctx;
    VideoInfo video_info;
    VpeFrm pic_list[MAX_WAIT_DEPTH];
} VpeContext;

typedef struct VpeOption {
    char *key;
    char *value;
} VpeOption;

#define LATENCY_PROFILE

#ifdef LATENCY_PROFILE
#define NFRAMES  10000
struct timeval *in_time = NULL;
struct timeval *out_time = NULL;
#endif

static const char *device = "/dev/transcoder0";
static char *enc_codec;
static char *output_file;

static FILE *input_fp  = NULL;
static FILE *output_fp = NULL;

static int input_frame_cnt = 0;
static int frame_out_cnt;
struct timeval start_time;

static char *strm_data = NULL;
static int base_strm_size = 0x800000;

static const VpeOption device_options[] = { { "priority", "vod" },
                                            { "vpeloglevel", "7" } };
static const VpeOption input_options[] = { { "video_size", "1920x1080" },
                                           { "pixel_format", "yuv420p" } };
static const VpeOption enc_options[] = {
      { "preset", "fast" },
      { "b", "1000000" },
      { "enc_params", "intra_pic_rate=100:gop_size=1" }
};
#define VPE_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))

static int64_t get_time_duration(struct timeval *ctime, struct timeval *ltime)
{
    int64_t duration;
    duration = (ctime->tv_sec - ltime->tv_sec) * 1000000;
    duration += ctime->tv_usec - ltime->tv_usec;
    return duration;
}

static void statistic_info(int force)
{
    static struct timeval last_time = { .tv_sec = 0, .tv_usec = 0 };
    struct timeval curr_time;
    int64_t duration;
    double fps;

    gettimeofday(&curr_time, NULL);
    if (force ||
        (get_time_duration(&curr_time, &last_time) > (1 * 1000 * 1000))) {
        duration = get_time_duration(&curr_time, &start_time);
        fps      = (double)frame_out_cnt /
              ((double)duration / (1 * 1000 * 1000));
        printf("\rframe %5d, fps=%3.1f", frame_out_cnt, fps);
#ifdef LATENCY_PROFILE
        printf(" latency=%3dms \n",
               get_time_duration(out_time+frame_out_cnt-1, in_time+frame_out_cnt-1)/1000);
#endif
        last_time = curr_time;
    }
}

static int create_vpe_device(VpeContext *vpe_ctx)
{
    VpeDeviceContext *device_ctx = &vpe_ctx->dev_ctx;
    int i;
    int ret;

    ret = vpi_get_sys_info_struct(&vpe_ctx->sys_info);
    if (ret || !vpe_ctx->sys_info) {
        printf("failed to get sys info struct\n");
        return -1;
    }

    device_ctx->device = vpi_open_hwdevice(device);
    if (device_ctx->device == -1) {
        printf("failed to open hw device %s\n", device);
        return -1;
    }

    vpe_ctx->sys_info->device        = device_ctx->device;
    vpe_ctx->sys_info->priority      = VPE_TASK_VOD;
    vpe_ctx->sys_info->sys_log_level = 0;

    for (i = 0; i < VPE_ARRAY_ELEMS(device_options); i++) {
        if (!strcmp(device_options[i].key, "priority")) {
            if (!strcmp(device_options[i].value, "live")) {
                vpe_ctx->sys_info->priority = VPE_TASK_LIVE;
            } else if (!strcmp(device_options[i].value, "vod")) {
                vpe_ctx->sys_info->priority = VPE_TASK_VOD;
            } else {
                printf("Unknow priority : %s\n",
                       device_options[i].value);
                return -1;
            }
        } else if (!strcmp(device_options[i].key, "vpeloglevel")) {
            vpe_ctx->sys_info->sys_log_level = atoi(device_options[i].value);
        }
    }

    if (vpi_create((void**)&vpe_ctx->sys_info, &device_ctx->func, device_ctx->device,
                    HWCONTEXT_VPE) != 0) {
        printf("Unable create VPE HW context\n");
        return -1;
    }

    return 0;
}

static void destroy_vpe_device(VpeContext *vpe_ctx)
{
    VpeDeviceContext *device_ctx = &vpe_ctx->dev_ctx;

    vpi_destroy(vpe_ctx->sys_info, device_ctx->device);

    vpi_freep(&vpe_ctx->sys_info);

    if (device_ctx->device >= 0)
        vpi_close_hwdevice(device_ctx->device);
}

static int open_input_file(VpeContext *vpe_ctx, const char *filename)
{
    VideoInfo *v_info = &vpe_ctx->video_info;
    int i;
    char *size;
    char buf[512];
    int width, height;

    for (i = 0; i < VPE_ARRAY_ELEMS(input_options); i++) {
        if (!strcmp(input_options[i].key, "video_size")) {
            size = input_options[i].value;
            sscanf(size, "%dx%d", &(v_info->width), &(v_info->height));
        } else if (!strcmp(input_options[i].key, "pixel_format")) {
            if (strcmp(input_options[i].value, "nv12") &&
                strcmp(input_options[i].value, "yuv420p")) {
                printf("%s format currently not support\n", input_options[i].value);
                return -1;
            }
            v_info->src_pix_fmt = input_options[i].value;
        }
    }

    if (!strcmp(v_info->src_pix_fmt, "nv12")) {
        v_info->linesize[0] = v_info->width;
        v_info->linesize[1] = v_info->width;
        v_info->linesize[2] = 0;
        v_info->size[0] = v_info->width*v_info->height;
        v_info->size[1] = v_info->width*v_info->height/2;
        v_info->size[2] = 0;
        v_info->data[0] = malloc(v_info->size[0]);
        v_info->data[1] = malloc(v_info->size[1]);
        v_info->data[2] = NULL;
    } else if (!strcmp(v_info->src_pix_fmt, "yuv420p")) {
        v_info->linesize[0] = v_info->width;
        v_info->linesize[1] = v_info->width/2;
        v_info->linesize[2] = v_info->width/2;
        v_info->size[0] = v_info->width*v_info->height;
        v_info->size[1] = v_info->width*v_info->height/4;
        v_info->size[2] = v_info->width*v_info->height/4;
        v_info->data[0] = malloc(v_info->size[0]);
        v_info->data[1] = malloc(v_info->size[1]);
        v_info->data[2] = malloc(v_info->size[1]);
    }
    if (filename) {
        input_fp = fopen(filename, "rb");
        if (!input_fp) {
            printf("Can't open output file %s\n", filename);
            return -1;
        }
    } else {
        printf("No valid input file\n");
        return -1;
    }
    return 0;
}

static int vpe_enc_create_param_list(VpeEncCtx *enc_ctx)
{
    char *enc_params              = enc_ctx->enc_params;
    VpiEncParamSet *tail          = NULL;
    VpiEncParamSet *node          = NULL;
    char *param_str;
    char *p_str, *key_str;
    char *key, *value;
    int key_size, value_size;
    int param_size;
    int ret = 0;
    int i;

    for (i = 0; i < VPE_ARRAY_ELEMS(enc_options); i++) {
        if (!strcmp(enc_options[i].key, "enc_params")) {
            enc_params = enc_options[i].value;
            break;
        }
    }
    if (i == VPE_ARRAY_ELEMS(enc_options)) {
        return 0;
    }

    param_size = strlen(enc_params);
    p_str = enc_params;
    do {
        param_str = p_str;
        key_str = strchr(param_str, '=');
        key_size = key_str - param_str;
        key = malloc(key_size+1);
        memcpy(key, param_str, key_size);
        key[key_size] = '\0';

        p_str = strchr(param_str, ':');
        if (!p_str) {
            ret = 1;
            p_str = param_str + param_size;
        }
        key_str++;
        value_size = p_str - key_str;
        value = malloc(value_size+1);
        memcpy(value, key_str, value_size);
        value[value_size] = '\0';

        p_str++;

        node = malloc(sizeof(VpiEncParamSet));
        node->key   = key;
        node->value = value;
        node->next  = NULL;
        if (tail) {
            tail->next = node;
            tail       = node;
        } else {
            enc_ctx->param_list = tail = node;
        }
        if (ret == 1) {
            break;
        } else {
            param_size -= key_size;
            param_size -= value_size;
            param_size -= 2;  // '='':'
        }
    } while(1);

    return 0;
}

/**
 * Release the vpe encoder param list
 */
static void vpe_enc_release_param_list(VpeEncCtx *enc_ctx)
{
    VpiEncParamSet *tail = enc_ctx->param_list;
    VpiEncParamSet *node = NULL;

    while (tail) {
        node = tail->next;
        free(tail->key);
        free(tail->value);
        free(tail);
        tail = node;
    }
}

static int vpe_filter_init(VpeContext *vpe_ctx)
{
    VpePPCtx *pp_ctx = &vpe_ctx->pp_ctx;
    VpeDeviceContext *device_ctx = &vpe_ctx->dev_ctx;
    VideoInfo *v_info = &vpe_ctx->video_info;
    VpiPPOption *option;
    VpiCtrlCmdParam cmd;
    int ret;

    ret = vpi_create(&pp_ctx->ctx, &pp_ctx->vpi, device_ctx->device, PP_VPE);
    if (ret) {
        printf("Failed to create PP_VPE\n");
        return -1;
    }

    // get the pp option struct
    cmd.cmd = VPI_CMD_PP_INIT_OPTION;
    ret = pp_ctx->vpi->control(pp_ctx->ctx, (void *)&cmd, (void *)&pp_ctx->option);
    if (ret) {
        printf("Get PP option struct failed\n");
        return -1;
    }

    option = (VpiPPOption *)pp_ctx->option;
    option->w = v_info->width;
    option->h = v_info->height;
    if (!strcmp(v_info->src_pix_fmt, "nv12")) {
        option->format = VPI_FMT_NV12;
    } else if (!strcmp(v_info->src_pix_fmt, "yuv420p")) {
        option->format = VPI_FMT_YUV420P;
    }
    option->nb_outputs  = 1; //pp_ctx->nb_outputs;
    option->force_10bit = 0; //pp_ctx->force_10bit;
    option->low_res     = NULL; //pp_ctx->low_res;
    option->b_disable_tcache = 0;
    option->frame       = &vpe_ctx->frame_ctx;
    ret = pp_ctx->vpi->init(pp_ctx->ctx, option);
    if (ret) {
        printf("Init PP plugin failed\n");
        return -1;
    }
    pp_ctx->initialized = 1;
    return 0;
}

static void vpe_filter_close(VpeContext *vpe_ctx)
{
    VpePPCtx *pp_ctx = &vpe_ctx->pp_ctx;
    VpeDeviceContext *device_ctx = &vpe_ctx->dev_ctx;

    if (!pp_ctx->initialized)
        return;

    pp_ctx->vpi->close(pp_ctx->ctx);

    vpi_destroy(pp_ctx->ctx, device_ctx->device);
}

static int vpe_encode_init(VpeContext *vpe_ctx)
{
    VpeEncCtx *enc_ctx = &vpe_ctx->enc_ctx;
    VpeDeviceContext *device_ctx = &vpe_ctx->dev_ctx;
    VideoInfo *v_info = &vpe_ctx->video_info;
    VpiPlugin type;
    VpiCtrlCmdParam cmd;
    VpiH26xEncCfg *h26x_enc_cfg;
    int i, ret;

    if (enc_ctx->initialized)
        return 0;

    if (!strcmp(enc_codec, "hevcenc") ||
        !strcmp(enc_codec, "h264enc")) {
        type = H26XENC_VPE;
    } else {
        printf("enc codec %s currently not supported\n", enc_codec);
        return -1;
    }
    if (vpi_create(&enc_ctx->ctx, &enc_ctx->vpi, device_ctx->device, type)) {
        printf("encoder vpe create failed\n");
        return -1;
    }
    ret = vpe_enc_create_param_list(enc_ctx);
    if (ret) {
        printf("vpe_enc_create_param_list failed\n");
        return ret;
    }
    for (i = 0; i < VPE_ARRAY_ELEMS(enc_options); i++) {
        if (!strcmp(enc_options[i].key, "preset")) {
            enc_ctx->preset = enc_options[i].value;
        } else if (!strcmp(enc_options[i].key, "b")) {
            enc_ctx->bit_rate = atoi(enc_options[i].value);
        }
    }
    // get the encoder cfg struct
    cmd.cmd = VPI_CMD_ENC_INIT_OPTION;
    ret = enc_ctx->vpi->control(enc_ctx->ctx, (void *)&cmd,
                                (void *)&enc_ctx->enc_cfg);
    if (ret) {
        printf("get enc init option struct fail\n");
        return -1;
    }
    h26x_enc_cfg = (VpiH26xEncCfg *)enc_ctx->enc_cfg;
    /*Initialize the VPE h26x encoder configuration*/
    if (!strcmp(enc_codec, "hevcenc")) {
        h26x_enc_cfg->codec_id = CODEC_ID_HEVC;
    } else if (!strcmp(enc_codec, "h264enc")) {
        h26x_enc_cfg->codec_id = CODEC_ID_H264;
    }
    h26x_enc_cfg->crf        = enc_ctx->crf;
    h26x_enc_cfg->preset     = enc_ctx->preset;
    h26x_enc_cfg->profile    = enc_ctx->profile;
    h26x_enc_cfg->level      = enc_ctx->level;
    h26x_enc_cfg->force_idr  = enc_ctx->force_idr;
    h26x_enc_cfg->input_rate_numer = 30;
    h26x_enc_cfg->input_rate_denom = 1;
    h26x_enc_cfg->bit_per_second = enc_ctx->bit_rate;
    h26x_enc_cfg->lum_width_src  = vpe_ctx->frame_ctx.width;
    h26x_enc_cfg->lum_height_src = vpe_ctx->frame_ctx.height;
    if (!strcmp(v_info->src_pix_fmt, "nv12")) {
        h26x_enc_cfg->input_format = VPI_YUV420_SEMIPLANAR;
    } else if (!strcmp(v_info->src_pix_fmt, "yuv420p")) {
        h26x_enc_cfg->input_format = VPI_YUV420_PLANAR;
    }
    h26x_enc_cfg->frame_ctx  = &vpe_ctx->frame_ctx;
    h26x_enc_cfg->param_list = enc_ctx->param_list;

    h26x_enc_cfg->colour_primaries         = 2;
    h26x_enc_cfg->transfer_characteristics = 2;
    h26x_enc_cfg->matrix_coeffs            = 2;

    /*Call the VPE h26x encoder initialization function*/
    ret = enc_ctx->vpi->init(enc_ctx->ctx, h26x_enc_cfg);
    if (ret < 0) {
        printf("vpe_h26x_encode_init failed, error=%s(%d)\n",
                vpi_error_str(ret), ret);
        return -1;
    }
    enc_ctx->initialized = 1;
    return 0;
}

static int vpe_encode_close(VpeContext *vpe_ctx)
{
    VpeEncCtx *enc_ctx = &vpe_ctx->enc_ctx;
    VpeDeviceContext *device_ctx = &vpe_ctx->dev_ctx;

    if (!enc_ctx->initialized)
        return 0;

    vpe_enc_release_param_list(enc_ctx);
    if (enc_ctx->ctx)
        enc_ctx->vpi->close(enc_ctx->ctx);
    //vpe_enc_consume_flush(avctx);
    if (enc_ctx->enc_cfg)
        free(enc_ctx->enc_cfg);
    if (enc_ctx->ctx) {
        if (vpi_destroy(enc_ctx->ctx, device_ctx->device)) {
            printf("encoder vpi_destroy failure\n");
            return -1;
        }
    }

    return 0;
}

static int vpe_read_frame(VpeContext *vpe_ctx, FILE *in_fp, VpiFrame *in_frame)
{
    VideoInfo *v_info = &vpe_ctx->video_info;
    int i;
    int ret;

    in_frame->width = v_info->width;
    in_frame->height = v_info->height;
    in_frame->linesize[0] = v_info->linesize[0];
    in_frame->linesize[1] = v_info->linesize[1];
    in_frame->linesize[2] = v_info->linesize[2];
    in_frame->key_frame   = 1;
    in_frame->pts         = input_frame_cnt;
    in_frame->pkt_dts     = input_frame_cnt;
    in_frame->data[0]     = v_info->data[0];
    in_frame->data[1]     = v_info->data[1];
    in_frame->data[2]     = v_info->data[2];
    input_frame_cnt++;

    for (i = 0; i < 3; i++) {
        if (v_info->size[i]) {
            ret = fread(in_frame->data[i], 1, v_info->size[i], in_fp);
            if (ret != v_info->size[i]) {
                ret = feof(in_fp);
                if (ret == 1) {
                    //EOF
                    return 1;
                } else {
                    //ERROR
                    return -1;
                }
            }
        }
    }
    return 0;
}

static int encode_frame(VpeContext *vpe_ctx, VpiFrame *frame, VpiPacket *pkt)
{
    VpeEncCtx *enc_ctx = &vpe_ctx->enc_ctx;
    VpiFrame *vpi_frame = NULL;
    VpiCtrlCmdParam cmd;
    int ret;
    int stream_size = 0;

    /* send the frame to the encoder */
    cmd.cmd  = VPI_CMD_ENC_GET_EMPTY_FRAME_SLOT;
    cmd.data = NULL;
    ret = enc_ctx->vpi->control(enc_ctx->ctx,
                    (void*)&cmd, (void *)&vpi_frame);
    if (ret || !vpi_frame)
        return -1;
    if (frame) {
        memcpy(vpi_frame, frame, sizeof(VpiFrame));
        vpi_frame->opaque     = (void *)frame;
        vpi_frame->vpi_opaque = (void *)frame;
    } else {
        printf("input image is empty, received EOF\n");
        memset(vpi_frame, 0, sizeof(VpiFrame));
        enc_ctx->eof = 1;
    }

    ret = enc_ctx->vpi->encode_put_frame(enc_ctx->ctx, (void*)vpi_frame);
    if (ret)
        return -1;

    while (ret == 0) {
        cmd.cmd = VPI_CMD_ENC_GET_FRAME_PACKET;
        ret = enc_ctx->vpi->control(enc_ctx->ctx, &cmd, (void *)&stream_size);
        if (ret == -1) {
            return 0;
        } else if (ret == 1) {
            printf("received EOF from enc\n");
            return 1;
        }

        if (base_strm_size < stream_size) {
            free(strm_data);
            base_strm_size = (stream_size + 0xFFFF) & (~0xFFFF);
            strm_data = malloc(base_strm_size);
            if (!strm_data) {
                return -1;
            }
        }
        pkt->data = strm_data;
        pkt->size = stream_size;
        ret = enc_ctx->vpi->encode_get_packet(enc_ctx->ctx, (void *)pkt);
        if (ret) {
            printf("enc encode failed, error=%s(%d)\n", vpi_error_str(ret), ret);
            ret = -1;
        }
        if (output_fp) {
            fwrite(pkt->data, 1, pkt->size, output_fp);
            fflush(output_fp);
        }
#ifdef LATENCY_PROFILE
        if (frame_out_cnt < NFRAMES ) {
            gettimeofday(out_time + frame_out_cnt, NULL);
        }
#endif
        frame_out_cnt++;
    }

    return 0;
}

static int encode_flush(VpeContext *vpe_ctx, VpiPacket *pkt)
{
    VpeEncCtx *enc_ctx = &vpe_ctx->enc_ctx;
    VpiCtrlCmdParam cmd;
    int stream_size;
    int ret;

    for (;;) {
        cmd.cmd = VPI_CMD_ENC_GET_FRAME_PACKET;
        ret = enc_ctx->vpi->control(enc_ctx->ctx, &cmd, (void *)&stream_size);
        if (ret == -1) {
            usleep(500);
            continue;
        } else if (ret == 1) {
            printf("received EOF from enc\n");
            return 1;
        }
        pkt->data = malloc(stream_size);
        pkt->size = stream_size;
        ret = enc_ctx->vpi->encode_get_packet(enc_ctx->ctx, (void *)pkt);
        if (ret) {
            printf("enc encode failed, error=%s(%d)\n", vpi_error_str(ret), ret);
            return -1;
        }

        if (output_fp) {
            fwrite(pkt->data, 1, pkt->size, output_fp);
            fflush(output_fp);
        }
#ifdef LATENCY_PROFILE
        if (frame_out_cnt < NFRAMES ) {
            gettimeofday(out_time + frame_out_cnt, NULL);
        }
#endif
        frame_out_cnt++;
        free(pkt->data);
    }
    return 0;
}

static int vpe_get_empty_pic(VpeContext *vpe_ctx)
{
    int i;

    for (i = 0; i < MAX_WAIT_DEPTH; i++) {
        if (vpe_ctx->pic_list[i].state == 0) {
            return i;
        }
    }
    return -1;
}

static int vpe_enc_consume_pic(VpeContext *vpe_ctx, VpiFrame *consume_frame)
{
    VpiFrame *frame;
    int i;

    for (i = 0; i < MAX_WAIT_DEPTH; i++) {
        if (vpe_ctx->pic_list[i].state == 1) {
            frame = &(vpe_ctx->pic_list[i].frame);
            if (frame == consume_frame) {
                vpe_ctx->pic_list[i].state = 0;
                break;
            }
        }
    }

    if (i == MAX_WAIT_DEPTH) {
        printf("frame %p not matched\n", consume_frame);
        return -1;
    }

    return 0;
}

static int vpe_free_pic(VpeContext *vpe_ctx)
{
    VpeEncCtx *enc_ctx = &vpe_ctx->enc_ctx;
    int i, ret;
    VpiFrame *frame;
    VpiCtrlCmdParam cmd;
    VpiFrame *frame_ref = NULL;

    do {
        cmd.cmd = VPI_CMD_ENC_CONSUME_PIC;
        ret     = enc_ctx->vpi->control(enc_ctx->ctx, (void *)&cmd,
                                        (void *)&frame_ref);
        if (ret < 0)
            return -1;

        if (frame_ref) {
            ret = vpe_enc_consume_pic(vpe_ctx, frame_ref);
            if (ret)
                return ret;
        } else {
            break;
        }
    } while(1);
    return 0;
}

int main(int argc, char **argv)
{
    VpeContext vpe_context = {0};
    VpePPCtx *pp_ctx = &vpe_context.pp_ctx;
    VpiFrame v_frame = {0};
    VpiFrame *filter_frame = NULL;
    VpiPacket v_pkt = {0};
    int ret, idx;

    printf("transcode test start\n");

    if (argc <= 2) {
        printf("Usage: %s <input file> <codec name> <output file>\n", argv[0]);
        return 1;
    }

#ifdef LATENCY_PROFILE
    in_time = malloc(NFRAMES * sizeof(struct timeval));
    if (!in_time) {
        printf("malloc in_time error\n");
        goto failure;
    }
    out_time = malloc(NFRAMES * sizeof(struct timeval));
    if (!out_time) {
        printf("malloc out_time error\n");
        goto failure;
    }
#endif

    strm_data = malloc(base_strm_size);
    if (!strm_data) {
        printf("malloc stream size error\n");
        goto failure;
    }

    ret = create_vpe_device(&vpe_context);
    if (ret < 0) {
        printf("Failed to init a VPE device\n");
        return -1;
    }

    enc_codec = malloc(128);
    strcpy(enc_codec, argv[2]);

    if (argc <= 3) {
        output_file = NULL;
    } else {
        output_file = malloc(128);
        strcpy(output_file, argv[3]);
        output_fp = fopen(output_file, "wb");
        if (output_fp == NULL) {
            printf("Failed to open output file\n");
            return -1;
        }
    }

    ret = open_input_file(&vpe_context, argv[1]);
    if (ret < 0) {
        printf("Failed to open input file\n");
        goto failure;
    }
    ret = vpe_filter_init(&vpe_context);
    if (ret < 0) {
        printf("Failed to init VPE filter\n");
        goto failure;
    }

    gettimeofday(&start_time, NULL);
    while (1) {
        ret = vpe_read_frame(&vpe_context, input_fp, &v_frame);
        if (ret == 0) {
            if (vpe_context.enc_ctx.initialized) {
                vpe_free_pic(&vpe_context);
            }

#ifdef LATENCY_PROFILE
            static int framein_count = 0;
            if (framein_count < NFRAMES) {
                gettimeofday(in_time + framein_count, NULL);
                framein_count ++;
            }
#endif
            //push to filter
            idx = vpe_get_empty_pic(&vpe_context);
            if (idx == -1) {
                goto failure;
            }
            filter_frame = &(vpe_context.pic_list[idx].frame);
            ret = pp_ctx->vpi->process(pp_ctx->ctx, &v_frame, filter_frame);
            if (ret) {
                goto failure;
            }
            vpe_context.pic_list[idx].state = 1;
            if (!(vpe_context.enc_ctx.initialized)) {
                ret = vpe_encode_init(&vpe_context);
                if (ret < 0) {
                    printf("Failed to init VPE encoder\n");
                    goto failure;
                }
            }
            encode_frame(&vpe_context, filter_frame, &v_pkt);
            statistic_info(0);
        } else {
            encode_frame(&vpe_context, NULL, &v_pkt);
            break;
        }
    }
    //encode_flush(&vpe_context, &v_pkt);

    statistic_info(1);
failure:
    if (enc_codec)
        free(enc_codec);

    if (input_fp)
        fclose(input_fp);

    if (output_fp)
        fclose(output_fp);

    if (output_file)
        free(output_file);

    vpe_encode_close(&vpe_context);
    vpe_filter_close(&vpe_context);

    if (strm_data)
        free(strm_data);

#ifdef LATENCY_PROFILE
    if (in_time)
        free(in_time);

    if (out_time)
        free(out_time);
#endif

    destroy_vpe_device(&vpe_context);

    printf("transcode test finish\n");
    return 0;
}
