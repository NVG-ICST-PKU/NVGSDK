/*
 * Copyright (c) 2011 Stefano Sabatini
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * libavfilter virtual input device
 */

/* #define DEBUG */

#include <float.h>              /* DBL_MIN, DBL_MAX */
#include <string.h>

#include "libavutil/bprint.h"
#include "libavutil/channel_layout.h"
#include "libavutil/file.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavformat/avio_internal.h"
#include "libavformat/internal.h"
#include "avdevice.h"

#define WHITESPACES " \n\t\r"
typedef struct VRContext{
    AVClass *class;          ///< class for private options
    char          *graph_str;
    char          *graph_filename;
    char          *dump_graph;
    AVFilterGraph *graph;
    AVFilterContext **sinks;
    int *sink_stream_map;
    int *sink_eof;
    int *stream_sink_map;
    int *sink_stream_subcc_map;
    AVFrame *decoded_frame;
    int nb_sinks;
    AVPacket subcc_packet;
    
    double x;
    double y;    //orientation coordinate
    int width;
    int height;  //tile width and height
    int col;
    int row;     //tile layout
    char *url;   //mpd file
    char *ori;
} VRContext;

static int *create_all_formats(int n)
{
    int i, j, *fmts, count = 0;

    for (i = 0; i < n; i++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(i);
        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
            count++;
    }

    if (!(fmts = av_malloc((count+1) * sizeof(int))))
        return NULL;
    for (j = 0, i = 0; i < n; i++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(i);
        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
            fmts[j++] = i;
    }
    fmts[j] = -1;
    return fmts;
}

av_cold static int vr_read_close(AVFormatContext *avctx)
{
    VRContext *vr = avctx->priv_data;

    av_freep(&vr->sink_stream_map);
    av_freep(&vr->sink_eof);
    av_freep(&vr->stream_sink_map);
    av_freep(&vr->sink_stream_subcc_map);
    av_freep(&vr->sinks);
    avfilter_graph_free(&vr->graph);
    av_frame_free(&vr->decoded_frame);

    return 0;
}

static int create_subcc_streams(AVFormatContext *avctx)
{
    VRContext *vr = avctx->priv_data;
    AVStream *st;
    int stream_idx, sink_idx;

    for (stream_idx = 0; stream_idx < vr->nb_sinks; stream_idx++) {
        sink_idx = vr->stream_sink_map[stream_idx];
        if (vr->sink_stream_subcc_map[sink_idx]) {
            vr->sink_stream_subcc_map[sink_idx] = avctx->nb_streams;
            if (!(st = avformat_new_stream(avctx, NULL)))
                return AVERROR(ENOMEM);
            st->codecpar->codec_id = AV_CODEC_ID_EIA_608;
            st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
        } else {
            vr->sink_stream_subcc_map[sink_idx] = -1;
        }
    }
    return 0;
}

static int set_opts(char *filter, int x, int y, char *url, char *in, char* in_second, char *out, int flag){
    char cmd[2048];
   
    if(filter[0]=='n'){
        snprintf(cmd, sizeof(cmd),"nullsrc=size=%dx%d %s;",x,y,out);
    }
    else if(filter[0]=='v'){
        snprintf(cmd, sizeof(cmd),"vrmovie=%s %s;",url, out);
    }
    else if(filter[0]=='s'){
        snprintf(cmd, sizeof(cmd),"%s scale=%dx%d %s;",in,x,y,out);
    }
    else if(filter[0]=='o'){
        if(flag == 1)  snprintf(cmd, sizeof(cmd),"%s%s overlay=x=%d:y=%d",in,in_second,x,y);
        else snprintf(cmd, sizeof(cmd),"%s%s overlay=x=%d:y=%d %s;",in,in_second,x,y,out);
    }
    strcpy(filter,cmd);
    return 0;
}

