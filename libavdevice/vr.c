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

    //zhd-add------------------------------------
    static int time_zhd = 0;
    int rate_zhd = -1;
    //    av_log(NULL, AV_LOG_INFO, "time: %.2lf\n", min_pts / AV_TIME_BASE);
    time_zhd = min_pts / AV_TIME_BASE;
    int time_step = 2;
    int time_step2 = 3;
    int stream_nb = 5;
    if (time_zhd > 2) {
        //        av_opt_get_int(lavfi->graph->filters[1]->priv, "rate_zhd", 0, &rate_zhd);
        //        av_log(NULL, AV_LOG_INFO, "lavfi: %d -> %d\n", rate_zhd, time_zhd/time_step%stream_nb);

        av_opt_set_int(vr->graph->filters[1]->priv, "rate_zhd", time_zhd/time_step%stream_nb, 0);
        av_opt_set_int(vr->graph->filters[2]->priv, "rate_zhd", (stream_nb - time_zhd/time_step%stream_nb)%stream_nb, 0);
        av_opt_set_int(vr->graph->filters[3]->priv, "rate_zhd", time_zhd/time_step2%stream_nb, 0);

        //        av_opt_set_double(lavfi->graph->filters[0]->priv, "d", min_pts / AV_TIME_BASE, 0);
    }
    //zhd-add------------------------------------



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

//    { "format_name",  "set format name",         OFFSET(format_name),  AV_OPT_TYPE_STRING,                                    .flags = FLAGS },
//    { "f",            "set format name",         OFFSET(format_name),  AV_OPT_TYPE_STRING,                                    .flags = FLAGS },
//    { "stream_index", "set stream index",        OFFSET(stream_index), AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, INT_MAX,                 FLAGS  },
//    { "si",           "set stream index",        OFFSET(stream_index), AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, INT_MAX,                 FLAGS  },
//    { "seek_point",   "set seekpoint (seconds)", OFFSET(seek_point_d), AV_OPT_TYPE_DOUBLE, { .dbl =  0 },  0, (INT64_MAX-1) / 1000000, FLAGS },
//    { "sp",           "set seekpoint (seconds)", OFFSET(seek_point_d), AV_OPT_TYPE_DOUBLE, { .dbl =  0 },  0, (INT64_MAX-1) / 1000000, FLAGS },
//    { "streams",      "set streams",             OFFSET(stream_specs), AV_OPT_TYPE_STRING, {.str =  0},  CHAR_MAX, CHAR_MAX, FLAGS },
//    { "s",            "set streams",             OFFSET(stream_specs), AV_OPT_TYPE_STRING, {.str =  0},  CHAR_MAX, CHAR_MAX, FLAGS },
//    { "loop",         "set loop count",          OFFSET(loop_count),   AV_OPT_TYPE_INT,    {.i64 =  1},  0,        INT_MAX, FLAGS },
//    { "discontinuity", "set discontinuity threshold", OFFSET(discontinuity_threshold), AV_OPT_TYPE_DURATION, {.i64 = 0}, 0, INT64_MAX, FLAGS },
//    { "orientation",  "set orientation",         OFFSET(orientation),  AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MAX, CHAR_MAX, FLAGS},  //byx-add
//    { "orientation_last",  "set if orientation is changed",         OFFSET(orientation_last),  AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MAX, CHAR_MAX, FLAGS},  //byx-add
//    { "rate_zhd",     "set rate_zhd",            OFFSET(rate_zhd),     AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, INT_MAX,                 FLAGS  }, // zhd add
//    { "x", "set the x expression", OFFSET(x_expr), AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MIN, CHAR_MAX, FLAGS },
//    { "y", "set the y expression", OFFSET(y_expr), AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MIN, CHAR_MAX, FLAGS },
//    { "eof_action", "Action to take when encountering EOF from secondary input ",
//        OFFSET(fs.opt_eof_action), AV_OPT_TYPE_INT, { .i64 = EOF_ACTION_REPEAT },
//        EOF_ACTION_REPEAT, EOF_ACTION_PASS, .flags = FLAGS, "eof_action" },
//    { "repeat", "Repeat the previous frame.",   0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_REPEAT }, .flags = FLAGS, "eof_action" },
//    { "endall", "End both streams.",            0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_ENDALL }, .flags = FLAGS, "eof_action" },
//    { "pass",   "Pass through the main input.", 0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_PASS },   .flags = FLAGS, "eof_action" },
//    { "eval", "specify when to evaluate expressions", OFFSET(eval_mode), AV_OPT_TYPE_INT, {.i64 = EVAL_MODE_FRAME}, 0, EVAL_MODE_NB-1, FLAGS, "eval" },
//    { "init",  "eval expressions once during initialization", 0, AV_OPT_TYPE_CONST, {.i64=EVAL_MODE_INIT},  .flags = FLAGS, .unit = "eval" },
//    { "frame", "eval expressions per-frame",                  0, AV_OPT_TYPE_CONST, {.i64=EVAL_MODE_FRAME}, .flags = FLAGS, .unit = "eval" },
//    { "shortest", "force termination when the shortest input terminates", OFFSET(fs.opt_shortest), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
//    { "format", "set output format", OFFSET(format), AV_OPT_TYPE_INT, {.i64=OVERLAY_FORMAT_YUV420}, 0, OVERLAY_FORMAT_NB-1, FLAGS, "format" },
//    { "yuv420", "", 0, AV_OPT_TYPE_CONST, {.i64=OVERLAY_FORMAT_YUV420}, .flags = FLAGS, .unit = "format" },
//    { "yuv422", "", 0, AV_OPT_TYPE_CONST, {.i64=OVERLAY_FORMAT_YUV422}, .flags = FLAGS, .unit = "format" },
//    { "yuv444", "", 0, AV_OPT_TYPE_CONST, {.i64=OVERLAY_FORMAT_YUV444}, .flags = FLAGS, .unit = "format" },
//    { "rgb",    "", 0, AV_OPT_TYPE_CONST, {.i64=OVERLAY_FORMAT_RGB},    .flags = FLAGS, .unit = "format" },
//    { "gbrp",   "", 0, AV_OPT_TYPE_CONST, {.i64=OVERLAY_FORMAT_GBRP},   .flags = FLAGS, .unit = "format" },
//    { "auto",   "", 0, AV_OPT_TYPE_CONST, {.i64=OVERLAY_FORMAT_AUTO},   .flags = FLAGS, .unit = "format" },
//    { "repeatlast", "repeat overlay of the last overlay frame", OFFSET(fs.opt_repeatlast), AV_OPT_TYPE_BOOL, {.i64=1}, 0, 1, FLAGS },
//    { "alpha", "alpha format", OFFSET(alpha_format), AV_OPT_TYPE_INT, {.i64=0}, 0, 1, FLAGS, "alpha_format" },
//    { "straight",      "", 0, AV_OPT_TYPE_CONST, {.i64=0}, .flags = FLAGS, .unit = "alpha_format" },
//    { "premultiplied", "", 0, AV_OPT_TYPE_CONST, {.i64=1}, .flags = FLAGS, .unit = "alpha_format" },
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

