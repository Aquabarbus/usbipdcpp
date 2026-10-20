#include "IpCameraSource.h"

#include <chrono>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include "usbipdcpp/virtual_device/UvcConstants.h"

namespace usbipdcpp {

IpCameraSource::IpCameraSource(std::string url, std::uint16_t width, std::uint16_t height, std::uint8_t fps) :
    url_(std::move(url)), width_(width), height_(height), frame_interval_(10'000'000U / fps) {
    // 黑帧（Y=16, U=V=128），连上摄像机前输出
    latest_.resize(static_cast<std::size_t>(width_) * height_ * 2);
    for (std::size_t i = 0; i < latest_.size(); i += 2) {
        latest_[i] = 16;
        latest_[i + 1] = 128;
    }
    front_ = latest_;
    thread_ = std::thread([this] { run(); });
}

IpCameraSource::~IpCameraSource() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}

std::vector<VideoFormatInfo> IpCameraSource::supported_formats() const {
    // 帧间隔约束同 ColorBarSource：(max - min) % min == 0
    return {{UvcFourCC::YUY2, width_, height_, static_cast<std::uint32_t>(width_ * height_ * 2), frame_interval_,
             frame_interval_, frame_interval_ * 10, 16}};
}

VideoFormatInfo IpCameraSource::current_format() const {
    return supported_formats().front();
}

bool IpCameraSource::set_format(std::uint32_t fourcc, std::uint16_t width, std::uint16_t height,
                                std::uint32_t frame_interval) {
    if (fourcc != UvcFourCC::YUY2 || width != width_ || height != height_)
        return false;
    frame_interval_ = frame_interval;
    return true;
}

bool IpCameraSource::get_frame(VideoFrame &frame) {
    {
        std::lock_guard lock(mutex_);
        front_ = latest_;
    }
    frame.data = front_.data();
    frame.size = front_.size();
    frame.is_keyframe = true;
    return true;
}

std::size_t IpCameraSource::max_frame_size() const {
    return static_cast<std::size_t>(width_) * height_ * 2;
}

std::uint32_t IpCameraSource::frame_interval() const {
    return frame_interval_;
}

void IpCameraSource::run() {
    while (!stop_) {
        stream_once();
        // 断流/连接失败：2 秒后重连（分段等待以便及时响应退出）
        for (int i = 0; i < 20 && !stop_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void IpCameraSource::stream_once() {
    AVFormatContext *fmt = avformat_alloc_context();
    // 退出时打断阻塞的网络读
    fmt->interrupt_callback.callback = [](void *self) {
        return static_cast<IpCameraSource *>(self)->stop_.load() ? 1 : 0;
    };
    fmt->interrupt_callback.opaque = this;

    AVDictionary *opts = nullptr;
    if (url_.rfind("rtsp://", 0) == 0)
        av_dict_set(&opts, "rtsp_transport", "tcp", 0); // TCP 不丢包，loopback/局域网足够
    av_dict_set(&opts, "timeout", "5000000", 0); // 5 秒无数据视为断流（微秒）
    int ret = avformat_open_input(&fmt, url_.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        SPDLOG_WARN("IP camera: cannot open {} (error {}), retrying", url_, ret);
        return; // 失败时 avformat_open_input 已释放 fmt
    }

    const AVCodec *codec = nullptr;
    AVCodecContext *dec = nullptr;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm = av_frame_alloc();
    SwsContext *sws = nullptr;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width_) * height_ * 2);
    int stream = -1;

    if (avformat_find_stream_info(fmt, nullptr) >= 0)
        stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream >= 0 && (dec = avcodec_alloc_context3(codec)) != nullptr &&
        avcodec_parameters_to_context(dec, fmt->streams[stream]->codecpar) >= 0 &&
        avcodec_open2(dec, codec, nullptr) >= 0) {
        SPDLOG_INFO("IP camera: connected {} — {} {}x{} → YUY2 {}x{}", url_, codec->name, dec->width, dec->height,
                    width_, height_);
        while (!stop_ && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == stream && avcodec_send_packet(dec, pkt) >= 0) {
                while (avcodec_receive_frame(dec, frm) >= 0) {
                    // 摄像机中途改分辨率/像素格式时 sws_getCachedContext 自动重建
                    sws = sws_getCachedContext(sws, frm->width, frm->height, static_cast<AVPixelFormat>(frm->format),
                                               width_, height_, AV_PIX_FMT_YUYV422, SWS_BILINEAR, nullptr, nullptr,
                                               nullptr);
                    if (!sws)
                        continue;
                    std::uint8_t *dst[4] = {out.data(), nullptr, nullptr, nullptr};
                    int dst_linesize[4] = {width_ * 2, 0, 0, 0};
                    sws_scale(sws, frm->data, frm->linesize, 0, frm->height, dst, dst_linesize);
                    std::lock_guard lock(mutex_);
                    latest_.swap(out);
                }
            }
            av_packet_unref(pkt);
        }
        if (!stop_)
            SPDLOG_WARN("IP camera: stream {} ended, reconnecting", url_);
    }
    else {
        SPDLOG_WARN("IP camera: no decodable video stream in {}, retrying", url_);
    }

    sws_freeContext(sws);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
}

} // namespace usbipdcpp
