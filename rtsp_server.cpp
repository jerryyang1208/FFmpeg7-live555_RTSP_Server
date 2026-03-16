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
        if (avformat_open_input(&fmtCtx, fFileName.c_str(), NULL, NULL) < 0) return;
        if (avformat_find_stream_info(fmtCtx, NULL) < 0) return;

        for (unsigned i = 0; i < fmtCtx->nb_streams; i++) {
            if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoStreamIdx = i;
                vCodecPar = avcodec_parameters_alloc();
                avcodec_parameters_copy(vCodecPar, fmtCtx->streams[i]->codecpar);
                fTimeBase = av_q2d(fmtCtx->streams[i]->time_base);
                break;
            }
        }

        if (videoStreamIdx < 0) return;

        // 判断是否需要转码
        if (vCodecPar->codec_id == AV_CODEC_ID_H264 || vCodecPar->codec_id == AV_CODEC_ID_HEVC) {
            fNeedsTranscoding = false;
            const AVBitStreamFilter* filter = (vCodecPar->codec_id == AV_CODEC_ID_HEVC) ? 
                av_bsf_get_by_name("hevc_mp4toannexb") : av_bsf_get_by_name("h264_mp4toannexb");
            if (filter) {
                av_bsf_alloc(filter, &bsf_ctx);
                avcodec_parameters_copy(bsf_ctx->par_in, vCodecPar);
                av_bsf_init(bsf_ctx);
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
        fDecCtx = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(fDecCtx, vCodecPar);
        avcodec_open2(fDecCtx, dec, NULL);

        // 2. 初始化编码器 (强制转换为 H.264)
        const AVCodec* enc = avcodec_find_encoder_by_name("libx264");
        fEncCtx = avcodec_alloc_context3(enc);
        fEncCtx->width = fDecCtx->width;
        fEncCtx->height = fDecCtx->height;
        fEncCtx->time_base = fmtCtx->streams[videoStreamIdx]->time_base; // 继承原始时基
        fEncCtx->framerate = av_guess_frame_rate(fmtCtx, fmtCtx->streams[videoStreamIdx], NULL);
        fEncCtx->pix_fmt = AV_PIX_FMT_YUV420P;
        
        // 极致降低延迟参数，防止阻塞 Live555 单线程
        AVDictionary* opt = NULL;
        av_dict_set(&opt, "preset", "ultrafast", 0);
        av_dict_set(&opt, "tune", "zerolatency", 0);
        avcodec_open2(fEncCtx, enc, &opt);
        av_dict_free(&opt);

        // 3. 分配内存
        fDecFrame = av_frame_alloc();
        fEncFrame = av_frame_alloc();
        fEncFrame->format = fEncCtx->pix_fmt;
        fEncFrame->width = fEncCtx->width;
        fEncFrame->height = fEncCtx->height;
        av_frame_get_buffer(fEncFrame, 0);
        
        fEncPkt = av_packet_alloc();
    }

    // 直通模式：处理原生的 H.264 / H.265
    void deliverDirectFrame() {
        if (!isCurrentlyAwaitingData()) return;

        AVPacket pkt;
        if (av_read_frame(fmtCtx, &pkt) >= 0) {
            if (pkt.stream_index == videoStreamIdx) {
                if (bsf_ctx) {
                    av_bsf_send_packet(bsf_ctx, &pkt);
                    if (av_bsf_receive_packet(bsf_ctx, &pkt) != 0) {
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
                memcpy(fTo, pkt.data, fFrameSize);

                double ptsInSeconds = pkt.pts * fTimeBase;
                fPresentationTime.tv_sec = fStartTime.tv_sec + (long)ptsInSeconds;
                fPresentationTime.tv_usec = fStartTime.tv_usec + (long)((ptsInSeconds - (long)ptsInSeconds) * 1000000.0);

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
        } else {
            handleClosure();
        }
    }

    // 转码模式：处理 VP9, AV1, MPEG4 等
    void deliverTranscodedFrame() {
        if (!isCurrentlyAwaitingData()) return;

        // 状态机：首先尝试从编码器拉取上一轮可能缓存的包
        if (avcodec_receive_packet(fEncCtx, fEncPkt) == 0) {
            goto PACKET_READY;
        }

        while (true) {
            // 尝试从解码器拉取已解码的帧
            if (avcodec_receive_frame(fDecCtx, fDecFrame) == 0) {
                // 如果像素格式不符合 H.264 要求的 YUV420P，进行转换
                if (!fSwsCtx) {
                    // 【已修正】：将 width, height, pix_fmt 的参数顺序调整为正确顺序
                    fSwsCtx = sws_getContext(fDecCtx->width, fDecCtx->height, fDecCtx->pix_fmt,
                                             fEncCtx->width, fEncCtx->height, fEncCtx->pix_fmt,
                                             SWS_FAST_BILINEAR, NULL, NULL, NULL);
                }
                sws_scale(fSwsCtx, fDecFrame->data, fDecFrame->linesize, 0, fDecCtx->height,
                          fEncFrame->data, fEncFrame->linesize);
                
                fEncFrame->pts = fDecFrame->pts;
                avcodec_send_frame(fEncCtx, fEncFrame);

                // 帧送入编码器后，立刻尝试拉取编码结果
                if (avcodec_receive_packet(fEncCtx, fEncPkt) == 0) {
                    goto PACKET_READY;
                }
                continue; // 继续清空解码器缓存
            }

            // 解码器为空，从文件读取新包
            AVPacket inPkt;
            if (av_read_frame(fmtCtx, &inPkt) < 0) {
                handleClosure();
                return;
            }

            if (inPkt.stream_index == videoStreamIdx) {
                avcodec_send_packet(fDecCtx, &inPkt);
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
        memcpy(fTo, fEncPkt->data, fFrameSize);

        // 同步编码后的时间戳
        double ptsInSeconds = fEncPkt->pts * fTimeBase;
        fPresentationTime.tv_sec = fStartTime.tv_sec + (long)ptsInSeconds;
        fPresentationTime.tv_usec = fStartTime.tv_usec + (long)((ptsInSeconds - (long)ptsInSeconds) * 1000000.0);

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
// DynamicSubsession: 强化传输参数
// ------------------------------------------------------------------
class DynamicFFmpegSubsession : public OnDemandServerMediaSubsession {
public:
    static DynamicFFmpegSubsession* createNew(UsageEnvironment& env, std::string const& fileName, bool isH265) {
        return new DynamicFFmpegSubsession(env, fileName, isH265);
    }

protected:
    DynamicFFmpegSubsession(UsageEnvironment& env, std::string const& fileName, bool isH265)
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

// 辅助函数：检查文件扩展名
bool isSupportedVideoExtension(std::string const& filename) {
    static const std::vector<std::string> extensions = {
        ".mp4", ".mkv", ".flv", ".avi", ".mov", ".wmv", ".ts", ".264", ".265", ".h264", ".h265"
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

    RTSPServer* rtspServer = RTSPServer::createNew(*env, 8554);
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
        if (!isSupportedVideoExtension(filename)) continue;

        AVFormatContext* tempCtx = avformat_alloc_context();
        
        // 2. 深筛：尝试打开并探测流信息
        if (avformat_open_input(&tempCtx, filename.c_str(), NULL, NULL) == 0) {
            // 适当增加探测时间，防止某些格式（如 m4v）识别不准
            tempCtx->probesize = 5000000; 
            tempCtx->max_analyze_duration = 2000000;

            if (avformat_find_stream_info(tempCtx, NULL) >= 0) {
                bool hasValidVideo = false;
                bool isNativeH265 = false;
                
                for (unsigned i = 0; i < tempCtx->nb_streams; i++) {
                    AVCodecParameters* par = tempCtx->streams[i]->codecpar;
                    // 必须是视频流，且编码器 ID 合法
                    if (par->codec_type == AVMEDIA_TYPE_VIDEO && par->codec_id != AV_CODEC_ID_NONE) {
                        // 额外过滤：如果宽度或高度为0，说明探测失败，不发布
                        if (par->width > 0 && par->height > 0) {
                            hasValidVideo = true;
                            isNativeH265 = (par->codec_id == AV_CODEC_ID_HEVC);
                            break;
                        }
                    }
                }

                if (hasValidVideo) {
                    ServerMediaSession* sms = ServerMediaSession::createNew(*env, filename.c_str(), filename.c_str(), "Universal Stream");
                    sms->addSubsession(DynamicFFmpegSubsession::createNew(*env, filename, isNativeH265));
                    rtspServer->addServerMediaSession(sms);
                    char* url = rtspServer->rtspURL(sms);
                    *env << "[Published] " << url << "\n";
                    delete[] url;
                } else {
                    // 如果是音频文件或损坏的视频，这里会被过滤掉
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