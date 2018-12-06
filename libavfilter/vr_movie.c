/*
 * Copyright (c) 2010 Stefano Sabatini
 * Copyright (c) 2008 Victor Paesa
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
 * movie video source
 *
 * @todo use direct rendering (no allocation of a new frame)
 * @todo support a PTS correction mechanism
 */

#include <float.h>
#include <stdint.h>

#include "libavutil/attributes.h"
#include "libavutil/avstring.h"
#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/timestamp.h"

#include "libavcodec/avcodec.h"

#include "libavformat/avformat.h"

#include "audio.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct VRMovieStream {
    AVStream *st;
    AVCodecContext *codec_ctx;
    int done;
    int64_t discontinuity_threshold;
    int64_t last_pts;
} VRMovieStream;

typedef struct VRMovieContext {
    /* common A/V fields */
    const AVClass *class;
    int64_t seek_point;   ///< seekpoint in microseconds
    double seek_point_d;
    char *format_name;
    char *file_name;
    char *stream_specs; /**< user-provided list of streams, separated by + */
    int stream_index; /**< for compatibility */
    int loop_count;
    int64_t discontinuity_threshold;
    int64_t ts_offset;
    
    AVFormatContext *format_ctx;
    int eof;
    AVPacket pkt, pkt0;
    
    char *orientation; //byx-add
    char *orientation_last; //byx-add
    int rate_zhd;  //zhd-add
    int rate_last; //zhd-add
    
    int max_stream_index; /**< max stream # actually used for output */
    MovieStream *st; /**< array of all streams, one per output */
    int *out_index; /**< stream number -> output number map, or -1 */
} VRMovieContext;

#define OFFSET(x) offsetof(MovieContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static const AVOption vrmovie_options[]= {
    { "filename",     NULL,                      OFFSET(file_name),    AV_OPT_TYPE_STRING,                                    .flags = FLAGS },
    { "format_name",  "set format name",         OFFSET(format_name),  AV_OPT_TYPE_STRING,                                    .flags = FLAGS },
    { "f",            "set format name",         OFFSET(format_name),  AV_OPT_TYPE_STRING,                                    .flags = FLAGS },
    { "stream_index", "set stream index",        OFFSET(stream_index), AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, INT_MAX,                 FLAGS  },
    { "si",           "set stream index",        OFFSET(stream_index), AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, INT_MAX,                 FLAGS  },
    { "seek_point",   "set seekpoint (seconds)", OFFSET(seek_point_d), AV_OPT_TYPE_DOUBLE, { .dbl =  0 },  0, (INT64_MAX-1) / 1000000, FLAGS },
    { "sp",           "set seekpoint (seconds)", OFFSET(seek_point_d), AV_OPT_TYPE_DOUBLE, { .dbl =  0 },  0, (INT64_MAX-1) / 1000000, FLAGS },
    { "streams",      "set streams",             OFFSET(stream_specs), AV_OPT_TYPE_STRING, {.str =  0},  CHAR_MAX, CHAR_MAX, FLAGS },
    { "s",            "set streams",             OFFSET(stream_specs), AV_OPT_TYPE_STRING, {.str =  0},  CHAR_MAX, CHAR_MAX, FLAGS },
    { "loop",         "set loop count",          OFFSET(loop_count),   AV_OPT_TYPE_INT,    {.i64 =  1},  0,        INT_MAX, FLAGS },
    { "discontinuity", "set discontinuity threshold", OFFSET(discontinuity_threshold), AV_OPT_TYPE_DURATION, {.i64 = 0}, 0, INT64_MAX, FLAGS },
    { "orientation",  "set orientation",         OFFSET(orientation),  AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MAX, CHAR_MAX, FLAGS},  //byx-add
    { "orientation_last",  "set if orientation is changed",         OFFSET(orientation_last),  AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MAX, CHAR_MAX, FLAGS},  //byx-add
    { "rate_zhd",     "set rate_zhd",            OFFSET(rate_zhd),     AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, INT_MAX,                 FLAGS  }, // zhd add
    
    { NULL },
};

static int movie_config_output_props(AVFilterLink *outlink);
static int movie_request_frame(AVFilterLink *outlink);

