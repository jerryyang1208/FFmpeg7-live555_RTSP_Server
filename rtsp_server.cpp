#include <iostream>
#include <string>
#include <vector>
#include <dirent.h>
#include <algorithm>
#include <sys/time.h>

// FFmpeg
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavcodec/bsf.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

// Live555
#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include "GroupsockHelper.hh"

// ------------------------------------------------------------------
// FFmpegVideoSource: 支持全格式解码与 H.264 实时转码
// ------------------------------------------------------------------
class FFmpegVideoSource : public FramedSource {
public:
    static FFmpegVideoSource* createNew(UsageEnvironment& env, std::string const& fileName) {
        return new FFmpegVideoSource(env, fileName);
    }

protected:
    FFmpegVideoSource(UsageEnvironment& env, std::string const& fileName) 
        : FramedSource(env), fFileName(fileName) {
        initFFmpeg();
    }

    virtual ~FFmpegVideoSource() {
        if (bsf_ctx) av_bsf_free(&bsf_ctx);
        if (vCodecPar) avcodec_parameters_free(&vCodecPar);
        
        // 释放转码相关资源
        if (fDecCtx) avcodec_free_context(&fDecCtx);
        if (fEncCtx) avcodec_free_context(&fEncCtx);
        if (fSwsCtx) sws_freeContext(fSwsCtx);
        if (fDecFrame) av_frame_free(&fDecFrame);
        if (fEncFrame) av_frame_free(&fEncFrame);
        if (fEncPkt) av_packet_free(&fEncPkt);
        
        if (fmtCtx) avformat_close_input(&fmtCtx);
    }

    virtual unsigned maxFrameSize() const { return 500000; } 

    virtual void doGetNextFrame() {
        if (fNeedsTranscoding) {
            deliverTranscodedFrame();
        } else {
            deliverDirectFrame();
        }
    }

private:
    void initFFmpeg() {
        fmtCtx = avformat_alloc_context();
        if (!fmtCtx) {
            envir() << "Error: Failed to allocate AVFormatContext\n";
            return;
        }
        
        if (avformat_open_input(&fmtCtx, fFileName.c_str(), NULL, NULL) < 0) {
            envir() << "Error: Failed to open input file: " << fFileName.c_str() << "\n";
            return;
        }
        
        if (avformat_find_stream_info(fmtCtx, NULL) < 0) {
            envir() << "Error: Failed to find stream info\n";
            avformat_close_input(&fmtCtx);
            fmtCtx = nullptr;
            return;
        }

        for (unsigned i = 0; i < fmtCtx->nb_streams; i++) {
            if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoStreamIdx = i;
                vCodecPar = avcodec_parameters_alloc();
                if (!vCodecPar) {
                    envir() << "Error: Failed to allocate AVCodecParameters\n";
                    avformat_close_input(&fmtCtx);
                    fmtCtx = nullptr;
                    return;
                }
                avcodec_parameters_copy(vCodecPar, fmtCtx->streams[i]->codecpar);
                fTimeBase = av_q2d(fmtCtx->streams[i]->time_base);
                break;
            }
        }

        if (videoStreamIdx < 0) {
            envir() << "Error: No video stream found\n";
            if (vCodecPar) avcodec_parameters_free(&vCodecPar);
            avformat_close_input(&fmtCtx);
            fmtCtx = nullptr;
            return;
        }

        // 判断是否需要转码
        if (vCodecPar->codec_id == AV_CODEC_ID_H264 || vCodecPar->codec_id == AV_CODEC_ID_HEVC) {
            fNeedsTranscoding = false;
            const AVBitStreamFilter* filter = (vCodecPar->codec_id == AV_CODEC_ID_HEVC) ? 
                av_bsf_get_by_name("hevc_mp4toannexb") : av_bsf_get_by_name("h264_mp4toannexb");
            if (filter) {
                if (av_bsf_alloc(filter, &bsf_ctx) < 0) {
                    envir() << "Error: Failed to allocate BSF context\n";
                } else {
                    if (avcodec_parameters_copy(bsf_ctx->par_in, vCodecPar) < 0) {
                        envir() << "Error: Failed to copy codec parameters to BSF context\n";
                        av_bsf_free(&bsf_ctx);
                        bsf_ctx = nullptr;
                    } else if (av_bsf_init(bsf_ctx) < 0) {
                        envir() << "Error: Failed to initialize BSF context\n";
                        av_bsf_free(&bsf_ctx);
                        bsf_ctx = nullptr;
                    }
                }
            }
        } else {
            fNeedsTranscoding = true;
            setupTranscoder();
        }
        