static char* vr_set_parameters(VRContext *vr, const char *graph_str){
    char backboard[100000],operation[100000],operation_total[100000];
    char *key, *value;
    char chr = 0;
    char filter[64], url[4096], in[64], in_second[64], out[64];
    char movie_out[64], scale_out[64], overlay_out[64];
    int flag = 0;

    graph_str += strspn(graph_str, WHITESPACES);
    do{
        key = av_get_token(&graph_str,"=");
        graph_str++;
        value = av_get_token(&graph_str, ";");
        chr = *graph_str++;
        av_opt_set(vr, key, value, 0);
        graph_str += strspn(graph_str, WHITESPACES);
    }while(chr==';');
    av_free(key);
    av_free(value);
    
    strcpy(filter, "nullsrc");
    int width  = vr->width  * vr->col;
    int height = vr->height * vr->row;
    strcpy(out,"[backboard]");
    set_opts(filter, width, height, url, in, in_second, out, flag);
    snprintf(backboard, sizeof(backboard),"%s %s",backboard, filter);
    
    int nb_tiles = vr->row * vr->col;
    int row, col;
    printf("%s\n", vr->url);
    for(int i = 0 ; i< nb_tiles ; i++){
        if(i==nb_tiles-1) flag = 1;
        else            flag = 0;
        strcpy(url, av_get_token(&vr->url, ","));
        vr->url++;
        row = i/vr->col;
        col = i%vr->col;
    
        snprintf(movie_out, sizeof(movie_out), "[v%d]",i);
        snprintf(scale_out, sizeof(scale_out), "[s%d]",i);
        snprintf(overlay_out, sizeof(overlay_out), "[o%d]",i);

        strcpy(filter, "vrmovie");
        set_opts(filter, width, height, url, in, in_second, movie_out, flag);
        snprintf(operation, sizeof(operation),"%s%s",operation, filter);

        strcpy(filter, "scale");
        set_opts(filter, vr->width, vr->height, url, movie_out, in_second, scale_out, flag);
        snprintf(operation, sizeof(operation),"%s%s",operation, filter);

        strcpy(filter, "overlay");
        set_opts(filter, col * vr->width, row * vr->height, url, out, scale_out, overlay_out, flag);
        snprintf(operation, sizeof(operation),"%s%s",operation, filter);
        strcpy(out, overlay_out);
        snprintf(operation_total, sizeof(operation_total),"%s%s",operation_total, operation);
    }
    snprintf(backboard, sizeof(backboard),"%s%s",backboard, operation_total);
    return backboard;
}
av_cold static int vr_read_header(AVFormatContext *avctx)
{
    VRContext *vr = avctx->priv_data;
    AVFilterInOut *input_links = NULL, *output_links = NULL, *inout;
    const AVFilter *buffersink, *abuffersink;
    int *pix_fmts = create_all_formats(AV_PIX_FMT_NB);
    enum AVMediaType type;
    int ret = 0, i, n;

#define FAIL(ERR) { ret = ERR; goto end; }

    if (!pix_fmts)
        FAIL(AVERROR(ENOMEM));

    buffersink = avfilter_get_by_name("buffersink");
    abuffersink = avfilter_get_by_name("abuffersink");

    if (vr->graph_filename && vr->graph_str) {
        av_log(avctx, AV_LOG_ERROR,
               "Only one of the graph or graph_file options must be specified\n");
        FAIL(AVERROR(EINVAL));
    }

    if (vr->graph_filename) {
        AVBPrint graph_file_pb;
        AVIOContext *avio = NULL;
        AVDictionary *options = NULL;
        if (avctx->protocol_whitelist && (ret = av_dict_set(&options, "protocol_whitelist", avctx->protocol_whitelist, 0)) < 0)
            goto end;
        ret = avio_open2(&avio, vr->graph_filename, AVIO_FLAG_READ, &avctx->interrupt_callback, &options);
        av_dict_set(&options, "protocol_whitelist", NULL, 0);
        if (ret < 0)
            goto end;
        av_bprint_init(&graph_file_pb, 0, AV_BPRINT_SIZE_UNLIMITED);
        ret = avio_read_to_bprint(avio, &graph_file_pb, INT_MAX);
        avio_closep(&avio);
        av_bprint_chars(&graph_file_pb, '\0', 1);
        if (!ret && !av_bprint_is_complete(&graph_file_pb))
            ret = AVERROR(ENOMEM);
        if (ret) {
            av_bprint_finalize(&graph_file_pb, NULL);
            goto end;
        }
        if ((ret = av_bprint_finalize(&graph_file_pb, &vr->graph_str)))
            goto end;
    }

    if (!vr->graph_str)
        vr->graph_str = av_strdup(avctx->url);
    
    vr->graph_str = av_strdup(vr_set_parameters(vr, vr->graph_str));
    /* parse the graph, create a stream for each open output */
    if (!(vr->graph = avfilter_graph_alloc()))  //初始化graph
        FAIL(AVERROR(ENOMEM));

    if ((ret = avfilter_graph_parse_ptr(vr->graph, vr->graph_str,
                                        &input_links, &output_links, avctx)) < 0)  //这两个LINK的类型是AVFilterInOut
        goto end;

    if (input_links) {
        av_log(avctx, AV_LOG_ERROR,
               "Open inputs in the filtergraph are not acceptable\n");
        FAIL(AVERROR(EINVAL));
    }

    /* count the outputs */
    for (n = 0, inout = output_links; inout; n++, inout = inout->next);
    vr->nb_sinks = n;

    if (!(vr->sink_stream_map = av_malloc(sizeof(int) * n)))
        FAIL(AVERROR(ENOMEM));
    if (!(vr->sink_eof = av_mallocz(sizeof(int) * n)))
        FAIL(AVERROR(ENOMEM));
    if (!(vr->stream_sink_map = av_malloc(sizeof(int) * n)))
        FAIL(AVERROR(ENOMEM));
    if (!(vr->sink_stream_subcc_map = av_malloc(sizeof(int) * n)))
        FAIL(AVERROR(ENOMEM));

    for (i = 0; i < n; i++)
        vr->stream_sink_map[i] = -1;

    /* parse the output link names - they need to be of the form out0, out1, ...
     * create a mapping between them and the streams */
    for (i = 0, inout = output_links; inout; i++, inout = inout->next) {
        int stream_idx = 0, suffix = 0, use_subcc = 0;
        sscanf(inout->name, "out%n%d%n", &suffix, &stream_idx, &suffix);
        if (!suffix) {
            av_log(avctx,  AV_LOG_ERROR,
                   "Invalid outpad name '%s'\n", inout->name);
            FAIL(AVERROR(EINVAL));
        }
        if (inout->name[suffix]) {
            if (!strcmp(inout->name + suffix, "+subcc")) {
                use_subcc = 1;
            } else {
                av_log(avctx,  AV_LOG_ERROR,
                       "Invalid outpad suffix '%s'\n", inout->name);
                FAIL(AVERROR(EINVAL));
            }
        }

        if ((unsigned)stream_idx >= n) {
            av_log(avctx, AV_LOG_ERROR,
                   "Invalid index was specified in output '%s', "
                   "must be a non-negative value < %d\n",
                   inout->name, n);
            FAIL(AVERROR(EINVAL));
        }

        if (vr->stream_sink_map[stream_idx] != -1) {
            av_log(avctx,  AV_LOG_ERROR,
                   "An output with stream index %d was already specified\n",
                   stream_idx);
            FAIL(AVERROR(EINVAL));
        }
        vr->sink_stream_map[i] = stream_idx;
        vr->stream_sink_map[stream_idx] = i;
        vr->sink_stream_subcc_map[i] = !!use_subcc;
    }

    /* for each open output create a corresponding stream */
    for (i = 0, inout = output_links; inout; i++, inout = inout->next) {
        AVStream *st;
        if (!(st = avformat_new_stream(avctx, NULL)))  //在这里根据output创建了一个新流（虽然是都是默认值）
            FAIL(AVERROR(ENOMEM));
        st->id = i;
    }

    /* create a sink for each output and connect them to the graph */
    vr->sinks = av_malloc_array(vr->nb_sinks, sizeof(AVFilterContext *));
    if (!vr->sinks)
        FAIL(AVERROR(ENOMEM));

    for (i = 0, inout = output_links; inout; i++, inout = inout->next) {  //好几个循环的条件都是inout=inout->next 因为只有一个output_link 所以for循环只走一次
        AVFilterContext *sink;

        type = avfilter_pad_get_type(inout->filter_ctx->output_pads, inout->pad_idx);

        if (type == AVMEDIA_TYPE_VIDEO && ! buffersink ||
            type == AVMEDIA_TYPE_AUDIO && ! abuffersink) {
            av_log(avctx, AV_LOG_ERROR, "Missing required buffersink filter, aborting.\n");
            FAIL(AVERROR_FILTER_NOT_FOUND);
        }

        if (type == AVMEDIA_TYPE_VIDEO) {
            ret = avfilter_graph_create_filter(&sink, buffersink,
                                               inout->name, NULL,
                                               NULL, vr->graph);
            if (ret >= 0)
                ret = av_opt_set_int_list(sink, "pix_fmts", pix_fmts,  AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
            if (ret < 0)
                goto end;
        } else if (type == AVMEDIA_TYPE_AUDIO) {
            enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_U8,
                AV_SAMPLE_FMT_S16,
                AV_SAMPLE_FMT_S32,
                AV_SAMPLE_FMT_FLT,
                AV_SAMPLE_FMT_DBL, -1 };

            ret = avfilter_graph_create_filter(&sink, abuffersink,
                                               inout->name, NULL,
                                               NULL, vr->graph);
            if (ret >= 0)
                ret = av_opt_set_int_list(sink, "sample_fmts", sample_fmts,  AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
            if (ret < 0)
                goto end;
            ret = av_opt_set_int(sink, "all_channel_counts", 1,
                                 AV_OPT_SEARCH_CHILDREN);
            if (ret < 0)
                goto end;
        } else {
            av_log(avctx,  AV_LOG_ERROR,
                   "Output '%s' is not a video or audio output, not yet supported\n", inout->name);
            FAIL(AVERROR(EINVAL));
        }

        vr->sinks[i] = sink;
        if ((ret = avfilter_link(inout->filter_ctx, inout->pad_idx, sink, 0)) < 0)
            goto end;
    }

    /* configure the graph */
    if ((ret = avfilter_graph_config(vr->graph, avctx)) < 0)  //做检查，检查无误就证明图已经连好了
        goto end;

    if (vr->dump_graph) {
        char *dump = avfilter_graph_dump(vr->graph, vr->dump_graph);
        fputs(dump, stderr);
        fflush(stderr);
        av_free(dump);
    }

    /* fill each stream with the information in the corresponding sink */
    for (i = 0; i < vr->nb_sinks; i++) {   //只有一个sink
        AVFilterContext *sink = vr->sinks[vr->stream_sink_map[i]];
        AVRational time_base = av_buffersink_get_time_base(sink);
        AVStream *st = avctx->streams[i];
        st->codecpar->codec_type = av_buffersink_get_type(sink);    //AVMEDIA_TYPE_VIDEO
        avpriv_set_pts_info(st, 64, time_base.num, time_base.den);
        if (av_buffersink_get_type(sink) == AVMEDIA_TYPE_VIDEO) {
            st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;   //在这里确定的codec_id什么的
            st->codecpar->format     = av_buffersink_get_format(sink);
            st->codecpar->width      = av_buffersink_get_w(sink);
            st->codecpar->height     = av_buffersink_get_h(sink);
            st       ->sample_aspect_ratio =
            st->codecpar->sample_aspect_ratio = av_buffersink_get_sample_aspect_ratio(sink);
            avctx->probesize = FFMAX(avctx->probesize,
                                     av_buffersink_get_w(sink) * av_buffersink_get_h(sink) *
                                     av_get_padded_bits_per_pixel(av_pix_fmt_desc_get(av_buffersink_get_format(sink))) *
                                     30);
        } else if (av_buffersink_get_type(sink) == AVMEDIA_TYPE_AUDIO) {
            st->codecpar->codec_id    = av_get_pcm_codec(av_buffersink_get_format(sink), -1);
            st->codecpar->channels    = av_buffersink_get_channels(sink);
            st->codecpar->format      = av_buffersink_get_format(sink);
            st->codecpar->sample_rate = av_buffersink_get_sample_rate(sink);
            st->codecpar->channel_layout = av_buffersink_get_channel_layout(sink);
            if (st->codecpar->codec_id == AV_CODEC_ID_NONE)
                av_log(avctx, AV_LOG_ERROR,
                       "Could not find PCM codec for sample format %s.\n",
                       av_get_sample_fmt_name(av_buffersink_get_format(sink)));
        }
    }

    if ((ret = create_subcc_streams(avctx)) < 0)
        goto end;

    if (!(vr->decoded_frame = av_frame_alloc()))
        FAIL(AVERROR(ENOMEM));

end:
    av_free(pix_fmts);
    avfilter_inout_free(&input_links);
    avfilter_inout_free(&output_links);
    if (ret < 0)
        vr_read_close(avctx);
    return ret;
}

static int create_subcc_packet(AVFormatContext *avctx, AVFrame *frame,
                               int sink_idx)
{
    VRContext *vr = avctx->priv_data;
    AVFrameSideData *sd;
    int stream_idx, i, ret;

    if ((stream_idx = vr->sink_stream_subcc_map[sink_idx]) < 0)
        return 0;
    for (i = 0; i < frame->nb_side_data; i++)
        if (frame->side_data[i]->type == AV_FRAME_DATA_A53_CC)
            break;
    if (i >= frame->nb_side_data)
        return 0;
    sd = frame->side_data[i];
    if ((ret = av_new_packet(&vr->subcc_packet, sd->size)) < 0)
        return ret;
    memcpy(vr->subcc_packet.data, sd->data, sd->size);
    vr->subcc_packet.stream_index = stream_idx;
    vr->subcc_packet.pts = frame->pts;
    vr->subcc_packet.pos = frame->pkt_pos;
    return 0;
}
static int cal_orientation(VRContext *vr){
    double mouse_x = vr->x, mouse_y = vr->y; //鼠标坐标
    double alpha = mouse_x * 2.0 * M_PI;
    double beta =  mouse_y * M_PI;
    double sp_x =  sin(beta) * cos(alpha);
    double sp_y =  - sin(beta) * sin(alpha);
    double sp_z =  cos(beta);
    double qua_theta = 0.5 * acos(-sp_x) ; //(-1,0,0)与(sp_x,sp_y,sp_z)做点积运算
    double qua_w =   cos(qua_theta);
    double qua_x =   0;             //(-1,0,0)与(sp_x,sp_y,sp_z)做叉积运算
    double qua_y =   sp_z;
    double qua_z = - sp_y;
    double total = sqrt(qua_x*qua_x+qua_y*qua_y+qua_z*qua_z);
    qua_x = qua_x * sin(qua_theta) / total;
    qua_y = qua_y * sin(qua_theta) / total;
    qua_z = qua_z * sin(qua_theta) / total;
    
    double RotationMatrix[3][3];
    
    RotationMatrix[0][0] = 1.0 - 2.0 * qua_y * qua_y - 2.0 * qua_z * qua_z;
    RotationMatrix[0][1] =       2.0 * qua_x * qua_y - 2.0 * qua_w * qua_z;
    RotationMatrix[0][2] =       2.0 * qua_x * qua_z + 2.0 * qua_w * qua_y;
    
    RotationMatrix[1][0] =       2.0 * qua_x * qua_y + 2.0 * qua_w * qua_z;
    RotationMatrix[1][1] = 1.0 - 2.0 * qua_x * qua_x - 2.0 * qua_z * qua_z;
    RotationMatrix[1][2] =       2.0 * qua_x * qua_y - 2.0 * qua_w * qua_x;
    
    RotationMatrix[2][0] =       2.0 * qua_x * qua_z - 2.0 * qua_w * qua_y;
    RotationMatrix[2][1] =       2.0 * qua_y * qua_z + 2.0 * qua_w * qua_x;
    RotationMatrix[2][2] = 1.0 - 2.0 * qua_x * qua_x - 2.0 * qua_y * qua_y;
    
    double hmd_x, hmd_y, hmd_z;
    double fin_x, fin_y, fin_z;
    double rect_x, rect_y;
    int img_w = vr->width * vr->col, img_h = vr->height * vr->row;
    int FOVWidth = img_w / 4, FOVHeight = img_w / 4;
    int tile_w = vr->width, tile_h = vr->height;
    int row = vr->row, col = vr->col;
    int TileProb[72] = {0};
    for( int i = - FOVHeight/2 ; i <= FOVHeight/2 ; i++){
        for( int j = - FOVWidth/2 ; j <= FOVWidth/2 ; j++ ){
            hmd_x = -1.0;
            hmd_y = 2.0 * j / FOVWidth;
            hmd_z = 2.0 * i / FOVHeight;
            
            fin_x = RotationMatrix[0][0] * hmd_x + RotationMatrix[0][1] * hmd_y + RotationMatrix[0][2] * hmd_z;
            fin_y = RotationMatrix[1][0] * hmd_x + RotationMatrix[1][1] * hmd_y + RotationMatrix[1][2] * hmd_z;
            fin_z = RotationMatrix[2][0] * hmd_x + RotationMatrix[2][1] * hmd_y + RotationMatrix[2][2] * hmd_z;
            
            total = sqrt(fin_x*fin_x+fin_y*fin_y+fin_z*fin_z);
            fin_x = fin_x  / total;
            fin_y = fin_y  / total;
            fin_z = fin_z  / total;

            double beta, alpha;
            if(1.0 - fabs(fin_z) < 0.00001){
                if(fin_z > 0) beta = 0;
                else          beta = M_PI;
                alpha = 0;
            }
            else{
                beta = acos(fin_z);
                alpha = asin( - fin_y / sin(beta));  //-pi/2 + pi/2
                if(fin_y < 0 && fin_x > 0)
                    alpha = alpha;
                else if(fin_x < 0)
                    alpha = M_PI - alpha;
                else
                    alpha = 2.0 * M_PI + alpha;
            }
            rect_x = img_w * alpha / (2.0 * M_PI);
            rect_y = img_h * beta / M_PI;

            int x = floor(rect_x), y = floor(rect_y);
            if (x >= 0 && x < img_w && y >= 0 && y < img_h)
                TileProb[(x      / tile_w)+ 1 + (y / tile_h) * col]++;
            if (x + 1 >= 0 && x + 1 < img_h && y >= 0 && y < img_w)
                TileProb[(x + 1) / tile_w + 1 + (y / tile_h) * col]++;
            if (x >= 0 && x < img_h && y + 1 >= 0 && y + 1 < img_w)
                TileProb[(x      / tile_w)+ 1 + ((y + 1) / tile_h) * col]++;
            if (x + 1 >= 0 && x + 1 < img_h && y + 1 >= 0 && y + 1 < img_w)
                TileProb[(x + 1) / tile_w + 1 + ((y + 1) / tile_h) * col]++;
            
        }
    }
    char ori[1024];
    for (int i = 1 ; i <= row * col ; i++){
        if(TileProb[i]>0)   snprintf(ori, sizeof(ori), "%s", "1");
        else                snprintf(ori, sizeof(ori), "%s", "0");
    }
    av_opt_set(vr, "ori", ori, 0);
    return 1;
}
static int vr_read_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    VRContext *vr = avctx->priv_data;
    double min_pts = DBL_MAX;
    int stream_idx, min_pts_sink_idx = 0;
    AVFrame *frame = vr->decoded_frame;
    AVDictionary *frame_metadata;
    int ret, i;
    int size = 0;

    int transTime = 3 * AV_TIME_BASE; //byx-add

    if (vr->subcc_packet.size) {
        *pkt = vr->subcc_packet;
        av_init_packet(&vr->subcc_packet);
        vr->subcc_packet.size = 0;
        vr->subcc_packet.data = NULL;
        return pkt->size;
    }

    /* iterate through all the graph sinks. Select the sink with the
     * minimum PTS */
    for (i = 0; i < vr->nb_sinks; i++) {
        AVRational tb = av_buffersink_get_time_base(vr->sinks[i]);
        double d;
        int ret;

        if (vr->sink_eof[i])
            continue;

        ret = av_buffersink_get_frame_flags(vr->sinks[i], frame,
                                            AV_BUFFERSINK_FLAG_PEEK);
        if (ret == AVERROR_EOF) {
            ff_dlog(avctx, "EOF sink_idx:%d\n", i);
            vr->sink_eof[i] = 1;
            continue;
        } else if (ret < 0)
            return ret;
        d = av_rescale_q_rnd(frame->pts, tb, AV_TIME_BASE_Q, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);  //重要！这个d就是真实播放的时间（微秒），他需要除以AV_TIME_BASE，才能得到真实的时间（秒）。

        ff_dlog(avctx, "sink_idx:%d time:%f\n", i, d);

        av_log(NULL, AV_LOG_DEBUG, "sink_idx:%d time:%f\n", i, d);  //byx-add

        av_frame_unref(frame);

        if (d < min_pts) {
            min_pts = d;
            min_pts_sink_idx = i;
        }
    }
    if (min_pts == DBL_MAX)
        return AVERROR_EOF;

    ff_dlog(avctx, "min_pts_sink_idx:%i\n", min_pts_sink_idx);

    av_log(NULL, AV_LOG_DEBUG, "min_pts_sink_idx:%i \n", min_pts_sink_idx);  //byx-add
    
    cal_orientation(vr);

    static int time_zhd = 0;
    int rate_zhd = -1;
    time_zhd = min_pts / AV_TIME_BASE;
    int time_step = 2;
    int time_step2 = 3;
    int stream_nb = 5;
    if (time_zhd > 2) {
        av_opt_set_int(vr->graph->filters[1]->priv, "rate_zhd", time_zhd/time_step%stream_nb, 0);
        av_opt_set_int(vr->graph->filters[2]->priv, "rate_zhd", (stream_nb - time_zhd/time_step%stream_nb)%stream_nb, 0);
        av_opt_set_int(vr->graph->filters[3]->priv, "rate_zhd", time_zhd/time_step2%stream_nb, 0);

    }
    //zhd-add------------------------------------

    int nb_tiles = vr->col * vr->row;
    for(int i = 0 ; i< nb_tiles ; i++){
        if(vr->ori[i] == '1'){
            av_opt_set_int(vr->graph->filters[i]->priv, "rate_zhd", 1, 0);
        }
        else{
            av_opt_set_int(vr->graph->filters[i]->priv, "rate_zhd", 0, 0);
        }
    }  //wyx-add

    av_buffersink_get_frame_flags(vr->sinks[min_pts_sink_idx], frame, 0);
    stream_idx = vr->sink_stream_map[min_pts_sink_idx];

    if (frame->width /* FIXME best way of testing a video */) {
        size = av_image_get_buffer_size(frame->format, frame->width, frame->height, 1);
        if ((ret = av_new_packet(pkt, size)) < 0)
            return ret;

        av_image_copy_to_buffer(pkt->data, size, (const uint8_t **)frame->data, frame->linesize,
                                frame->format, frame->width, frame->height, 1);
    } else if (frame->channels /* FIXME test audio */) {
        size = frame->nb_samples * av_get_bytes_per_sample(frame->format) *
        frame->channels;
        if ((ret = av_new_packet(pkt, size)) < 0)
            return ret;
        memcpy(pkt->data, frame->data[0], size);
    }

    frame_metadata = frame->metadata;
    if (frame_metadata) {
        uint8_t *metadata;
        AVDictionaryEntry *e = NULL;
        AVBPrint meta_buf;

        av_bprint_init(&meta_buf, 0, AV_BPRINT_SIZE_UNLIMITED);
        while ((e = av_dict_get(frame_metadata, "", e, AV_DICT_IGNORE_SUFFIX))) {
            av_bprintf(&meta_buf, "%s", e->key);
            av_bprint_chars(&meta_buf, '\0', 1);
            av_bprintf(&meta_buf, "%s", e->value);
            av_bprint_chars(&meta_buf, '\0', 1);
        }
        if (!av_bprint_is_complete(&meta_buf) ||
            !(metadata = av_packet_new_side_data(pkt, AV_PKT_DATA_STRINGS_METADATA,
                                                 meta_buf.len))) {
            av_bprint_finalize(&meta_buf, NULL);
            return AVERROR(ENOMEM);
        }
        memcpy(metadata, meta_buf.str, meta_buf.len);
        av_bprint_finalize(&meta_buf, NULL);
    }

    if ((ret = create_subcc_packet(avctx, frame, min_pts_sink_idx)) < 0) {
        av_frame_unref(frame);
        av_packet_unref(pkt);
        return ret;
    }

    pkt->stream_index = stream_idx;
    av_log(NULL , AV_LOG_DEBUG, "vr:pkt->stream-idx = %d", pkt->stream_index);
    pkt->pts = frame->pts;
    pkt->pos = frame->pkt_pos;
    pkt->size = size;
    av_frame_unref(frame);
    return size;
}