static AVStream *find_stream(void *log, AVFormatContext *avf, const char *spec)
{
    int i, ret, already = 0, stream_id = -1;
    char type_char[2], dummy;
    AVStream *found = NULL;
    enum AVMediaType type;
    
    ret = sscanf(spec, "d%1[av]%d%c", type_char, &stream_id, &dummy);
    if (ret >= 1 && ret <= 2) {
        type = type_char[0] == 'v' ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
        ret = av_find_best_stream(avf, type, stream_id, -1, NULL, 0);
        if (ret < 0) {
            av_log(log, AV_LOG_ERROR, "No %s stream with index '%d' found\n",
                   av_get_media_type_string(type), stream_id);
            return NULL;
        }
        return avf->streams[ret];
    }
    for (i = 0; i < avf->nb_streams; i++) {
        ret = avformat_match_stream_specifier(avf, avf->streams[i], spec);
        if (ret < 0) {
            av_log(log, AV_LOG_ERROR,
                   "Invalid stream specifier \"%s\"\n", spec);
            return NULL;
        }
        if (!ret)
            continue;
        if (avf->streams[i]->discard != AVDISCARD_ALL) {
            already++;
            continue;
        }
        if (found) {
            av_log(log, AV_LOG_WARNING,
                   "Ambiguous stream specifier \"%s\", using #%d\n", spec, i);
            break;
        }
        found = avf->streams[i];
    }
    if (!found) {
        av_log(log, AV_LOG_WARNING, "Stream specifier \"%s\" %s\n", spec,
               already ? "matched only already used streams" :
               "did not match any stream");
        return NULL;
    }
    if (found->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
        found->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        av_log(log, AV_LOG_ERROR, "Stream specifier \"%s\" matched a %s stream,"
               "currently unsupported by libavfilter\n", spec,
               av_get_media_type_string(found->codecpar->codec_type));
        return NULL;
    }
    return found;
}

static int open_stream(void *log, MovieStream *st)
{
    AVCodec *codec;
    int ret;
    
    codec = avcodec_find_decoder(st->st->codecpar->codec_id);
    if (!codec) {
        av_log(log, AV_LOG_ERROR, "Failed to find any codec\n");
        return AVERROR(EINVAL);
    }
    
    st->codec_ctx = avcodec_alloc_context3(codec);
    if (!st->codec_ctx)
        return AVERROR(ENOMEM);
    
    ret = avcodec_parameters_to_context(st->codec_ctx, st->st->codecpar);
    if (ret < 0)
        return ret;
    
    st->codec_ctx->refcounted_frames = 1;
    
    if ((ret = avcodec_open2(st->codec_ctx, codec, NULL)) < 0) {
        av_log(log, AV_LOG_ERROR, "Failed to open codec\n");
        return ret;
    }
    
    return 0;
}

static int guess_channel_layout(MovieStream *st, int st_index, void *log_ctx)
{
    AVCodecParameters *dec_par = st->st->codecpar;
    char buf[256];
    int64_t chl = av_get_default_channel_layout(dec_par->channels);
    
    if (!chl) {
        av_log(log_ctx, AV_LOG_ERROR,
               "Channel layout is not set in stream %d, and could not "
               "be guessed from the number of channels (%d)\n",
               st_index, dec_par->channels);
        return AVERROR(EINVAL);
    }
    
    av_get_channel_layout_string(buf, sizeof(buf), dec_par->channels, chl);
    av_log(log_ctx, AV_LOG_WARNING,
           "Channel layout is not set in output stream %d, "
           "guessed channel layout is '%s'\n",
           st_index, buf);
    dec_par->channel_layout = chl;
    return 0;
}

