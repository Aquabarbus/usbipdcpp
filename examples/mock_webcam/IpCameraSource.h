#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "usbipdcpp/virtual_device/video_sources/VideoSource.h"

namespace usbipdcpp {

/// IP 摄像机视频源（FFmpeg 能打开的任意 URL：rtsp://、http:// MJPEG 等）。
/// 后台线程拉流解码并缩放为固定 WxH YUY2，get_frame 总是返回最新一帧
/// （摄像机帧率与 UVC 帧率不同也不累积延迟）；未连上时输出黑帧，断流自动重连
class IpCameraSource : public VideoSource {
public:
    IpCameraSource(std::string url, std::uint16_t width, std::uint16_t height, std::uint8_t fps);
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
    std::uint16_t width_;
    std::uint16_t height_;
    std::uint32_t frame_interval_;

    std::vector<std::uint8_t> latest_; // 解码线程写入的最新帧（mutex_ 保护）
    std::vector<std::uint8_t> front_; // get_frame 返回给 UVC 的副本
    std::mutex mutex_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace usbipdcpp
