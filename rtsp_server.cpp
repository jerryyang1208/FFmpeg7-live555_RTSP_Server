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
#include <libavcodec/bsf.h>
}

// Live555
#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include "GroupsockHelper.hh"

// ------------------------------------------------------------------
// FFmpegVideoSource: 解决截断、同步与 NALU 完整性问题
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
        if (fmtCtx) avformat_close_input(&fmtCtx);
    }

    // 限制单次读取大小，Live555 的默认缓冲区通常较小
    virtual unsigned maxFrameSize() const { return 500000; } 

    virtual void doGetNextFrame() {
        deliverFrame();
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
                // 获取时基，用于精准同步
                fTimeBase = av_q2d(fmtCtx->streams[i]->time_base);
                break;
            }
        }

        if (videoStreamIdx < 0) return;

        const AVBitStreamFilter* filter = (vCodecPar->codec_id == AV_CODEC_ID_HEVC) ? 
            av_bsf_get_by_name("hevc_mp4toannexb") : av_bsf_get_by_name("h264_mp4toannexb");
        
        if (filter) {
            av_bsf_alloc(filter, &bsf_ctx);
            avcodec_parameters_copy(bsf_ctx->par_in, vCodecPar);
            av_bsf_init(bsf_ctx);
        }
        
        gettimeofday(&fStartTime, NULL);
    }

    void deliverFrame() {
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
                
                // --- 动态缓冲区安全检查，记录剩余数据或提示缓冲区不足 ---
                if (pkt.size > fMaxSize) {
                    fFrameSize = fMaxSize;
                    fNumTruncatedBytes = pkt.size - fMaxSize;
                } else {
                    fFrameSize = pkt.size;
                    fNumTruncatedBytes = 0;
                }

                memcpy(fTo, pkt.data, fFrameSize);

                // 将 FFmpeg 的 PTS 转换为相对系统时间，解决播放速度不稳问题
                double ptsInSeconds = pkt.pts * fTimeBase;
                fPresentationTime.tv_sec = fStartTime.tv_sec + (long)ptsInSeconds;
                fPresentationTime.tv_usec = fStartTime.tv_usec + (long)((ptsInSeconds - (long)ptsInSeconds) * 1000000.0);
                if (fPresentationTime.tv_usec >= 1000000) {
                    fPresentationTime.tv_sec++;
                    fPresentationTime.tv_usec -= 1000000;
                }

                // 注入 Duration (如果可用)，帮助 Framer 更好工作
                fDurationInMicroseconds = (unsigned)(av_q2d(fmtCtx->streams[videoStreamIdx]->avg_frame_rate) > 0 ? 
                                           1000000.0 / av_q2d(fmtCtx->streams[videoStreamIdx]->avg_frame_rate) : 0);

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
        // 设置一个较高的估计码率，防止 Live555 限制带宽导致的丢包
        estBitrate = 15000; 
        FFmpegVideoSource* baseSource = FFmpegVideoSource::createNew(envir(), fFileName);
        
        // 使用 ByteStreamMemoryBufferSource 思想的 Framer 能更好地处理 NALU
        if (fIsH265) return H265VideoStreamFramer::createNew(envir(), baseSource);
        return H264VideoStreamFramer::createNew(envir(), baseSource);
    }

    virtual RTPSink* createNewRTPSink(Groupsock* rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic, FramedSource*) {
        // --- 进一步提升 Socket 缓冲区 ---
        setSendBufferTo(envir(), rtpGroupsock->socketNum(), 4 * 1024 * 1024); 
        
        if (fIsH265) return H265VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic);
        return H264VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic);
    }

private:
    std::string fFileName;
    bool fIsH265;
};

// ------------------------------------------------------------------
// Main Entry: 提升全局缓冲区
// ------------------------------------------------------------------
int main(int argc, char** argv) {
    // --- 调大 Live555 内部全局发送缓冲区提升到 10MB ---
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
    *env << "--- FFmpeg + Live555 稳定性强化版 (Anti-Artifacts) ---\n";

    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_REG) continue;
        std::string filename = entry->d_name;

        // 严格过滤非媒体文件
        if (filename.find(".mp4") == std::string::npos && 
            filename.find(".flv") == std::string::npos &&
            filename.find(".264") == std::string::npos) continue;

        AVFormatContext* tempCtx = avformat_alloc_context();
        if (avformat_open_input(&tempCtx, filename.c_str(), NULL, NULL) == 0) {
            if (avformat_find_stream_info(tempCtx, NULL) >= 0) {
                bool isH265 = false, supported = false;
                for (unsigned i = 0; i < tempCtx->nb_streams; i++) {
                    AVCodecID cid = tempCtx->streams[i]->codecpar->codec_id;
                    if (cid == AV_CODEC_ID_H264 || cid == AV_CODEC_ID_HEVC) {
                        isH265 = (cid == AV_CODEC_ID_HEVC);
                        supported = true;
                        break;
                    }
                }

                if (supported) {
                    ServerMediaSession* sms = ServerMediaSession::createNew(*env, filename.c_str(), filename.c_str(), "RTSP Stable Stream");
                    sms->addSubsession(DynamicFFmpegSubsession::createNew(*env, filename, isH265));
                    rtspServer->addServerMediaSession(sms);
                    char* url = rtspServer->rtspURL(sms);
                    *env << "[Published] " << url << "\n";
                    delete[] url;
                }
            }
            avformat_close_input(&tempCtx);
        }
    }
    closedir(dir);
    env->taskScheduler().doEventLoop();
    return 0;
}