static av_cold int movie_common_init(AVFilterContext *ctx)
{
    VRMovieContext *vrmovie = ctx->priv;
    AVInputFormat *iformat = NULL;
    int64_t timestamp;
    int nb_streams = 1, ret, i;
    char default_streams[16], *stream_specs, *spec, *cursor;
    char name[16];
    AVStream *st;
    
    if (!vrmovie->file_name) {
        av_log(ctx, AV_LOG_ERROR, "No filename provided!\n");
        return AVERROR(EINVAL);
    }
    
    vrmovie->seek_point = vrmovie->seek_point_d * 1000000 + 0.5;
    
    stream_specs = vrmovie->stream_specs;
    if (!stream_specs) {
        snprintf(default_streams, sizeof(default_streams), "d%c%d",
                 !strcmp(ctx->filter->name, "amovie") ? 'a' : 'v',
                 vrmovie->stream_index);
        stream_specs = default_streams;
    }
    for (cursor = stream_specs; *cursor; cursor++)
        if (*cursor == '+')
            nb_streams++;
    
    if (vrmovie->loop_count != 1 && nb_streams != 1) {
        av_log(ctx, AV_LOG_ERROR,
               "Loop with several streams is currently unsupported\n");
        return AVERROR_PATCHWELCOME;
    }
    
    // Try to find the movie format (container)
    iformat = vrmovie->format_name ? av_find_input_format(vrmovie->format_name) : NULL;
    
    vrmovie->format_ctx = NULL;
    if ((ret = avformat_open_input(&vrmovie->format_ctx, vrmovie->file_name, iformat, NULL)) < 0) {  //到movie这里又重新打开了avformat_open_input函数
        av_log(ctx, AV_LOG_ERROR,
               "Failed to avformat_open_input '%s'\n", vrmovie->file_name);
        return ret;
    }
    if ((ret = avformat_find_stream_info(vrmovie->format_ctx, NULL)) < 0)
        av_log(ctx, AV_LOG_WARNING, "Failed to find stream info\n");
    
    // if seeking requested, we execute it
    if (vrmovie->seek_point > 0) {
        timestamp = vrmovie->seek_point;
        // add the stream start time, should it exist
        if (vrmovie->format_ctx->start_time != AV_NOPTS_VALUE) {
            if (timestamp > 0 && vrmovie->format_ctx->start_time > INT64_MAX - timestamp) {
                av_log(ctx, AV_LOG_ERROR,
                       "%s: seek value overflow with start_time:%"PRId64" seek_point:%"PRId64"\n",
                       vrmovie->file_name, vrmovie->format_ctx->start_time, vrmovie->seek_point);
                return AVERROR(EINVAL);
            }
            timestamp += vrmovie->format_ctx->start_time;
        }
        if ((ret = av_seek_frame(vrmovie->format_ctx, -1, timestamp, AVSEEK_FLAG_BACKWARD)) < 0) {
            av_log(ctx, AV_LOG_ERROR, "%s: could not seek to position %"PRId64"\n",
                   vrmovie->file_name, timestamp);
            return ret;
        }
    }
    
    for (i = 0; i < vrmovie->format_ctx->nb_streams; i++)
        vrmovie->format_ctx->streams[i]->discard = AVDISCARD_ALL;
    
    vrmovie->st = av_calloc(nb_streams, sizeof(*vrmovie->st));
    if (!vrmovie->st)
        return AVERROR(ENOMEM);
    av_log(NULL ,AV_LOG_DEBUG, "------------------------------------avformat_find_stream_info\n");
    for (i = 0; i < nb_streams; i++) {
        spec = av_strtok(stream_specs, "+", &cursor);
        if (!spec)
            return AVERROR_BUG;
        stream_specs = NULL; /* for next strtok */
        st = find_stream(ctx, vrmovie->format_ctx, spec);
        if (!st)
            return AVERROR(EINVAL);
        st->discard = AVDISCARD_DEFAULT;
        vrmovie->st[i].st = st;
        vrmovie->max_stream_index = FFMAX(vrmovie->max_stream_index, st->index);
        av_log(NULL , AV_LOG_DEBUG, "movie->max_stream_index == %d \n", movie->max_stream_index);
        vrmovie->st[i].discontinuity_threshold =
        av_rescale_q(vrmovie->discontinuity_threshold, AV_TIME_BASE_Q, st->time_base);
    }
    if (av_strtok(NULL, "+", &cursor))
        return AVERROR_BUG;
    
    vrmovie->out_index = av_calloc(vrmovie->max_stream_index + 1,
                                 sizeof(*vrmovie->out_index));
    if (!vrmovie->out_index)
        return AVERROR(ENOMEM);
    for (i = 0; i <= vrmovie->max_stream_index; i++)
        vrmovie->out_index[i] = -1;
    for (i = 0; i < nb_streams; i++) {
        AVFilterPad pad = { 0 };
        vrmovie->out_index[movie->st[i].st->index] = i;
        snprintf(name, sizeof(name), "out%d", i);
        pad.type          = vrmovie->st[i].st->codecpar->codec_type;
        pad.name          = av_strdup(name);
        if (!pad.name)
            return AVERROR(ENOMEM);
        pad.config_props  = vrmovie_config_output_props;
        pad.request_frame = vrmovie_request_frame;
        if ((ret = ff_insert_outpad(ctx, i, &pad)) < 0) {   //Insert a new output pad for the filter.
            av_freep(&pad.name);
            return ret;
        }
        if ( vrmovie->st[i].st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            !vrmovie->st[i].st->codecpar->channel_layout) {
            ret = guess_channel_layout(&vrmovie->st[i], i, ctx);
            if (ret < 0)
                return ret;
        }
        ret = open_stream(ctx, &vrmovie->st[i]);
        if (ret < 0)
            return ret;
    }
    
    av_log(ctx, AV_LOG_VERBOSE, "seek_point:%"PRIi64" format_name:%s file_name:%s stream_index:%d\n",
           vrmovie->seek_point, vrmovie->format_name, vrmovie->file_name,
           vrmovie->stream_index);
    
    return 0;
}

