/* Copyright (C) 2019 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "screencaster.h"

#include <QDebug>
#include <QGuiApplication>
#include <QScreen>
#include <QStandardPaths>
#include <QDir>

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
#include <libavdevice/avdevice.h>
#include <libavutil/imgutils.h>
#include <libavutil/timestamp.h>
#include <libavutil/time.h>
}

#ifdef SAILFISH
#include <QTransform>
#include <QPainter>
#include "wayland-lipstick-recorder-client-protocol.h"
#endif

#include "settings.h"
#include "pulseaudiosource.h"
#include "utils.h"
#include "contentserver.h"


ScreenCaster::ScreenCaster(QObject *parent) : QObject(parent)
{
    connect(this, &ScreenCaster::error,
            ContentServerWorker::instance(), &ContentServerWorker::screenErrorHandler);
#ifdef DESKTOP
    connect(this, &ScreenCaster::readNextVideoFrame,
            this, &ScreenCaster::readVideoFrame, Qt::QueuedConnection);
#endif
#ifdef SAILFISH
    auto s = Settings::instance();
    frameTimer.setTimerType(Qt::PreciseTimer);
    frameTimer.setInterval(1000/s->getScreenFramerate());
    connect(&frameTimer, &QTimer::timeout, this, &ScreenCaster::writeVideoData, Qt::DirectConnection);
    repaintTimer.setInterval(1000); // 1s
    repaintTimer.setTimerType(Qt::VeryCoarseTimer);
    connect(&repaintTimer, &QTimer::timeout, this, &ScreenCaster::repaint);
#endif
}

ScreenCaster::~ScreenCaster()
{
    if (video_sws_ctx) {
        sws_freeContext(video_sws_ctx);
        video_sws_ctx = nullptr;
    }
    if (in_frame_s) {
        av_frame_free(&in_frame_s);
        in_frame_s = nullptr;
    }
    if (video_outbuf) {
        av_free(video_outbuf);
        video_outbuf = nullptr;
    }
    if (out_format_ctx) {
        if (out_format_ctx->pb) {
            if (out_format_ctx->pb->buffer) {
                av_free(out_format_ctx->pb->buffer);
                out_format_ctx->pb->buffer = nullptr;
            }
            avio_context_free(&out_format_ctx->pb);
            out_format_ctx->pb = nullptr;
        }
        avformat_free_context(out_format_ctx);
        out_format_ctx = nullptr;
    }
    if (in_frame) {
        av_frame_free(&in_frame);
        in_frame = nullptr;
    }
    if (in_video_format_ctx) {
        avformat_close_input(&in_video_format_ctx);
        in_video_format_ctx = nullptr;
    }
    if (audio_swr_ctx) {
        swr_free(&audio_swr_ctx);
        audio_swr_ctx = nullptr;
    }
    if (in_audio_codec_ctx) {
        avcodec_free_context(&in_audio_codec_ctx);
        in_audio_codec_ctx = nullptr;
    }

    av_packet_unref(&video_out_pkt);
    av_packet_unref(&audio_out_pkt);
}

bool ScreenCaster::init()
{
    qDebug() << "ScreenCaster init";

    char errbuf[50];
    int ret = 0;

    // in video

    auto s = Settings::instance();
    auto video_framerate = s->getScreenFramerate();
    video_pkt_duration = av_rescale_q(1, AVRational{1, video_framerate}, AVRational{1, 1000000});
    video_size = QGuiApplication::primaryScreen()->size();
    qDebug() << "video_framerate:" << video_framerate;
    qDebug() << "video_pkt_duration:" << video_pkt_duration;

    if (video_size.width() < video_size.height()) {
        qDebug() << "Portrait orientation detected, so transposing to landscape";
        video_size.transpose();
    }

    int xoff = 0; int yoff = 0;
    if (s->getScreenCropTo169()) {
        qDebug() << "Cropping to 16:9 ratio";
        int h = video_size.width()*(9.0/16.0);
        h = h - h%2;
        if (h <= video_size.height()) {
            yoff = (video_size.height() - h) / 2;
            video_size.setHeight(h);
        } else {
            int w = video_size.height()*(16.0/9.0);
            w = w - w%2;
            xoff = (video_size.width() - w) / 2;
            video_size.setWidth(w);
        }
    }

    AVDictionary* options = nullptr;

#ifdef SAILFISH
    bgImg = QImage(video_size, QImage::Format_RGB32); bgImg.fill(Qt::black);

    auto in_video_codec = avcodec_find_decoder(AV_CODEC_ID_RAWVIDEO);
    if (!in_video_codec) {
        qWarning() << "Error in avcodec_find_decoder for video";
        return false;
    }

    in_video_codec_ctx = avcodec_alloc_context3(in_video_codec);
    if (!in_video_codec_ctx) {
        qWarning() << "Error: avcodec_alloc_context3 is null";
        return false;
    }
#else
    auto x11grabUrl = QString(":0.0+%1,%2").arg(xoff).arg(yoff).toLatin1();
    auto video_ssize = QString("%1x%2").arg(video_size.width())
            .arg(video_size.height()).toLatin1();
    qDebug() << "video size:" << video_ssize;
    qDebug() << "video framerate:" << video_framerate;
    qDebug() << "video x11grabUrl:" << x11grabUrl;
    if (QGuiApplication::screens().size() > 1) {
        qWarning() << "More that one screen but capturing only first screen";
    }

    auto in_video_format = av_find_input_format("x11grab");

    if (av_dict_set_int(&options, "framerate", video_framerate, 0) < 0 ||
        av_dict_set(&options, "preset", "fast", 0) < 0 ||
        av_dict_set(&options, "video_size", video_ssize.data(), 0) < 0 ||
        //av_dict_set(&options, "show_region", "1", 0) < 0 ||
        //av_dict_set(&options, "region_border", "1", 0) < 0 ||
        //av_dict_set(&options, "follow_mouse", "centered", 0) < 0 ||
        av_dict_set(&options, "draw_mouse", "1", 0) < 0) {
        qWarning() << "Error in av_dict_set";
        return false;
    }

    in_video_format_ctx = nullptr;
    if (avformat_open_input(&in_video_format_ctx, x11grabUrl.data(), in_video_format,
                            &options) < 0) {
        qWarning() << "Error in avformat_open_input for video";
        av_dict_free(&options);
        return false;
    }
    av_dict_free(&options);

    int in_video_stream_idx = 0;
    if (in_video_format_ctx->streams[in_video_stream_idx]->
            codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        qWarning() << "x11grab stream[0] is not video";
        return false;
    }

    /*qDebug() << "x11grab video stream:";
    qDebug() << " id:" << in_video_format_ctx->streams[in_video_stream_idx]->id;
    qDebug() << " height:" << in_video_format_ctx->streams[in_video_stream_idx]->codecpar->height;
    qDebug() << " width:" << in_video_format_ctx->streams[in_video_stream_idx]->codecpar->width;
    qDebug() << " codec_id:" << in_video_format_ctx->streams[in_video_stream_idx]->codecpar->codec_id;
    qDebug() << " codec_type:" << in_video_format_ctx->streams[in_video_stream_idx]->codecpar->codec_type;*/

    auto in_video_codec = avcodec_find_decoder(in_video_format_ctx->
                                               streams[in_video_stream_idx]->
                                               codecpar->codec_id);
    if (!in_video_codec) {
        qWarning() << "Error in avcodec_find_decoder for video";
        return false;
    }

    in_video_codec_ctx = in_video_format_ctx->streams[in_video_stream_idx]->codec;
    if (!in_video_codec_ctx) {
        qWarning() << "Error: in_video_codec_ctx is null";
        return false;
    }