        gettimeofday(&fStartTime, NULL);
    }

    // 初始化解码器和 x264 编码器
    void setupTranscoder() {
        // 1. 初始化解码器
        const AVCodec* dec = avcodec_find_decoder(vCodecPar->codec_id);
        if (!dec) {
            envir() << "Error: Failed to find decoder for codec ID: " << vCodecPar->codec_id << "\n";
            return;
        }
        
        fDecCtx = avcodec_alloc_context3(dec);
        if (!fDecCtx) {
            envir() << "Error: Failed to allocate decoder context\n";
            return;
        }
        
        if (avcodec_parameters_to_context(fDecCtx, vCodecPar) < 0) {
            envir() << "Error: Failed to copy codec parameters to decoder context\n";
            avcodec_free_context(&fDecCtx);
            fDecCtx = nullptr;
            return;
        }
        
        if (avcodec_open2(fDecCtx, dec, NULL) < 0) {
            envir() << "Error: Failed to open decoder\n";
            avcodec_free_context(&fDecCtx);
            fDecCtx = nullptr;
            return;
        }

        // 2. 初始化编码器 (强制转换为 H.264)
        const AVCodec* enc = avcodec_find_encoder_by_name("libx264");
        if (!enc) {
            envir() << "Error: Failed to find libx264 encoder\n";
            avcodec_free_context(&fDecCtx);
            fDecCtx = nullptr;
            return;
        }
        
        fEncCtx = avcodec_alloc_context3(enc);
        if (!fEncCtx) {
            envir() << "Error: Failed to allocate encoder context\n";
            avcodec_free_context(&fDecCtx);
            fDecCtx = nullptr;
            return;
        }
        
        fEncCtx->width = fDecCtx->width;
        fEncCtx->height = fDecCtx->height;
        fEncCtx->time_base = fmtCtx->streams[videoStreamIdx]->time_base; // 继承原始时基
        fEncCtx->framerate = av_guess_frame_rate(fmtCtx, fmtCtx->streams[videoStreamIdx], NULL);
        fEncCtx->pix_fmt = AV_PIX_FMT_YUV420P;
        fEncCtx->max_b_frames = 0; // 禁用B帧，减少延迟
        fEncCtx->gop_size = 10; // 减小GOP大小，减少延迟
        
        // 极致降低延迟参数，防止阻塞 Live555 单线程
        AVDictionary* opt = NULL;
        av_dict_set(&opt, "preset", "ultrafast", 0);
        av_dict_set(&opt, "tune", "zerolatency", 0);
        av_dict_set(&opt, "profile", "baseline", 0); // 使用baseline profile，减少复杂度
        av_dict_set(&opt, "bufsize", "1000", 0); // 减小缓冲区大小
        
        if (avcodec_open2(fEncCtx, enc, &opt) < 0) {
            envir() << "Error: Failed to open encoder\n";
            av_dict_free(&opt);
            avcodec_free_context(&fDecCtx);
            avcodec_free_context(&fEncCtx);
            fDecCtx = nullptr;
            fEncCtx = nullptr;
            return;
        }
        av_dict_free(&opt);

        // 3. 分配内存
        fDecFrame = av_frame_alloc();
        if (!fDecFrame) {
            envir() << "Error: Failed to allocate decode frame\n";
            avcodec_free_context(&fDecCtx);
            avcodec_free_context(&fEncCtx);
            fDecCtx = nullptr;
            fEncCtx = nullptr;
            return;
        }
        
        fEncFrame = av_frame_alloc();
        if (!fEncFrame) {
            envir() << "Error: Failed to allocate encode frame\n";
            av_frame_free(&fDecFrame);
            avcodec_free_context(&fDecCtx);
            avcodec_free_context(&fEncCtx);
            fDecFrame = nullptr;
            fDecCtx = nullptr;
            fEncCtx = nullptr;
            return;
        }
        
        fEncFrame->format = fEncCtx->pix_fmt;
        fEncFrame->width = fEncCtx->width;
        fEncFrame->height = fEncCtx->height;
        
        if (av_frame_get_buffer(fEncFrame, 0) < 0) {
            envir() << "Error: Failed to allocate buffer for encode frame\n";
            av_frame_free(&fDecFrame);
            av_frame_free(&fEncFrame);
            avcodec_free_context(&fDecCtx);
            avcodec_free_context(&fEncCtx);
            fDecFrame = nullptr;
            fEncFrame = nullptr;
            fDecCtx = nullptr;
            fEncCtx = nullptr;
            return;
        }
        
        fEncPkt = av_packet_alloc();
        if (!fEncPkt) {
            envir() << "Error: Failed to allocate encode packet\n";
            av_frame_free(&fDecFrame);
            av_frame_free(&fEncFrame);
            avcodec_free_context(&fDecCtx);
            avcodec_free_context(&fEncCtx);
            fDecFrame = nullptr;
            fEncFrame = nullptr;
            fDecCtx = nullptr;
            fEncCtx = nullptr;
            return;
        }
    }

    // 直通模式：处理原生的 H.264 / H.265
    void deliverDirectFrame() {
        if (!isCurrentlyAwaitingData()) return;

        if (!fmtCtx) {
            handleClosure();
            return;
        }

        AVPacket pkt;
        int ret = av_read_frame(fmtCtx, &pkt);
        if (ret >= 0) {
            if (pkt.stream_index == videoStreamIdx) {
                if (bsf_ctx) {
                    ret = av_bsf_send_packet(bsf_ctx, &pkt);
                    if (ret < 0) {
                        envir() << "Error: Failed to send packet to BSF: " << ret << "\n";
                        av_packet_unref(&pkt);
                        handleRetry();
                        return;
                    }
                    
                    ret = av_bsf_receive_packet(bsf_ctx, &pkt);
                    if (ret != 0) {
                        av_packet_unref(&pkt);
                        handleRetry();
                        return;
                    }
                }
                
                if (pkt.size > fMaxSize) {
                    fFrameSize = fMaxSize;
                    fNumTruncatedBytes = pkt.size - fMaxSize;
                } else {
                    fFrameSize = pkt.size;
                    fNumTruncatedBytes = 0;
                }
                
                if (pkt.data) {
                    memcpy(fTo, pkt.data, fFrameSize);
                } else {
                    envir() << "Warning: Empty packet data\n";
                    av_packet_unref(&pkt);
                    handleRetry();
                    return;
                }

                double ptsInSeconds = pkt.pts * fTimeBase;
                fPresentationTime.tv_sec = fStartTime.tv_sec + (long)ptsInSeconds;
                fPresentationTime.tv_usec = fStartTime.tv_usec + (long)((ptsInSeconds - (long)ptsInSeconds) * 1000000.0);
                
                // 确保时间戳不为负
                if (fPresentationTime.tv_usec < 0) {
                    fPresentationTime.tv_sec--;
                    fPresentationTime.tv_usec += 1000000;
                }
                if (fPresentationTime.tv_sec < 0) {
                    fPresentationTime.tv_sec = 0;
                    fPresentationTime.tv_usec = 0;
                }

                if (pkt.duration > 0) {
                    fDurationInMicroseconds = (unsigned)(pkt.duration * fTimeBase * 1000000.0);
                } else {
                    double fps = av_q2d(fmtCtx->streams[videoStreamIdx]->avg_frame_rate);
                    fDurationInMicroseconds = (unsigned)(fps > 0 ? 1000000.0 / fps : 40000);
                }

                av_packet_unref(&pkt);
                FramedSource::afterGetting(this);
                return;
            }
            av_packet_unref(&pkt);
            handleRetry();
        } else if (ret == AVERROR_EOF) {
            handleClosure();
        } else {
            envir() << "Error: Failed to read frame: " << ret << "\n";
            handleRetry();
        }
    }

    // 转码模式：处理 VP9, AV1, MPEG4 等
    void deliverTranscodedFrame() {
        if (!isCurrentlyAwaitingData()) return;

        if (!fmtCtx || !fDecCtx || !fEncCtx || !fEncPkt) {
            handleClosure();
            return;
        }

        // 限制每帧处理时间，避免阻塞事件循环
        struct timeval start_time, current_time;
        gettimeofday(&start_time, NULL);
        const long MAX_PROCESSING_TIME_US = 50000; // 50ms

        // 状态机：首先尝试从编码器拉取上一轮可能缓存的包
        int ret = avcodec_receive_packet(fEncCtx, fEncPkt);
        if (ret == 0) {
            goto PACKET_READY;
        } else if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            envir() << "Error: Failed to receive packet from encoder: " << ret << "\n";
            handleRetry();
            return;
        }

        while (true) {
            // 检查处理时间，避免阻塞事件循环
            gettimeofday(&current_time, NULL);
            long processing_time = (current_time.tv_sec - start_time.tv_sec) * 1000000 + 
                                 (current_time.tv_usec - start_time.tv_usec);
            if (processing_time > MAX_PROCESSING_TIME_US) {
                // 处理时间过长，将剩余工作交给下一次调用
                handleRetry();
                return;
            }

            // 尝试从解码器拉取已解码的帧
            ret = avcodec_receive_frame(fDecCtx, fDecFrame);
            if (ret == 0) {
                // 如果像素格式不符合 H.264 要求的 YUV420P，进行转换
                if (!fSwsCtx) {
                    // 【已修正】：将 width, height, pix_fmt 的参数顺序调整为正确顺序
                    fSwsCtx = sws_getContext(fDecCtx->width, fDecCtx->height, fDecCtx->pix_fmt,
                                             fEncCtx->width, fEncCtx->height, fEncCtx->pix_fmt,
                                             SWS_FAST_BILINEAR, NULL, NULL, NULL);
                    if (!fSwsCtx) {
                        envir() << "Error: Failed to create SwsContext\n";
                        handleRetry();
                        return;
                    }
                }
                
                if (!fEncFrame) {
                    envir() << "Error: Encode frame is not initialized\n";
                    handleRetry();
                    return;
                }
                
                sws_scale(fSwsCtx, fDecFrame->data, fDecFrame->linesize, 0, fDecCtx->height,
                          fEncFrame->data, fEncFrame->linesize);
                
                fEncFrame->pts = fDecFrame->pts;
                ret = avcodec_send_frame(fEncCtx, fEncFrame);
                if (ret < 0) {
                    envir() << "Error: Failed to send frame to encoder: " << ret << "\n";
                    handleRetry();
                    return;
                }

                // 帧送入编码器后，立刻尝试拉取编码结果
                ret = avcodec_receive_packet(fEncCtx, fEncPkt);
                if (ret == 0) {
                    goto PACKET_READY;
                } else if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                    envir() << "Error: Failed to receive packet from encoder: " << ret << "\n";
                    handleRetry();
                    return;
                }
                continue; // 继续清空解码器缓存
            } else if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                envir() << "Error: Failed to receive frame from decoder: " << ret << "\n";
                handleRetry();
                return;
            }

            // 解码器为空，从文件读取新包
            AVPacket inPkt;
            ret = av_read_frame(fmtCtx, &inPkt);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    // 尝试刷新编码器
                    ret = avcodec_send_frame(fEncCtx, NULL);
                    if (ret < 0 && ret != AVERROR_EOF) {
                        envir() << "Error: Failed to flush encoder: " << ret << "\n";
                        handleClosure();
                        return;
                    }
                    
                    ret = avcodec_receive_packet(fEncCtx, fEncPkt);
                    if (ret == 0) {
                        goto PACKET_READY;
                    } else {
                        handleClosure();
                        return;
                    }
                } else {
                    envir() << "Error: Failed to read frame: " << ret << "\n";
                    handleRetry();
                    return;
                }
            }

            if (inPkt.stream_index == videoStreamIdx) {
                ret = avcodec_send_packet(fDecCtx, &inPkt);
                if (ret < 0) {
                    envir() << "Error: Failed to send packet to decoder: " << ret << "\n";
                }
            }
            av_packet_unref(&inPkt);
        }

    PACKET_READY:
        // x264 输出的数据天然是 Annex-B 格式，直接打包即可
        if (fEncPkt->size > fMaxSize) {
            fFrameSize = fMaxSize;
            fNumTruncatedBytes = fEncPkt->size - fMaxSize;
        } else {
            fFrameSize = fEncPkt->size;
            fNumTruncatedBytes = 0;
        }
        
        if (fEncPkt->data) {
            memcpy(fTo, fEncPkt->data, fFrameSize);
        } else {
            envir() << "Warning: Empty packet data\n";
            av_packet_unref(fEncPkt);
            handleRetry();
            return;
        }

        // 同步编码后的时间戳
        double ptsInSeconds = fEncPkt->pts * fTimeBase;
        fPresentationTime.tv_sec = fStartTime.tv_sec + (long)ptsInSeconds;
        fPresentationTime.tv_usec = fStartTime.tv_usec + (long)((ptsInSeconds - (long)ptsInSeconds) * 1000000.0);
        
        // 确保时间戳不为负
        if (fPresentationTime.tv_usec < 0) {
            fPresentationTime.tv_sec--;
            fPresentationTime.tv_usec += 1000000;
        }
        if (fPresentationTime.tv_sec < 0) {
            fPresentationTime.tv_sec = 0;
            fPresentationTime.tv_usec = 0;
        }

        if (fEncPkt->duration > 0) {
            fDurationInMicroseconds = (unsigned)(fEncPkt->duration * fTimeBase * 1000000.0);
        } else {
            double fps = av_q2d(fmtCtx->streams[videoStreamIdx]->avg_frame_rate);
            fDurationInMicroseconds = (unsigned)(fps > 0 ? 1000000.0 / fps : 40000);
        }

        av_packet_unref(fEncPkt);
        FramedSource::afterGetting(this);
    }

    void handleRetry() {
        nextTask() = envir().taskScheduler().scheduleDelayedTask(0, (TaskFunc*)doGetNextFrame, this);
    }

    static void doGetNextFrame(void* ptr) { ((FFmpegVideoSource*)ptr)->doGetNextFrame(); }

    std::string fFileName;
    AVFormatContext* fmtCtx = nullptr;
    AVCodecParameters* vCodecPar = nullptr;
    AVBSFContext* bsf_ctx = nullptr;
    int videoStreamIdx = -1;
    double fTimeBase = 0.0;
    struct timeval fStartTime;

    // 转码专用上下文
    bool fNeedsTranscoding = false;
    AVCodecContext* fDecCtx = nullptr;
    AVCodecContext* fEncCtx = nullptr;
    SwsContext* fSwsCtx = nullptr;
    AVFrame* fDecFrame = nullptr;
    AVFrame* fEncFrame = nullptr;
    AVPacket* fEncPkt = nullptr;
};