static av_cold void vrmovie_uninit(AVFilterContext *ctx)
{
    VRMovieContext *vrmovie = ctx->priv;
    int i;
    
    for (i = 0; i < ctx->nb_outputs; i++) {
        av_freep(&ctx->output_pads[i].name);
        if (vrmovie->st[i].st)
            avcodec_free_context(&vrmovie->st[i].codec_ctx);
    }
    av_freep(&vrmovie->st);
    av_freep(&vrmovie->out_index);
    if (vrmovie->format_ctx)
        avformat_close_input(&vrmovie->format_ctx);
}

static int vrmovie_query_formats(AVFilterContext *ctx)
{
    VRMovieContext *vrmovie = ctx->priv;
    int list[] = { 0, -1 };
    int64_t list64[] = { 0, -1 };
    int i, ret;
    
    for (i = 0; i < ctx->nb_outputs; i++) {
        VRMovieStream *st = &vrmovie->st[i];
        AVCodecParameters *c = st->st->codecpar;
        AVFilterLink *outlink = ctx->outputs[i];
        
        switch (c->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                list[0] = c->format;
                if ((ret = ff_formats_ref(ff_make_format_list(list), &outlink->in_formats)) < 0)
                    return ret;
                break;
            case AVMEDIA_TYPE_AUDIO:
                list[0] = c->format;
                if ((ret = ff_formats_ref(ff_make_format_list(list), &outlink->in_formats)) < 0)
                    return ret;
                list[0] = c->sample_rate;
                if ((ret = ff_formats_ref(ff_make_format_list(list), &outlink->in_samplerates)) < 0)
                    return ret;
                list64[0] = c->channel_layout;
                if ((ret = ff_channel_layouts_ref(avfilter_make_format64_list(list64),
                                                  &outlink->in_channel_layouts)) < 0)
                    return ret;
                break;
        }
    }
    
    return 0;
}

static int vrmovie_config_output_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    VRMovieContext *vrmovie  = ctx->priv;
    unsigned out_id = FF_OUTLINK_IDX(outlink);
    VRMovieStream *st = &vrmovie->st[out_id];
    AVCodecParameters *c = st->st->codecpar;
    
    outlink->time_base = st->st->time_base;
    
    switch (c->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            outlink->w          = c->width;
            outlink->h          = c->height;
            outlink->frame_rate = st->st->r_frame_rate;
            break;
        case AVMEDIA_TYPE_AUDIO:
            break;
    }
    
    return 0;
}

static char *describe_frame_to_str(char *dst, size_t dst_size,
                                   AVFrame *frame, enum AVMediaType frame_type,
                                   AVFilterLink *link)
{
    switch (frame_type) {
        case AVMEDIA_TYPE_VIDEO:
            snprintf(dst, dst_size,
                     "video pts:%s time:%s size:%dx%d aspect:%d/%d",
                     av_ts2str(frame->pts), av_ts2timestr(frame->pts, &link->time_base),
                     frame->width, frame->height,
                     frame->sample_aspect_ratio.num,
                     frame->sample_aspect_ratio.den);
            break;
        case AVMEDIA_TYPE_AUDIO:
            snprintf(dst, dst_size,
                     "audio pts:%s time:%s samples:%d",
                     av_ts2str(frame->pts), av_ts2timestr(frame->pts, &link->time_base),
                     frame->nb_samples);
            break;
        default:
            snprintf(dst, dst_size, "%s BUG", av_get_media_type_string(frame_type));
            break;
    }
    return dst;
}