#endif

    in_video_codec_ctx->width = video_size.width();
    in_video_codec_ctx->height = video_size.height();
    in_video_codec_ctx->time_base.num = 1;
    in_video_codec_ctx->time_base.den = 1000000;
    in_video_codec_ctx->pix_fmt = AV_PIX_FMT_RGB32;

    ret = avcodec_open2(in_video_codec_ctx, in_video_codec, &options);
    if (ret < 0) {
        qWarning() << "Error in avcodec_open2 for in_video_codec_ctx:" << av_make_error_string(errbuf, 50, ret);
        return false;
    }

    in_frame = av_frame_alloc();
    if(!in_frame) {
        qWarning() << "Error in av_frame_alloc";
        return false;
    }

    // in audio

    if (s->getScreenAudio()) {
        auto in_audio_codec = avcodec_find_decoder(AV_CODEC_ID_PCM_S16LE);
        if (!in_audio_codec) {
            qWarning() << "Error in avcodec_find_decoder for audio";
            return false;
        }

        in_audio_codec_ctx = avcodec_alloc_context3(in_audio_codec);
        if (!in_audio_codec_ctx) {
            qWarning() << "Error: avcodec_alloc_context3 is null";
            return false;
        }

        in_audio_codec_ctx->channels = PulseAudioSource::sampleSpec.channels;
        in_audio_codec_ctx->channel_layout = av_get_default_channel_layout(in_audio_codec_ctx->channels);
        in_audio_codec_ctx->sample_rate = PulseAudioSource::sampleSpec.rate;
        in_audio_codec_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
        in_audio_codec_ctx->time_base.num = 1;
        in_audio_codec_ctx->time_base.den = 1000000;

        ret = avcodec_open2(in_audio_codec_ctx, in_audio_codec, &options);
        if (ret < 0) {
            qWarning() << "Error in avcodec_open2 for in_audio_codec_ctx:" << av_make_error_string(errbuf, 50, ret);
            return false;
        }

        /*qDebug() << "In audio codec params:";
        qDebug() << " channels:" << in_audio_codec_ctx->channels;
        qDebug() << " channel_layout:" << in_audio_codec_ctx->channel_layout;
        qDebug() << " sample_rate:" << in_audio_codec_ctx->sample_rate;
        qDebug() << " codec_id:" << in_audio_codec_ctx->codec_id;
        qDebug() << " codec_type:" << in_audio_codec_ctx->codec_type;
        qDebug() << " sample_fmt:" << in_audio_codec_ctx->sample_fmt;
        qDebug() << " frame_size:" << in_audio_codec_ctx->frame_size;
        qDebug() << " time_base.num:" << in_audio_codec_ctx->time_base.num;
        qDebug() << " time_base.den:" << in_audio_codec_ctx->time_base.den;*/
    }

    // out

    if (avformat_alloc_output_context2(&out_format_ctx, nullptr, "mpegts", nullptr) < 0) {
        qWarning() << "Error in avformat_alloc_output_context2";
        return false;
    }

    auto out_video_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!out_video_codec) {
        qWarning() << "Error in avcodec_find_encoder for H264";
        return false;
    }

    AVStream *out_video_stream = avformat_new_stream(out_format_ctx, out_video_codec);
    if (!out_video_stream) {
        qWarning() << "Error in avformat_new_stream for video";
        return false;
    }
    out_video_stream->id = 0;
    out_video_codec_ctx = out_video_stream->codec;
    out_video_codec_ctx->pix_fmt  = AV_PIX_FMT_YUV420P;
    out_video_codec_ctx->width = video_size.width();
    out_video_codec_ctx->height = video_size.height();
    out_video_codec_ctx->flags = AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;
    //out_video_codec_ctx->gop_size = 25;
    //out_video_codec_ctx->max_b_frames = 3;
    //out_video_codec_ctx->b_frame_strategy = 1;
    out_video_codec_ctx->time_base.num = 1;
    out_video_codec_ctx->time_base.den = video_framerate;

    auto passLogfile = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath(
                QString("passlogfile-%1.log").arg(Utils::randString())).toLatin1();
    av_dict_set(&options, "preset", "ultrafast", 0);
    av_dict_set(&options, "tune", "zerolatency", 0);
    av_dict_set(&options, "g", "0", 0);
    av_dict_set(&options, "passlogfile", passLogfile.constData(), 0);
    ret = avcodec_open2(out_video_codec_ctx, out_video_codec, &options);
    if (ret < 0) {
        qWarning() << "Error in avcodec_open2 for out_video_codec_ctx:" << av_make_error_string(errbuf, 50, ret);
        av_dict_free(&options);
        return false;
    }
    av_dict_free(&options);

    qDebug() << "Out video codec params:" << out_video_codec_ctx->codec_id;
    qDebug() << " codec_id:" << out_video_codec_ctx->codec_id;
    qDebug() << " codec_type:" << out_video_codec_ctx->codec_type;
    qDebug() << " pix_fmt:" << out_video_codec_ctx->pix_fmt;
    qDebug() << " bit_rate:" << out_video_codec_ctx->bit_rate;
    qDebug() << " width:" << out_video_codec_ctx->width;
    qDebug() << " height:" << out_video_codec_ctx->height;
    qDebug() << " gop_size:" << out_video_codec_ctx->gop_size;
    qDebug() << " max_b_frames:" << out_video_codec_ctx->max_b_frames;
    qDebug() << " b_frame_strategy:" << out_video_codec_ctx->b_frame_strategy;
    qDebug() << " time_base.num:" << out_video_codec_ctx->time_base.num;
    qDebug() << " time_base.den:" << out_video_codec_ctx->time_base.den;

    int nbytes = av_image_get_buffer_size(out_video_codec_ctx->pix_fmt,
                                          out_video_codec_ctx->width,
                                          out_video_codec_ctx->height, 32);

    video_outbuf = (uint8_t*)av_malloc(nbytes);
    if (!video_outbuf) {
        qWarning() << "Unable to allocate memory";
        return false;
    }

    in_frame_s = av_frame_alloc();
    if(!in_frame_s) {
        qWarning() << "Error in av_frame_alloc";
        return false;
    }

    if (av_image_fill_arrays(in_frame_s->data, in_frame_s->linesize,
                                 video_outbuf, out_video_codec_ctx->pix_fmt,
                                 out_video_codec_ctx->width,
                                 out_video_codec_ctx->height, 1) < 0) {
        qWarning() << "Error in av_image_fill_arrays";
        return false;
    }

    video_sws_ctx = sws_getContext(in_video_codec_ctx->width, in_video_codec_ctx->height,
                        in_video_codec_ctx->pix_fmt, out_video_codec_ctx->width,
                        out_video_codec_ctx->height, out_video_codec_ctx->pix_fmt,
                        SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!video_sws_ctx) {
        qWarning() << "Error in sws_getContext";
        return false;
    }

    const size_t outbuf_size = 500000;
    uint8_t *outbuf = (uint8_t*)av_malloc(outbuf_size);
    if (!outbuf) {
        qWarning() << "Unable to allocate memory";
        return false;
    }

    out_format_ctx->pb = avio_alloc_context(outbuf, outbuf_size, 1, nullptr, nullptr,
                                            write_packet_callback, nullptr);
    if (s->getScreenAudio()) {
        //auto out_audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        auto out_audio_codec = avcodec_find_encoder(AV_CODEC_ID_MP3);
        if (!out_audio_codec) {
            qWarning() << "Error in avcodec_find_encoder";
            return false;
        }

        AVStream *out_audio_stream = avformat_new_stream(out_format_ctx, out_audio_codec);
        if (!out_audio_stream) {
            qWarning() << "Error in avformat_new_stream for audio";
            return false;
        }
        out_audio_stream->id = 1;
        out_audio_codec_ctx = out_audio_stream->codec;
        out_audio_codec_ctx->channels = in_audio_codec_ctx->channels;
        out_audio_codec_ctx->sample_rate = in_audio_codec_ctx->sample_rate;
        out_audio_codec_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        out_audio_codec_ctx->flags = AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;
        out_audio_codec_ctx->channel_layout = in_audio_codec_ctx->channel_layout;
        out_audio_codec_ctx->time_base.num = 1;
        out_audio_codec_ctx->time_base.den = in_audio_codec_ctx->sample_rate;

        av_dict_set(&options, "b", "128k", 0);
        av_dict_set(&options, "q", "9", 0);
        ret = avcodec_open2(out_audio_codec_ctx, out_audio_codec, &options);
        if (ret < 0) {
            qWarning() << "Error in avcodec_open2 for out_audio_codec_ctx:" << av_make_error_string(errbuf, 50, ret);
            av_dict_free(&options);
            return false;
        }
        av_dict_free(&options);

        in_audio_codec_ctx->frame_size = out_audio_codec_ctx->frame_size;
        audio_frame_size = av_samples_get_buffer_size(nullptr,
                                              in_audio_codec_ctx->channels,
                                              out_audio_codec_ctx->frame_size,
                                              in_audio_codec_ctx->sample_fmt, 0);
        audio_pkt_duration = av_rescale_q(out_audio_codec_ctx->frame_size,
                                          AVRational{1, out_audio_codec_ctx->sample_rate},
                                          AVRational{1, 1000000});
        video_audio_ratio = video_pkt_duration/audio_pkt_duration;

        qDebug() << "Out audio codec params:" << out_audio_codec_ctx->codec_id;
        qDebug() << " codec_id:" << out_audio_codec_ctx->codec_id;
        qDebug() << " codec_type:" << out_audio_codec_ctx->codec_type;
        qDebug() << " bit_rate:" << out_audio_codec_ctx->bit_rate;
        qDebug() << " channels:" << out_audio_codec_ctx->channels;
        qDebug() << " channel_layout:" << out_audio_codec_ctx->channel_layout;
        qDebug() << " sample_rate:" << out_audio_codec_ctx->sample_rate;
        qDebug() << " sample_fmt:" << out_audio_codec_ctx->sample_fmt;
        qDebug() << " time_base.num:" << out_audio_codec_ctx->time_base.num;
        qDebug() << " time_base.den:" << out_audio_codec_ctx->time_base.den;
        qDebug() << " frame_size:" << out_audio_codec_ctx->frame_size;
        qDebug() << " audio_frame_size:" << audio_frame_size;
        qDebug() << " audio_pkt_duration:" << audio_pkt_duration;
        qDebug() << " video_audio_ratio:" << video_audio_ratio;

        audio_swr_ctx = swr_alloc();
        av_opt_set_int(audio_swr_ctx, "in_channel_layout", in_audio_codec_ctx->channel_layout, 0);
        av_opt_set_int(audio_swr_ctx, "out_channel_layout", out_audio_codec_ctx->channel_layout,  0);
        av_opt_set_int(audio_swr_ctx, "in_sample_rate", in_audio_codec_ctx->sample_rate, 0);
        av_opt_set_int(audio_swr_ctx, "out_sample_rate", out_audio_codec_ctx->sample_rate, 0);
        av_opt_set_sample_fmt(audio_swr_ctx, "in_sample_fmt",  in_audio_codec_ctx->sample_fmt, 0);
        av_opt_set_sample_fmt(audio_swr_ctx, "out_sample_fmt", out_audio_codec_ctx->sample_fmt,  0);
        swr_init(audio_swr_ctx);
    }

    ret = avformat_write_header(out_format_ctx, nullptr);
    if (ret != AVSTREAM_INIT_IN_WRITE_HEADER && ret != AVSTREAM_INIT_IN_INIT_OUTPUT) {
        qWarning() << "Error in avformat_write_header:"
                   << av_make_error_string(errbuf, 50, ret);
        return false;
    }

    return true;
}