// ------------------------------------------------------------------
// FFmpegAudioSource: 支持全格式解码与 AAC 实时转码
// ------------------------------------------------------------------
class FFmpegAudioSource : public FramedSource {
public:
    static FFmpegAudioSource* createNew(UsageEnvironment& env, std::string const& fileName) {
        return new FFmpegAudioSource(env, fileName);
    }

protected:
    FFmpegAudioSource(UsageEnvironment& env, std::string const& fileName) 
        : FramedSource(env), fFileName(fileName) {
        initFFmpeg();
    }

    virtual ~FFmpegAudioSource() {
        if (aCodecPar) avcodec_parameters_free(&aCodecPar);
        if (swrCtx) swr_free(&swrCtx);
        
        // 释放转码相关资源
        if (fDecCtx) avcodec_free_context(&fDecCtx);
        if (fEncCtx) avcodec_free_context(&fEncCtx);
        if (fDecFrame) av_frame_free(&fDecFrame);
        if (fEncFrame) av_frame_free(&fEncFrame);
        if (fEncPkt) av_packet_free(&fEncPkt);
        
        if (fmtCtx) avformat_close_input(&fmtCtx);
    }

    virtual unsigned maxFrameSize() const { return 100000; } 

    virtual void doGetNextFrame() {
        deliverTranscodedFrame();
    }

private:
    void initFFmpeg() {
        fmtCtx = avformat_alloc_context();
        if (!fmtCtx) {
            envir() << "Error: Failed to allocate AVFormatContext\n";
            return;
        }
        
        if (avformat_open_input(&fmtCtx, fFileName.c_str(), NULL, NULL) < 0) {
            envir() << "Error: Failed to open input file: " << fFileName.c_str() << "\n";
            return;
        }
        
        if (avformat_find_stream_info(fmtCtx, NULL) < 0) {
            envir() << "Error: Failed to find stream info\n";
            avformat_close_input(&fmtCtx);
            fmtCtx = nullptr;
            return;
        }

        for (unsigned i = 0; i < fmtCtx->nb_streams; i++) {
            if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audioStreamIdx = i;
                aCodecPar = avcodec_parameters_alloc();
                if (!aCodecPar) {
                    envir() << "Error: Failed to allocate AVCodecParameters\n";
                    avformat_close_input(&fmtCtx);
                    fmtCtx = nullptr;
                    return;
                }
                avcodec_parameters_copy(aCodecPar, fmtCtx->streams[i]->codecpar);
                fTimeBase = av_q2d(fmtCtx->streams[i]->time_base);
                break;
            }
        }