static int rewind_file(AVFilterContext *ctx)
{
    VRMovieContext *vrmovie = ctx->priv;
    int64_t timestamp = vrmovie->seek_point;
    int ret, i;
    
    if (vrmovie->format_ctx->start_time != AV_NOPTS_VALUE)
        timestamp += vrmovie->format_ctx->start_time;
    ret = av_seek_frame(vrmovie->format_ctx, -1, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Unable to loop: %s\n", av_err2str(ret));
        vrmovie->loop_count = 1; /* do not try again */
        return ret;
    }
    
    for (i = 0; i < ctx->nb_outputs; i++) {
        avcodec_flush_buffers(vrmovie->st[i].codec_ctx);
        vrmovie->st[i].done = 0;
    }
    vrmovie->eof = 0;
    return 0;
}

/**
 * Try to push a frame to the requested output.
 *
 * @param ctx     filter context
 * @param out_id  number of output where a frame is wanted;
 *                if the frame is read from file, used to set the return value;
 *                if the codec is being flushed, flush the corresponding stream
 * @return  1 if a frame was pushed on the requested output,
 *          0 if another attempt is possible,
 *          <0 AVERROR code
 */
static int vrmovie_push_frame(AVFilterContext *ctx, unsigned out_id)
{
    VRMovieContext *vrmovie = ctx->priv;
    AVPacket *pkt = &vrmovie->pkt;
    enum AVMediaType frame_type;
    VRMovieStream *st;
    int ret, got_frame = 0, pkt_out_id;
    AVFilterLink *outlink;
    AVFrame *frame;
    av_log(NULL , AV_LOG_DEBUG, "movie----orientation = %s \n", vrmovie->orientation); //byx-add
    av_log(NULL , AV_LOG_DEBUG, "out_id = %d \n", out_id); //byx-add
    
    //zhd-add ------------------------------------------------------
    
    if (vrmovie->rate_zhd != vrmovie->rate_last) {
        av_log(NULL, AV_LOG_INFO, "movie: rate %d -> %d\n", movie->rate_last, movie->rate_zhd);
        //        av_log(NULL, AV_LOG_INFO, "pts: %lf, timestamp: %d\n", movie->d, (int)(movie->d * 30));
        vrmovie->rate_last = vrmovie->rate_zhd;
        
        avcodec_free_context(&vrmovie->st[0].codec_ctx);
        
        //        movie->format_ctx->iformat->read_seek(movie->format_ctx, 9, (int)(movie->d * 30), 1);
        
        int i, nb_streams = 1, stream_index;
        char  default_streams[16], *stream_specs, *spec, *cursor;
        AVStream *st1;
        
        for (i = 0; i < vrmovie->format_ctx->nb_streams; i++)
            vrmovie->format_ctx->streams[i]->discard = AVDISCARD_ALL;
        
        snprintf(default_streams, sizeof(default_streams), "dv%d", vrmovie->rate_zhd);
        stream_specs = default_streams;
        
        for (i = 0; i < nb_streams; i++) {
            spec = av_strtok(stream_specs, "+", &cursor);
            if (!spec)
                return AVERROR_BUG;
            st1 = find_stream(ctx, vrmovie->format_ctx, spec);
            st1->discard = AVDISCARD_DEFAULT;
            vrmovie->st[i].st = st1;
            vrmovie->max_stream_index = FFMAX(vrmovie->max_stream_index, st1->index);
            av_log(NULL , AV_LOG_INFO, "movie->max_stream_index == %d \n", vrmovie->max_stream_index);
            vrmovie->st[i].discontinuity_threshold =
            av_rescale_q(vrmovie->discontinuity_threshold, AV_TIME_BASE_Q, st1->time_base);
        }
        
        av_free(vrmovie->out_index);
        vrmovie->out_index = av_calloc(vrmovie->max_stream_index + 1, sizeof(*vrmovie->out_index));
        
        for (i = 0; i <= vrmovie->max_stream_index; i++)
            vrmovie->out_index[i] = -1;
        
        vrmovie->out_index[vrmovie->st[0].st->index] = 0;
        
        open_stream(ctx, &vrmovie->st[0]);
    }
    
    //zhd-add ------------------------------------------------------
    
    if (!pkt->size) {
        if (vrmovie->eof) {
            if (vrmovie->st[out_id].done) {
                if (vrmovie->loop_count != 1) {
                    ret = rewind_file(ctx);
                    if (ret < 0)
                        return ret;
                    vrmovie->loop_count -= vrmovie->loop_count > 1;
                    av_log(ctx, AV_LOG_VERBOSE, "Stream finished, looping.\n");
                    return 0; /* retry */
                }
                return AVERROR_EOF;
            }
            pkt->stream_index = vrmovie->st[out_id].st->index;
            /* packet is already ready for flushing */
        } else {
            //            av_opt_set(movie->format_ctx->priv_data, "orientation", movie->orientation, 0); //byx-add
            
            ret = av_read_frame(vrmovie->format_ctx, &vrmovie->pkt0);
            if (ret < 0) {
                av_init_packet(&vrmovie->pkt0); /* ready for flushing */
                *pkt = vrmovie->pkt0;
                if (ret == AVERROR_EOF) {
                    movie->eof = 1;
                    return 0; /* start flushing */
                }
                return ret;
            }
            *pkt = vrmovie->pkt0;
        }
    }
    
    pkt_out_id = pkt->stream_index > vrmovie->max_stream_index ? -1 :
    vrmovie->out_index[pkt->stream_index];
    av_log(NULL, AV_LOG_DEBUG, "============================================ \n");
    
    av_log(NULL, AV_LOG_DEBUG, "movie:pkt->stream_index = %d \n", pkt->stream_index);
    av_log(NULL , AV_LOG_DEBUG, "（最大值是多少）movie->max_stream_index = %d \n", vrmovie->max_stream_index );
    av_log(NULL , AV_LOG_DEBUG, " movie->out_index[pkt->stream_index] = %d \n", vrmovie->out_index[pkt->stream_index] );
    
    av_log(NULL , AV_LOG_DEBUG, "pkt_out_id = %d \n", pkt_out_id );
    av_log(NULL, AV_LOG_DEBUG, "============================================ \n");
    
    
    if (pkt_out_id < 0) {
        av_packet_unref(&vrmovie->pkt0);
        pkt->size = 0; /* ready for next run */
        pkt->data = NULL;
        return 0;
    }
    st = &vrmovie->st[pkt_out_id];
    outlink = ctx->outputs[pkt_out_id];
    
    frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);
    
    frame_type = st->st->codecpar->codec_type;
    switch (frame_type) {
        case AVMEDIA_TYPE_VIDEO:
            ret = avcodec_decode_video2(st->codec_ctx, frame, &got_frame, pkt);
            break;
        case AVMEDIA_TYPE_AUDIO:
            ret = avcodec_decode_audio4(st->codec_ctx, frame, &got_frame, pkt);
            break;
        default:
            ret = AVERROR(ENOSYS);
            break;
    }
    if (ret < 0) {
        av_log(ctx, AV_LOG_WARNING, "Decode error: %s\n", av_err2str(ret));
        av_frame_free(&frame);
        av_packet_unref(&vrmovie->pkt0);
        vrmovie->pkt.size = 0;
        vrmovie->pkt.data = NULL;
        return 0;
    }
    if (!ret || st->st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        ret = pkt->size;
    
    pkt->data += ret;
    pkt->size -= ret;
    if (pkt->size <= 0) {
        av_packet_unref(&vrmovie->pkt0);
        pkt->size = 0; /* ready for next run */
        pkt->data = NULL;
    }
    if (!got_frame) {
        if (!ret)
            st->done = 1;
        av_frame_free(&frame);
        return 0;
    }
    
    frame->pts = frame->best_effort_timestamp;
    if (frame->pts != AV_NOPTS_VALUE) {
        if (vrmovie->ts_offset)
            frame->pts += av_rescale_q_rnd(vrmovie->ts_offset, AV_TIME_BASE_Q, outlink->time_base, AV_ROUND_UP);
        if (st->discontinuity_threshold) {
            if (st->last_pts != AV_NOPTS_VALUE) {
                int64_t diff = frame->pts - st->last_pts;
                if (diff < 0 || diff > st->discontinuity_threshold) {
                    av_log(ctx, AV_LOG_VERBOSE, "Discontinuity in stream:%d diff:%"PRId64"\n", pkt_out_id, diff);
                    vrmovie->ts_offset += av_rescale_q_rnd(-diff, outlink->time_base, AV_TIME_BASE_Q, AV_ROUND_UP);
                    frame->pts -= diff;
                }
            }
        }
        st->last_pts = frame->pts;
    }
    ff_dlog(ctx, "movie_push_frame(): file:'%s' %s\n", vrmovie->file_name,
            describe_frame_to_str((char[1024]){0}, 1024, frame, frame_type, outlink));
    
    if (st->st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        av_log(ctx, AV_LOG_DEBUG, "frame->format = %d \n", frame->format);
        av_log(ctx, AV_LOG_DEBUG, "outlink->format = %d \n", outlink->format);
        
        
        if (frame->format != outlink->format) {
            av_log(ctx, AV_LOG_ERROR, "Format changed %s -> %s, discarding frame\n",
                   av_get_pix_fmt_name(outlink->format),
                   av_get_pix_fmt_name(frame->format)
                   );
            av_frame_free(&frame);
            return 0;
        }
    }
    ret = ff_filter_frame(outlink, frame);
    
    
    if (ret < 0)
        return ret;
    
    return pkt_out_id == out_id;
}