void ScreenCaster::start()
{
#ifdef SAILFISH
    qDebug() << "start thread:" << QThread::currentThreadId();
    qDebug() << "frameTimer interval:" << frameTimer.interval();
    recorder = std::unique_ptr<Recorder>(new Recorder(this,1));
    frameTimer.start();
    repaintTimer.start();
#else
    emit readNextVideoFrame();
#endif
}

bool ScreenCaster::audioEnabled()
{
    return audio_frame_size > 0;
}

int ScreenCaster::write_packet_callback(void *opaque, uint8_t *buf, int buf_size)
{
    Q_UNUSED(opaque);
    auto worker = ContentServerWorker::instance();
    //qDebug() << "write_packet_callback:" << buf_size;
    worker->sendScreenCaptureData(static_cast<void*>(buf), buf_size);
    return buf_size;
}

void ScreenCaster::writeAudioData(const QByteArray& data)
{
    //qDebug() << "writeAudioData:" << data.size();

    if (video_pkt_start_time == 0) {
        //qDebug() << "Discarding audio because video didn't start";
        return;
    }

    audio_outbuf.append(data);

    if (!writeAudioData2()) {
        errorHandler();
    }
}

bool ScreenCaster::writeAudioData2()
{
    //qDebug() << "audio_outbuf.size:" << audio_outbuf.size();

    while (audio_outbuf.size() >= audio_frame_size) {
        const char* d = audio_outbuf.data();
        int ret = -1;

        int ndelay = 0;
        bool start = false;
        if (audio_pkt_time == 0) {
            qDebug() << "First audio samples";
            audio_pkt_time = video_pkt_start_time - 3 * audio_pkt_duration;
            ndelay = 3;
            start = true;
        } /*else {
            int audio_delay = (av_gettime() - audio_pkt_time)/audio_pkt_duration;
            int audio_buff_size = audio_outbuf.size() / audio_frame_size;
            qDebug() << "audio_buff_size:" << audio_buff_size;
            qDebug() << "audio_delay:" << audio_delay;
            if (audio_delay < -2) {
                qWarning() << "Audio too early, so not sending audio pkt";
                break;
            }
        }*/

        AVPacket audio_in_pkt;
        if (av_new_packet(&audio_in_pkt, audio_frame_size) < 0) {
            qDebug() << "Error in av_new_packet";
            return false;
        }

        int samples_to_remove = 1;
        for (int i = 0; i < ndelay + 1; ++i) {
            //qDebug() << "Encoding new audio data";
            //qDebug() << "i:" << i;
            //qDebug() << "audio_pkt_time:" << audio_pkt_time;

            audio_in_pkt.dts = audio_pkt_time;
            audio_in_pkt.pts = audio_pkt_time;
            audio_in_pkt.duration = audio_pkt_duration;

            memcpy(audio_in_pkt.data, d, audio_frame_size);

            ret = avcodec_send_packet(in_audio_codec_ctx, &audio_in_pkt);
            if (ret == 0) {
                ret = avcodec_receive_frame(in_audio_codec_ctx, in_frame);
                if (ret == 0 || ret == AVERROR(EAGAIN)) {
                    //qDebug() << "Audio frame decoded";
                    // resampling
                    AVFrame* frame = av_frame_alloc();
                    frame->channel_layout = out_audio_codec_ctx->channel_layout;
                    frame->format = out_audio_codec_ctx->sample_fmt;
                    frame->sample_rate = out_audio_codec_ctx->sample_rate;
                    swr_convert_frame(audio_swr_ctx, frame, in_frame);
                    frame->pts = in_frame->pts;
                    av_init_packet(&audio_out_pkt);
                    ret = avcodec_send_frame(out_audio_codec_ctx, frame);
                    av_frame_free(&frame);
                    if (ret == 0 || ret == AVERROR(EAGAIN)) {
                        ret = avcodec_receive_packet(out_audio_codec_ctx, &audio_out_pkt);
                        if (ret == 0) {
                            //qDebug() << "Audio packet encoded";
                            start = false;
                            if (!writeAudioData3())
                                return false;
                        } else if (ret == AVERROR(EAGAIN)) {
                            //qDebug() << "Packet not ready";
                        } else {
                            qWarning() << "Error in avcodec_receive_packet for audio";
                            return false;
                        }
                    } else {
                        qWarning() << "Error in avcodec_send_frame for audio";
                        return false;
                    }
                } else if (ret == AVERROR(EAGAIN)) {
                    //qDebug() << "Audio frame not ready";
                } else {
                    qWarning() << "Error in avcodec_receive_frame for audio";
                    return false;
                }
            } else {
                qWarning() << "Error in avcodec_send_packet for audio";
                return false;
            }

            if (start) {
                audio_pkt_time += audio_pkt_duration;
            }
        }

        int audio_to_remove = samples_to_remove * audio_frame_size;
        if (audio_to_remove >= audio_outbuf.size())
            audio_outbuf.clear();
        else
            audio_outbuf.remove(0, audio_to_remove);

        av_packet_unref(&audio_in_pkt);
    }

    return true;
}