        if (audioStreamIdx < 0) {
            envir() << "Error: No audio stream found\n";
            if (aCodecPar) avcodec_parameters_free(&aCodecPar);
            avformat_close_input(&fmtCtx);
            fmtCtx = nullptr;
            return;
        }

        // 无论什么音频格式，都转码为 AAC
        fNeedsTranscoding = true;
        setupTranscoder();
        
        gettimeofday(&fStartTime, NULL);
    }

    // 初始化解码器和 AAC 编码器
    void setupTranscoder() {
        // 1. 初始化解码器
        const AVCodec* dec = avcodec_find_decoder(aCodecPar->codec_id);
        if (!dec) {
            envir() << "Error: Failed to find decoder for codec ID: " << aCodecPar->codec_id << "\n";
            return;
        }
        
        fDecCtx = avcodec_alloc_context3(dec);
        if (!fDecCtx) {
            envir() << "Error: Failed to allocate decoder context\n";
            return;
        }
        
        if (avcodec_parameters_to_context(fDecCtx, aCodecPar) < 0) {
            envir() << "Error: Failed to copy codec parameters to decoder context\n";
            avcodec_free_context(&fDecCtx);
            fDecCtx = nullptr;
            return;
        }
        
        if (avcodec_open2(fDecCtx, dec, NULL) < 0) {
            envir() << "Error: Failed to open decoder\n";
            avcodec_free_context(&fDecCtx);
            fDecCtx = nullptr;
            return;
        }

        // 2. 初始化编码器 (强制转换为 AAC)
        const AVCodec* enc = avcodec_find_encoder_by_name("aac");
        if (!enc) {
            envir() << "Error: Failed to find AAC encoder\n";
            avcodec_free_context(&fDecCtx);
            fDecCtx = nullptr;
            return;
        }
        
        fEncCtx = avcodec_alloc_context3(enc);
        if (!fEncCtx) {
            envir() << "Error: Failed to allocate encoder context\n";
            avcodec_free_context(&fDecCtx);
            fDecCtx = nullptr;
            return;
        }
        
        fEncCtx->sample_rate = 48000; // 统一采样率
        fEncCtx->channels = 2; // 统一声道数
        fEncCtx->channel_layout = AV_CH_LAYOUT_STEREO;
        fEncCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        fEncCtx->time_base = {1, 48000};
        fEncCtx->bit_rate = 128000; // 固定比特率，减少波动
        
        // 极致降低延迟参数
        AVDictionary* opt = NULL;
        av_dict_set(&opt, "aac_profile", "LC", 0);
        av_dict_set(&opt, "delay", "0", 0); // 最小化延迟
        
        if (avcodec_open2(fEncCtx, enc, &opt) < 0) {
            envir() << "Error: Failed to open encoder\n";
            av_dict_free(&opt);
            avcodec_free_context(&fDecCtx);
            avcodec_free_context(&fEncCtx);
            fDecCtx = nullptr;
            fEncCtx = nullptr;
            return;
        }
        av_dict_free(&opt);

        // 3. 初始化重采样上下文
        swrCtx = swr_alloc_set_opts(NULL,
                                   AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLTP, 48000,
                                   fDecCtx->channel_layout, fDecCtx->sample_fmt, fDecCtx->sample_rate,
                                   0, NULL);
        if (!swrCtx) {
            envir() << "Error: Failed to allocate SwrContext\n";
            avcodec_free_context(&fDecCtx);
            avcodec_free_context(&fEncCtx);
            fDecCtx = nullptr;
            fEncCtx = nullptr;
            return;
        }
        
        if (swr_init(swrCtx) < 0) {
            envir() << "Error: Failed to initialize SwrContext\n";
            swr_free(&swrCtx);
            avcodec_free_context(&fDecCtx);
            avcodec_free_context(&fEncCtx);
            swrCtx = nullptr;
            fDecCtx = nullptr;
            fEncCtx = nullptr;
            return;
        }

        // 4. 分配内存
        fDecFrame = av_frame_alloc();
        if (!fDecFrame) {
            envir() << "Error: Failed to allocate decode frame\n";
            swr_free(&swrCtx);
            avcodec_free_context(&fDecCtx);
            avcodec_free_context(&fEncCtx);
            swrCtx = nullptr;
            fDecCtx = nullptr;
            fEncCtx = nullptr;
            return;
        }
        
        fEncFrame = av_frame_alloc();
        if (!fEncFrame) {
            envir() << "Error: Failed to allocate encode frame\n";
            av_frame_free(&fDecFrame);
            swr_free(&swrCtx);
            avcodec_free_context(&fDecCtx);
            avcodec_free_context(&fEncCtx);
            fDecFrame = nullptr;
            swrCtx = nullptr;
            fDecCtx = nullptr;
            fEncCtx = nullptr;
            return;
        }
        
        fEncFrame->format = fEncCtx->sample_fmt;
        fEncFrame->channels = fEncCtx->channels;
        fEncFrame->channel_layout = fEncCtx->channel_layout;
        fEncFrame->sample_rate = fEncCtx->sample_rate;
        fEncFrame->nb_samples = 1024; // AAC 标准帧大小
        
        if (av_frame_get_buffer(fEncFrame, 0) < 0) {
            envir() << "Error: Failed to allocate buffer for encode frame\n";
            av_frame_free(&fDecFrame);
            av_frame_free(&fEncFrame);
            swr_free(&swrCtx);
            avcodec_free_context(&fDecCtx);
            avcodec_free_context(&fEncCtx);
            fDecFrame = nullptr;
            fEncFrame = nullptr;
            swrCtx = nullptr;
            fDecCtx = nullptr;
            fEncCtx = nullptr;
            return;
        }
        
        fEncPkt = av_packet_alloc();
        if (!fEncPkt) {
            envir() << "Error: Failed to allocate encode packet\n";
            av_frame_free(&fDecFrame);
            av_frame_free(&fEncFrame);
            swr_free(&swrCtx);
            avcodec_free_context(&fDecCtx);
            avcodec_free_context(&fEncCtx);
            fDecFrame = nullptr;
            fEncFrame = nullptr;
            swrCtx = nullptr;
            fDecCtx = nullptr;
            fEncCtx = nullptr;
            return;
        }
    }

    // 转码模式：处理所有音频格式，统一转码为 AAC
    void deliverTranscodedFrame() {
        if (!isCurrentlyAwaitingData()) return;

        if (!fmtCtx || !fDecCtx || !fEncCtx || !fEncPkt || !fEncFrame || !swrCtx) {
            handleClosure();
            return;
        }

        // 限制每帧处理时间，避免阻塞事件循环
        struct timeval start_time, current_time;
        gettimeofday(&start_time, NULL);
        const long MAX_PROCESSING_TIME_US = 20000; // 20ms，音频处理通常更快

        // 状态机：首先尝试从编码器拉取上一轮可能缓存的包
        int ret = avcodec_receive_packet(fEncCtx, fEncPkt);
        if (ret == 0) {
            goto PACKET_READY;
        } else if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            envir() << "Error: Failed to receive packet from encoder: " << ret << "\n";
            handleRetry();
            return;
        }

        while (true) {
            // 检查处理时间，避免阻塞事件循环
            gettimeofday(&current_time, NULL);
            long processing_time = (current_time.tv_sec - start_time.tv_sec) * 1000000 + 
                                 (current_time.tv_usec - start_time.tv_usec);
            if (processing_time > MAX_PROCESSING_TIME_US) {
                // 处理时间过长，将剩余工作交给下一次调用
                handleRetry();
                return;
            }

            // 尝试从解码器拉取已解码的帧
            ret = avcodec_receive_frame(fDecCtx, fDecFrame);
            if (ret == 0) {
                // 重采样到 AAC 所需的格式
                int swr_ret = swr_convert(swrCtx, fEncFrame->data, fEncFrame->nb_samples,
                           (const uint8_t**)fDecFrame->data, fDecFrame->nb_samples);
                if (swr_ret < 0) {
                    envir() << "Error: Failed to convert audio samples: " << swr_ret << "\n";
                    handleRetry();
                    return;
                }
                
                fEncFrame->pts = fDecFrame->pts;
                ret = avcodec_send_frame(fEncCtx, fEncFrame);
                if (ret < 0) {
                    envir() << "Error: Failed to send frame to encoder: " << ret << "\n";
                    handleRetry();
                    return;
                }

                // 帧送入编码器后，立刻尝试拉取编码结果
                ret = avcodec_receive_packet(fEncCtx, fEncPkt);
                if (ret == 0) {
                    goto PACKET_READY;
                } else if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                    envir() << "Error: Failed to receive packet from encoder: " << ret << "\n";
                    handleRetry();
                    return;
                }
                continue; // 继续清空解码器缓存
            } else if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                envir() << "Error: Failed to receive frame from decoder: " << ret << "\n";
                handleRetry();
                return;
            }

            // 解码器为空，从文件读取新包
            AVPacket inPkt;
            ret = av_read_frame(fmtCtx, &inPkt);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    // 尝试刷新编码器
                    ret = avcodec_send_frame(fEncCtx, NULL);
                    if (ret < 0 && ret != AVERROR_EOF) {
                        envir() << "Error: Failed to flush encoder: " << ret << "\n";
                        handleClosure();
                        return;
                    }
                    
                    ret = avcodec_receive_packet(fEncCtx, fEncPkt);
                    if (ret == 0) {
                        goto PACKET_READY;
                    } else {
                        handleClosure();
                        return;
                    }
                } else {
                    envir() << "Error: Failed to read frame: " << ret << "\n";
                    handleRetry();
                    return;
                }
            }

            if (inPkt.stream_index == audioStreamIdx) {
                ret = avcodec_send_packet(fDecCtx, &inPkt);
                if (ret < 0) {
                    envir() << "Error: Failed to send packet to decoder: " << ret << "\n";
                }
            }
            av_packet_unref(&inPkt);
        }

    PACKET_READY:
        if (fEncPkt->size > fMaxSize) {
            fFrameSize = fMaxSize;
            fNumTruncatedBytes = fEncPkt->size - fMaxSize;
        } else {
            fFrameSize = fEncPkt->size;
            fNumTruncatedBytes = 0;
        }
        
        if (fEncPkt->data) {
            memcpy(fTo, fEncPkt->data, fFrameSize);
        } else {
            envir() << "Warning: Empty packet data\n";
            av_packet_unref(fEncPkt);
            handleRetry();
            return;
        }

        // 同步编码后的时间戳
        double ptsInSeconds = fEncPkt->pts * fTimeBase;
        fPresentationTime.tv_sec = fStartTime.tv_sec + (long)ptsInSeconds;
        fPresentationTime.tv_usec = fStartTime.tv_usec + (long)((ptsInSeconds - (long)ptsInSeconds) * 1000000.0);
        
        // 确保时间戳不为负
        if (fPresentationTime.tv_usec < 0) {
            fPresentationTime.tv_sec--;
            fPresentationTime.tv_usec += 1000000;
        }
        if (fPresentationTime.tv_sec < 0) {
            fPresentationTime.tv_sec = 0;
            fPresentationTime.tv_usec = 0;
        }

        // AAC 帧持续时间
        fDurationInMicroseconds = (unsigned)(1000000.0 * fEncFrame->nb_samples / fEncCtx->sample_rate);

        av_packet_unref(fEncPkt);
        FramedSource::afterGetting(this);
    }

    void handleRetry() {
        nextTask() = envir().taskScheduler().scheduleDelayedTask(0, (TaskFunc*)doGetNextFrame, this);
    }

    static void doGetNextFrame(void* ptr) { ((FFmpegAudioSource*)ptr)->doGetNextFrame(); }

    std::string fFileName;
    AVFormatContext* fmtCtx = nullptr;
    AVCodecParameters* aCodecPar = nullptr;
    int audioStreamIdx = -1;
    double fTimeBase = 0.0;
    struct timeval fStartTime;

    // 转码专用上下文
    bool fNeedsTranscoding = false;
    AVCodecContext* fDecCtx = nullptr;
    AVCodecContext* fEncCtx = nullptr;
    struct SwrContext* swrCtx = nullptr;
    AVFrame* fDecFrame = nullptr;
    AVFrame* fEncFrame = nullptr;
    AVPacket* fEncPkt = nullptr;
};