#define OFFSET(x) offsetof(VRContext, x)

#define DEC AV_OPT_FLAG_DECODING_PARAM

static const AVOption options[] = {
    { "graph",     "set libavfilter graph", OFFSET(graph_str),  AV_OPT_TYPE_STRING,      {.str = NULL}, 0, 0, DEC },
    { "graph_file","set libavfilter graph filename", OFFSET(graph_filename), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, DEC},
    { "dumpgraph", "dump graph to stderr",  OFFSET(dump_graph), AV_OPT_TYPE_STRING,      {.str = NULL}, 0, 0, DEC },
    
    { "x",         "set the x expression",  OFFSET(x),          AV_OPT_TYPE_DOUBLE,      {.dbl =  0 },  0, 1, DEC },
    { "y",         "set the y expression",  OFFSET(y),          AV_OPT_TYPE_DOUBLE,      {.dbl =  0 },  0, 1, DEC },
    { "size",      "set tile size",         OFFSET(width),      AV_OPT_TYPE_IMAGE_SIZE,  {.str = NULL}, 0, 0, DEC },
    { "layout",    "set tile layout",       OFFSET(col),        AV_OPT_TYPE_IMAGE_SIZE,  {.str = NULL}, 0, 0, DEC },
    { "url",       "set urls",              OFFSET(url),        AV_OPT_TYPE_STRING,      {.str = NULL}, 0, 0, DEC },
    { "ori",       "set orientation",       OFFSET(ori),        AV_OPT_TYPE_STRING,      {.str = NULL}, 0, 0, DEC },  //byx-add

    { NULL },
};