static int vrmovie_request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    unsigned out_id = FF_OUTLINK_IDX(outlink);
    int ret;
    
    while (1) {
        ret = movie_push_frame(ctx, out_id);  //Try to push a frame to the requested output.
        if (ret)
            return FFMIN(ret, 0);
    }
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    VRMovieContext *vrmovie = ctx->priv;
    int ret = AVERROR(ENOSYS);
    
    if (!strcmp(cmd, "seek")) {
        int idx, flags, i;
        int64_t ts;
        char tail[2];
        
        if (sscanf(args, "%i|%"SCNi64"|%i %1s", &idx, &ts, &flags, tail) != 3)
            return AVERROR(EINVAL);
        
        ret = av_seek_frame(vrmovie->format_ctx, idx, ts, flags);
        if (ret < 0)
            return ret;
        
        for (i = 0; i < ctx->nb_outputs; i++) {
            avcodec_flush_buffers(vrmovie->st[i].codec_ctx);
            vrmovie->st[i].done = 0;
        }
        return ret;
    } else if (!strcmp(cmd, "get_duration")) {
        int print_len;
        char tail[2];
        
        if (!res || res_len <= 0)
            return AVERROR(EINVAL);
        
        if (args && sscanf(args, "%1s", tail) == 1)
            return AVERROR(EINVAL);
        
        print_len = snprintf(res, res_len, "%"PRId64, vrmovie->format_ctx->duration);
        if (print_len < 0 || print_len >= res_len)
            return AVERROR(EINVAL);
        
        return 0;
    }
    
    return ret;
}