// ------------------------------------------------------------------
// DynamicVideoSubsession: 视频流专用传输参数
// ------------------------------------------------------------------
class DynamicVideoSubsession : public OnDemandServerMediaSubsession {
public:
    static DynamicVideoSubsession* createNew(UsageEnvironment& env, std::string const& fileName, bool isH265) {
        return new DynamicVideoSubsession(env, fileName, isH265);
    }

protected:
    DynamicVideoSubsession(UsageEnvironment& env, std::string const& fileName, bool isH265)
        : OnDemandServerMediaSubsession(env, True), fFileName(fileName), fIsH265(isH265) {}

    virtual FramedSource* createNewStreamSource(unsigned, unsigned& estBitrate) {
        estBitrate = 50000; 
        FFmpegVideoSource* baseSource = FFmpegVideoSource::createNew(envir(), fFileName);
        
        // 如果文件是原生 H265，走 H265Framer；如果是转码，输出恒定为 H264
        if (fIsH265) return H265VideoStreamFramer::createNew(envir(), baseSource);
        return H264VideoStreamFramer::createNew(envir(), baseSource);
    }

    virtual RTPSink* createNewRTPSink(Groupsock* rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic, FramedSource*) {
        setSendBufferTo(envir(), rtpGroupsock->socketNum(), 8 * 1024 * 1024); 
        
        if (fIsH265) return H265VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic);
        return H264VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic);
    }