bool ScreenCaster::writeAudioData3()
{
    auto in_tb  = in_audio_codec_ctx->time_base;
    auto out_tb = out_format_ctx->streams[1]->time_base;

    audio_out_pkt.stream_index = 1; // audio output stream
    audio_out_pkt.pos = -1;
    audio_out_pkt.pts = av_rescale_q_rnd(audio_pkt_time, in_tb, out_tb,
                     static_cast<AVRounding>(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
    audio_out_pkt.dts = av_rescale_q_rnd(audio_pkt_time, in_tb, out_tb,
                     static_cast<AVRounding>(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
    audio_out_pkt.duration = av_rescale_q(audio_out_pkt.duration, in_tb, out_tb);

    int ret = av_interleaved_write_frame(out_format_ctx, &audio_out_pkt);
    if (ret < 0) {
        qWarning() << "Error in av_write_frame for audio";
        return false;
    }

    audio_pkt_time += audio_pkt_duration;

    return true;
}

void ScreenCaster::errorHandler()
{
    qDebug() << "Screen capture error";
#ifdef SAILFISH
    frameTimer.stop();
    repaintTimer.stop();
#endif
    emit error();
}

#ifdef SAILFISH
void ScreenCaster::repaint()
{
    if (recorder)
        recorder->repaint();
}

void ScreenCaster::writeVideoData()
{
    qDebug() << "writeVideoData thread:" << QThread::currentThreadId();

    int64_t curr_time = av_gettime();

    if (video_pkt_time == 0) {
        video_pkt_time = curr_time;
        video_pkt_start_time = curr_time;
    }

    int video_delay = (curr_time - video_pkt_time)/video_pkt_duration;
    bool img_not_changed = havePrevVideoPkt &&
            (!currImgBuff || (currImgBuff && !currImgBuff->busy));
    bool video_delayed = havePrevVideoPkt ? video_delay > 0 : false;
    bool audio_delayed = false;

    if (audioEnabled() && audio_pkt_time != 0 && havePrevVideoPkt) {
        //int audio_delay = (curr_time - audio_pkt_time)/audio_pkt_duration;
        //int audio_video_delay = (video_pkt_time - audio_pkt_time)/audio_pkt_duration;
        int audio_buff_size = audio_outbuf.size() / audio_frame_size;
        audio_delayed = audio_buff_size > 0;
        /*qDebug() << "audio_delay:" << audio_delay;
        qDebug() << "audio_buff_size:" << audio_buff_size;
        qDebug() << "audio_video_delay:" << audio_video_delay;*/
    }

    /*qDebug() << "video_delay:" << video_delay;
    qDebug() << "img_not_changed:" << img_not_changed;
    qDebug() << "audio_delayed:" << audio_delayed;
    qDebug() << "video_delayed:" << video_delayed;*/

    /*if (video_delay < -2) {
        qWarning() << "Video too early, so not sending video pkt";
        return;
    }*/

    if (!img_not_changed && !video_delayed && !audio_delayed) {
        //qDebug() << "Encoding new pkt";

        auto img = makeCurrImg();
        auto size = img.byteCount();
        auto data = img.constBits();

        AVPacket video_in_pkt;
        if (av_new_packet(&video_in_pkt, size) < 0) {
            qDebug() << "Error in av_new_packet";
            errorHandler();
            return;
        }

        memcpy(video_in_pkt.data, data, size);
        int ret = avcodec_send_packet(in_video_codec_ctx, &video_in_pkt);
        if (ret != 0 && ret != AVERROR(EAGAIN)) {
            qWarning() << "Error in avcodec_send_packet for video";
            errorHandler();
            return;
        }

        ret = avcodec_receive_frame(in_video_codec_ctx, in_frame);
        if (ret == 0) {
            //qDebug() << "Video frame decoded";
            sws_scale(video_sws_ctx, in_frame->data, in_frame->linesize, 0,
                      out_video_codec_ctx->height, in_frame_s->data,
                      in_frame_s->linesize);
            in_frame_s->pict_type = AV_PICTURE_TYPE_NONE;
            in_frame_s->width = in_frame->width;
            in_frame_s->height = in_frame->width;
            in_frame_s->format = in_frame->format;
            av_frame_copy_props(in_frame_s, in_frame);
            av_init_packet(&video_out_pkt);
            ret = avcodec_send_frame(out_video_codec_ctx, in_frame_s);
            if (ret == 0 || ret == AVERROR(EAGAIN)) {
                ret = avcodec_receive_packet(out_video_codec_ctx, &video_out_pkt);
                if (ret == 0) {
                    //qDebug() << "Video packet encoded";
                    havePrevVideoPkt = true;
                    if (!writeVideoData2()) {
                        errorHandler();
                        return;
                    }
                } else if (ret == AVERROR(EAGAIN)) {
                    //qDebug() << "Packet not ready";
                } else {
                    qWarning() << "Error in avcodec_receive_packet for video";
                    errorHandler();
                    return;
                }
            } else {
                qWarning() << "Error in avcodec_send_frame for video";
                errorHandler();
                return;
            }
        } else if (ret == AVERROR(EAGAIN)) {
            //qDebug() << "Video frame not ready";
        } else {
            qWarning() << "Error in avcodec_receive_frame for video";
            errorHandler();
            return;
        }

        av_packet_unref(&video_in_pkt);
    } else {
        //qDebug() << "Sending previous pkt instead new";
        if (!writeVideoData2()) {
            errorHandler();
            return;
        }
    }
}

bool ScreenCaster::writeVideoData2()
{
    int ndelay = (av_gettime() - video_pkt_time)/video_pkt_duration;
    //qDebug() << "video ndelay:" << ndelay;
    ndelay = ndelay < 0 ? 0 : ndelay;

    auto in_tb  = in_video_codec_ctx->time_base;
    auto out_tb = out_format_ctx->streams[0]->time_base;

    for (int i = 0; i < ndelay + 1; ++i) {
        //qDebug() << "i:" << i;
        //qDebug() << "video_pkt_time:" << video_pkt_time;
        video_out_pkt.stream_index = 0; // video output stream
        video_out_pkt.pos = -1;
        video_out_pkt.pts = av_rescale_q_rnd(video_pkt_time, in_tb, out_tb,
                         static_cast<AVRounding>(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        video_out_pkt.dts = av_rescale_q_rnd(video_pkt_time, in_tb, out_tb,
                         static_cast<AVRounding>(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        video_out_pkt.duration = av_rescale_q(video_out_pkt.duration, in_tb, out_tb);

        /*qDebug() << "video_out_pkt.flags AV_PKT_FLAG_KEY:" << (video_out_pkt.flags & AV_PKT_FLAG_KEY);
        qDebug() << "video_out_pkt.flags AV_PKT_FLAG_DISCARD:" << (video_out_pkt.flags & AV_PKT_FLAG_DISCARD);
        qDebug() << "video_out_pkt.flags AV_PKT_FLAG_CORRUPT:" << (video_out_pkt.flags & AV_PKT_FLAG_CORRUPT);
        qDebug() << "video_out_pkt.pts:" << video_out_pkt.pts;
        qDebug() << "video_out_pkt.dts:" << video_out_pkt.dts;*/

        AVPacket pkt; av_packet_ref(&pkt, &video_out_pkt);

        int ret = av_interleaved_write_frame(out_format_ctx, &pkt);
        if (ret < 0) {
            qWarning() << "Error in av_write_frame for video";
            return false;
        }

        video_pkt_time += video_pkt_duration;
    }

    return true;
}

QImage ScreenCaster::makeCurrImg()
{
    QImage img = currImgBuff ? currImgTransform == LIPSTICK_RECORDER_TRANSFORM_Y_INVERTED ?
                currImgBuff->image.mirrored(false, true) : currImgBuff->image : bgImg;
    if (img.width() < img.height()) {
        auto orientation = QGuiApplication::primaryScreen()->orientation();
        if (orientation == Qt::LandscapeOrientation) {
            QTransform t; t.rotate(-90);
            img = img.transformed(t);
        } else if (orientation == Qt::InvertedLandscapeOrientation) {
            QTransform t; t.rotate(-270);
            img = img.transformed(t);
        } else {
            if (orientation == Qt::InvertedPortraitOrientation) {
                QTransform t; t.rotate(-180);
                img = img.transformed(t);
            }
            QImage target = bgImg;
            QPainter p(&target);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            img = img.scaledToHeight(target.height(), Qt::SmoothTransformation);
            p.drawImage((target.width()-img.width())/2, 0, img);
            p.end();
            img = target;
        }
    }

    if (img.format() != QImage::Format_RGB32) {
        img = img.convertToFormat(QImage::Format_RGB32);
    }

    if (currImgBuff) {
        currImgBuff->busy = false;
        if (recorder && recorder->m_starving)
            recorder->recordFrame();
    }

    return img;
}

bool ScreenCaster::event(QEvent *e)
{
    if (e->type() == FrameEvent::FrameEventType) {
        //qDebug() << "New screen image";
        auto fe = static_cast<FrameEvent*>(e);
        currImgBuff = fe->buffer;
        currImgTransform = fe->transform;
    }
    return true;
}
#endif

#ifdef DESKTOP
void ScreenCaster::readVideoFrame()
{
    //qDebug() << "========= ScreenCaster::readVideoFrame";
    //char errbuf[50];
    bool error = false;
    if (av_read_frame(in_video_format_ctx, &in_pkt) >= 0) {
        if (in_pkt.stream_index == 0) { // video in stream
            int ret = avcodec_send_packet(in_video_codec_ctx, &in_pkt);
            //qDebug() << "avcodec_send_packet:" << av_make_error_string(errbuf, 50, ret);
            if (ret == 0 || ret == AVERROR(EAGAIN)) {
                ret = avcodec_receive_frame(in_video_codec_ctx, in_frame);
                //qDebug() << "avcodec_receive_frame:" << av_make_error_string(errbuf, 50, ret);
                if (ret == 0) {
                    //qDebug() << "Video frame decoded";
                    sws_scale(video_sws_ctx, in_frame->data, in_frame->linesize, 0,
                              out_video_codec_ctx->height, in_frame_s->data,
                              in_frame_s->linesize);
                    in_frame_s->width = in_frame->width;
                    in_frame_s->height = in_frame->width;
                    in_frame_s->format = in_frame->format;
                    av_frame_copy_props(in_frame_s, in_frame);
                    out_pkt.data = nullptr;
                    out_pkt.size = 0;
                    ret = avcodec_send_frame(out_video_codec_ctx, in_frame_s);
                    //qDebug() << "avcodec_send_frame:" << av_make_error_string(errbuf, 50, ret);
                    if (ret == 0 || ret == AVERROR(EAGAIN)) {
                        ret = avcodec_receive_packet(out_video_codec_ctx, &out_pkt);
                        //qDebug() << "avcodec_receive_packet:" << av_make_error_string(errbuf, 50, ret);
                        if (ret == 0) {
                            //qDebug() << "Video packet encoded";
                            auto in_tb  = AVRational{1, 1000000};
                            auto out_tb = out_format_ctx->streams[0]->time_base;
                            out_pkt.stream_index = 0; // video out stream
                            out_pkt.pts = av_rescale_q_rnd(out_pkt.pts, in_tb, out_tb,
                               static_cast<AVRounding>(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
                            out_pkt.dts = av_rescale_q_rnd(out_pkt.dts, in_tb, out_tb,
                               static_cast<AVRounding>(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
                            out_pkt.duration = av_rescale_q(out_pkt.duration, in_tb, out_tb);
                            out_pkt.pos = -1;
                            //ret = av_write_frame(out_format_ctx, &out_pkt);
                            ret = av_interleaved_write_frame(out_format_ctx, &out_pkt);
                            //qDebug() << "video av_write_frame:" << av_make_error_string(errbuf, 50, ret);
                            if (ret < 0) {
                                qWarning() << "Error in av_write_frame for video";
                            }
                            emit readNextVideoFrame();
                        } else if (ret == AVERROR(EAGAIN)) {
                            //qDebug() << "Packet not ready";
                            emit readNextVideoFrame();
                        } else {
                            qWarning() << "Error in avcodec_receive_packet for video";
                            error = true;
                        }
                    } else {
                        qWarning() << "Error in avcodec_send_frame for video";
                        error = true;
                    }
                } else if (ret == AVERROR(EAGAIN)) {
                    //qDebug() << "Video frame not ready";
                    emit readNextVideoFrame();
                } else {
                    qWarning() << "Error in avcodec_receive_frame for video";
                    error = true;
                }
            } else {
                qWarning() << "Error in avcodec_receive_frame for video";
                error = true;
            }
        } else {
            qDebug() << "stream_index != video_index for video";
            error = true;
        }
    } else {
        qWarning() << "Error in av_read_frame for video";
        error = true;
    }

    av_packet_unref(&in_pkt);
    av_packet_unref(&out_pkt);

    if (error) {
        qWarning() << "Error readVideoFrame";
        emit frameError();
    }
}
#endif