static const AVClass vr_class = {
    .class_name = "vr indev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_INPUT,
};

AVInputFormat ff_vr_demuxer = {
    .name           = "vr",
    .long_name      = NULL_IF_CONFIG_SMALL("vr input device"),
    .priv_data_size = sizeof(VRContext),
    .read_header    = vr_read_header,
    .read_packet    = vr_read_packet,
    .read_close     = vr_read_close,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &vr_class,
};

//#include <stdio.h>
//#include <stdlib.h>
//#include <limits.h>
//#include <string>
//#include <string.h>
//#include <assert.h>
//#include <queue>
//#include <vector>
//#include <math.h>
//#include <algorithm>
//using namespace std;
//void normolize(double &x, double &y, double &z){
//    double total = sqrt(x*x+y*y+z*z);
//    x /= total;
//    y /= total;
//    z /= total;
//}
//void sph2rect(double &fin_x, double &fin_y, double &fin_z, double &rect_x, double &rect_y, int &w, int &h){
//    double beta, alpha;
//    if(abs(fin_y) - 1.0 < 0.00001){
//        if(fin_y > 0) beta = 0;
//        else          beta = M_PI;
//        alpha = 0;
//    }
//    else{
//        beta = acos(fin_y);
//        alpha = asin( - fin_x / sin(beta));  //-pi/2 + pi/2
//        if(fin_x < 0 && fin_z < 0){
//            alpha = alpha;
//        }
//        else if(fin_z > 0){
//            alpha = M_PI - alpha;
//        }
//        else{
//            alpha = M_2_PI + alpha;
//        }
//    }
//    rect_x = w * alpha / M_2_PI;
//    rect_y = h * beta / M_PI;
//}
//
//int main(){
//    double mouse_x, mouse_y; //鼠标坐标
//    scanf("%lf%lf", &mouse_x, &mouse_y);
//    double alpha = mouse_x * 2.0 * M_PI;
//    double beta =  mouse_y * M_PI;
//    double sp_x = - sin(beta) * sin(alpha);
//    double sp_y =   cos(beta);
//    double sp_z = - sin(beta) * cos(alpha);
//    double qua_theta = 0.5 * acos(sp_z) ; //(0,0,1)与(sp_x,sp_y,sp_z)做点积运算
//    double qua_w = cos(qua_theta);
//    double qua_x = - sp_y;             //(0,0,1)与(sp_x,sp_y,sp_z)做叉积运算
//    double qua_y =   sp_x;
//    double qua_z = 0;
//    normolize(qua_x, qua_y, qua_z);
//    double RotationMatrix[3][3];
//
//    RotationMatrix[0][0] = 1.0 - 2.0 * qua_y * qua_y - 2.0 * qua_z * qua_z;
//    RotationMatrix[0][1] =       2.0 * qua_x * qua_y - 2.0 * qua_w * qua_z;
//    RotationMatrix[0][2] =       2.0 * qua_x * qua_z + 2.0 * qua_w * qua_y;
//
//    RotationMatrix[1][0] =       2.0 * qua_x * qua_y + 2.0 * qua_w * qua_z;
//    RotationMatrix[1][1] = 1.0 - 2.0 * qua_x * qua_x - 2.0 * qua_w * qua_w;
//    RotationMatrix[1][2] =       2.0 * qua_x * qua_y - 2.0 * qua_w * qua_x;
//
//    RotationMatrix[2][0] =       2.0 * qua_x * qua_z - 2.0 * qua_w * qua_y;
//    RotationMatrix[2][1] =       2.0 * qua_y * qua_z + 2.0 * qua_w * qua_x;
//    RotationMatrix[2][2] = 1.0 - 2.0 * qua_x * qua_x - 2.0 * qua_y * qua_y;
//
//    double hmd_x, hmd_y, hmd_z;
//    double fin_x, fin_y, fin_z;
//    double rect_x, rect_y;
//    int img_w = 2880, img_h = 1440;
//    int FOVWidth = 720, FOVHeight = 720;
//    int tile_w = 720, tile_h = 480;
//    int row = 3, col = 4;
//    int TileProb[72] = {0};
//    for( int i = - FOVHeight/2 ; i <= FOVHeight/2 ; i++){
//        for( int j = - FOVWidth/2 ; j <= FOVWidth/2 ; j++ ){
//            hmd_x = 2.0 * j / FOVWidth;
//            hmd_y = 2.0 * i / FOVHeight;
//            hmd_z = 1.0;
//            fin_x = RotationMatrix[0][0] * hmd_x + RotationMatrix[0][1] * hmd_y + RotationMatrix[0][2] * hmd_z; //z
//            fin_y = RotationMatrix[1][0] * hmd_x + RotationMatrix[1][1] * hmd_y + RotationMatrix[1][2] * hmd_z; //x
//            fin_z = RotationMatrix[2][0] * hmd_x + RotationMatrix[2][1] * hmd_y + RotationMatrix[2][2] * hmd_z; //y
//            normolize(fin_x, fin_y, fin_z);
//            sph2rect(fin_x, fin_y, fin_z, rect_x, rect_y, img_w, img_h);
//            int x = floor(rect_y), y = floor(rect_x);
//
//            if (x >= 0 && x < img_h && y >= 0 && y < img_w)                   TileProb[(x      / tile_w)+ 1 + row * y       / tile_h]++;
//            if (x + 1 >= 0 && x + 1 < img_h && y >= 0 && y < img_w)           TileProb[(x + 1) / tile_w + 1 + row * y       / tile_h]++;
//            if (x >= 0 && x < img_h && y + 1 >= 0 && y + 1 < img_w)           TileProb[(x      / tile_w)+ 1 + row * (y + 1) / tile_h]++;
//            if (x + 1 >= 0 && x + 1 < img_h && y + 1 >= 0 && y + 1 < img_w)   TileProb[(x + 1) / tile_w     + row * (y + 1) / tile_h]++;
//
//        }
//    }
//    for (int i = 1 ; i <= row * col ; i++){
//        if(TileProb[i]>0)   printf("1");
//        else                printf("0");
//    }
//    printf("\n");
//}


