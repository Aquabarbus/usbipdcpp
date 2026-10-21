#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "StreamAudioBuffer.h"
#include "usbipdcpp/virtual_device/video_sources/VideoSource.h"

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace usbipdcpp {

/// 打开网络流（rtsp 走 TCP、5 秒无数据超时；stop 置位时打断阻塞读）。失败返回 nullptr
AVFormatContext *open_stream_input(const std::string &url, std::atomic<bool> *stop);

/// 网络流音轨解码器：解码 + swresample 到 StreamAudioBuffer 当前协商格式后 push。
/// 每次连接调用 open，随后对每个包调用 decode
class StreamAudioDecoder {
public:
    explicit StreamAudioDecoder(std::shared_ptr<StreamAudioBuffer> buffer);
    ~StreamAudioDecoder();

    /// 在已 find_stream_info 的流中找最佳音轨并打开解码器；没有音轨返回 false
    bool open(AVFormatContext *fmt);
    int stream() const {
        return stream_;
    }
    void decode(const AVPacket *pkt);

private:
    void close();

    std::shared_ptr<StreamAudioBuffer> buffer_;
    AVCodecContext *dec_ = nullptr;
    SwrContext *swr_ = nullptr;
    AVFrame *frm_ = nullptr;
    int stream_ = -1;
    // 当前 swr 的输入/输出参数（任一变化即重建）
    int in_rate_ = 0;
    int in_format_ = -1;
    int in_channels_ = 0;
    std::uint32_t out_rate_ = 0;
    std::uint16_t out_channels_ = 0;
    std::vector<std::int16_t> out_;
};

/// IP 摄像机视频源（FFmpeg 能打开的任意 URL：rtsp://、http:// MJPEG 等）。
/// 后台线程拉流解码并缩放为固定 WxH YUY2，get_frame 总是返回最新一帧
/// （摄像机帧率与 UVC 帧率不同也不累积延迟）；未连上时输出黑帧，断流自动重连
class IpCameraSource : public VideoSource {
public:
    /// @param audio 非空时同一条流里的音轨送入该缓冲（--mic url）
    IpCameraSource(std::string url, std::uint16_t width, std::uint16_t height, std::uint8_t fps,
                   std::shared_ptr<StreamAudioBuffer> audio = nullptr);
    ~IpCameraSource() override;

    std::vector<VideoFormatInfo> supported_formats() const override;
    VideoFormatInfo current_format() const override;
    bool set_format(std::uint32_t fourcc, std::uint16_t width, std::uint16_t height,
                    std::uint32_t frame_interval) override;
    bool get_frame(VideoFrame &frame) override;
    std::size_t max_frame_size() const override;
    std::uint32_t frame_interval() const override;

private:
    void run();
    void stream_once();

    std::string url_;
    std::shared_ptr<StreamAudioBuffer> audio_;
    std::uint16_t width_;
    std::uint16_t height_;
    std::uint32_t frame_interval_;

    std::vector<std::uint8_t> latest_; // 解码线程写入的最新帧（mutex_ 保护）
    std::vector<std::uint8_t> front_; // get_frame 返回给 UVC 的副本
    std::mutex mutex_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

/// 独立音频流（--audio-url，如 IP Webcam 的 /audio.wav）：后台线程拉流解码送入缓冲，断流 2 秒后重连
class IpAudioStream {
public:
    IpAudioStream(std::string url, std::shared_ptr<StreamAudioBuffer> buffer);
    ~IpAudioStream();

private:
    void run();
    void stream_once();

    std::string url_;
    std::shared_ptr<StreamAudioBuffer> buffer_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace usbipdcpp