private:
    std::string fFileName;
    bool fIsH265;
};

// ------------------------------------------------------------------
// DynamicAudioSubsession: 音频流专用传输参数
// ------------------------------------------------------------------
class DynamicAudioSubsession : public OnDemandServerMediaSubsession {
public:
    static DynamicAudioSubsession* createNew(UsageEnvironment& env, std::string const& fileName) {
        return new DynamicAudioSubsession(env, fileName);
    }

protected:
    DynamicAudioSubsession(UsageEnvironment& env, std::string const& fileName)
        : OnDemandServerMediaSubsession(env, True), fFileName(fileName) {}

    virtual FramedSource* createNewStreamSource(unsigned, unsigned& estBitrate) {
        estBitrate = 128000; // AAC 比特率
        return FFmpegAudioSource::createNew(envir(), fFileName);
    }

    virtual RTPSink* createNewRTPSink(Groupsock* rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic, FramedSource*) {
        setSendBufferTo(envir(), rtpGroupsock->socketNum(), 2 * 1024 * 1024);
        return MPEG4GenericRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic, 48000, "audio", "AAC-hbr", "LC", 2);
    }

private:
    std::string fFileName;
};

// 辅助函数：检查文件扩展名
bool isSupportedMediaExtension(std::string const& filename) {
    static const std::vector<std::string> extensions = {
        // 视频格式
        ".mp4", ".mkv", ".flv", ".avi", ".mov", ".wmv", ".ts", ".264", ".265", ".h264", ".h265",
        ".mpg", ".mpeg", ".m4v", ".webm", ".ogv", ".rmvb", ".3gp", ".3g2",
        // 音频格式
        ".mp3", ".wav", ".aac", ".ogg", ".flac", ".wma", ".m4a", ".opus", ".ac3", ".dts"
    };
    std::string lowerFilename = filename;
    std::transform(lowerFilename.begin(), lowerFilename.end(), lowerFilename.begin(), ::tolower);
    
    for (auto const& ext : extensions) {
        if (lowerFilename.length() >= ext.length() &&
            lowerFilename.compare(lowerFilename.length() - ext.length(), ext.length(), ext) == 0) {
            return true;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    OutPacketBuffer::maxSize = 10000000; 

    TaskScheduler* scheduler = BasicTaskScheduler::createNew();
    UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

    RTSPServer* rtspServer = RTSPServer::createNew(*env, 8558);
    if (!rtspServer) {
        *env << "Error: " << env->getResultMsg() << "\n";
        return 1;
    }

    DIR* dir = opendir(".");
    struct dirent* entry;
    *env << "--- FFmpeg + Live555 (All-Format Transcoding Engine) ---\n";

    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_REG) continue;
        std::string filename = entry->d_name;

        // 1. 初筛：检查后缀名，跳过 CMakeCache.txt、编译产物等非媒体文件
        if (!isSupportedMediaExtension(filename)) continue;

        AVFormatContext* tempCtx = avformat_alloc_context();
        
        // 2. 深筛：尝试打开并探测流信息
        if (avformat_open_input(&tempCtx, filename.c_str(), NULL, NULL) == 0) {
            // 适当增加探测时间，防止某些格式（如 m4v）识别不准
            tempCtx->probesize = 5000000; 
            tempCtx->max_analyze_duration = 2000000;

            if (avformat_find_stream_info(tempCtx, NULL) >= 0) {
                bool hasValidVideo = false;
                bool hasValidAudio = false;
                bool isNativeH265 = false;
                
                for (unsigned i = 0; i < tempCtx->nb_streams; i++) {
                    AVCodecParameters* par = tempCtx->streams[i]->codecpar;
                    if (par->codec_type == AVMEDIA_TYPE_VIDEO && par->codec_id != AV_CODEC_ID_NONE) {
                        // 额外过滤：如果宽度或高度为0，说明探测失败，不发布
                        if (par->width > 0 && par->height > 0) {
                            hasValidVideo = true;
                            isNativeH265 = (par->codec_id == AV_CODEC_ID_HEVC);
                        }
                    } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && par->codec_id != AV_CODEC_ID_NONE) {
                        hasValidAudio = true;
                    }
                }

                if (hasValidVideo || hasValidAudio) {
                    ServerMediaSession* sms = ServerMediaSession::createNew(*env, filename.c_str(), filename.c_str(), "Universal Stream");
                    
                    // 添加视频流
                    if (hasValidVideo) {
                        sms->addSubsession(DynamicVideoSubsession::createNew(*env, filename, isNativeH265));
                    }
                    
                    // 添加音频流
                    if (hasValidAudio) {
                        sms->addSubsession(DynamicAudioSubsession::createNew(*env, filename));
                    }
                    
                    rtspServer->addServerMediaSession(sms);
                    char* url = rtspServer->rtspURL(sms);
                    *env << "[Published] " << url << "\n";
                    delete[] url;
                } else {
                    // 如果是损坏的媒体文件，这里会被过滤掉
                }
            }
            avformat_close_input(&tempCtx);
        }
    }
    closedir(dir);
    
    *env << "Server started. Waiting for connections...\n";
    env->taskScheduler().doEventLoop();
    return 0;
}