#if CONFIG_MOVIE_FILTER

AVFILTER_DEFINE_CLASS(vrmovie);

AVFilter ff_avsrc_vr_movie = {
    .name          = "vrmovie",
    .description   = NULL_IF_CONFIG_SMALL("Read from a movie source."),
    .priv_size     = sizeof(VRMovieContext),
    .priv_class    = &vrmovie_class,
    .init          = vrmovie_common_init,
    .uninit        = vrmovie_uninit,
    .query_formats = vrmovie_query_formats,
    
    .inputs    = NULL,
    .outputs   = NULL,
    .flags     = AVFILTER_FLAG_DYNAMIC_OUTPUTS,
    .process_command = process_command
};

#endif  /* CONFIG_MOVIE_FILTER */

#if CONFIG_AMOVIE_FILTER

#define amovie_options movie_options
AVFILTER_DEFINE_CLASS(vramovie);

AVFilter ff_avsrc_vr_amovie = {
    .name          = "vramovie",
    .description   = NULL_IF_CONFIG_SMALL("Read audio from a movie source."),
    .priv_size     = sizeof(VRMovieContext),
    .init          = vrmovie_common_init,
    .uninit        = vrmovie_uninit,
    .query_formats = vrmovie_query_formats,
    
    .inputs     = NULL,
    .outputs    = NULL,
    .priv_class = &vramovie_class,
    .flags      = AVFILTER_FLAG_DYNAMIC_OUTPUTS,
    .process_command = process_command,
};

#endif /* CONFIG_AMOVIE_FILTER */



