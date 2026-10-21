#include "IpCameraSource.h"

#include <chrono>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "usbipdcpp/virtual_device/UvcConstants.h"

namespace usbipdcpp {

AVFormatContext *open_stream_input(const std::string &url, std::atomic<bool> *stop) {
    AVFormatContext *fmt = avformat_alloc_context();
    // 退出时打断阻塞的网络读
    fmt->interrupt_callback.callback = [](void *flag) {
        return static_cast<std::atomic<bool> *>(flag)->load() ? 1 : 0;
    };
    fmt->interrupt_callback.opaque = stop;

    AVDictionary *opts = nullptr;
    if (url.rfind("rtsp://", 0) == 0)
        av_dict_set(&opts, "rtsp_transport", "tcp", 0); // TCP 不丢包，loopback/局域网足够
    av_dict_set(&opts, "timeout", "5000000", 0); // 5 秒无数据视为断流（微秒）
    int ret = avformat_open_input(&fmt, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        SPDLOG_WARN("Stream: cannot open {} (error {}), retrying", url, ret);
        return nullptr; // 失败时 avformat_open_input 已释放 fmt
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        SPDLOG_WARN("Stream: no stream info in {}, retrying", url);
        avformat_close_input(&fmt);
        return nullptr;
    }
    return fmt;
}

// ========== StreamAudioDecoder ==========

StreamAudioDecoder::StreamAudioDecoder(std::shared_ptr<StreamAudioBuffer> buffer) : buffer_(std::move(buffer)) {}

StreamAudioDecoder::~StreamAudioDecoder() {
    close();
}

void StreamAudioDecoder::close() {
    swr_free(&swr_);
    av_frame_free(&frm_);
    avcodec_free_context(&dec_);
    stream_ = -1;
    in_rate_ = 0;
}

bool StreamAudioDecoder::open(AVFormatContext *fmt) {
    close();
    const AVCodec *codec = nullptr;
    int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (stream < 0)
        return false;
    dec_ = avcodec_alloc_context3(codec);
    if (!dec_ || avcodec_parameters_to_context(dec_, fmt->streams[stream]->codecpar) < 0 ||
        avcodec_open2(dec_, codec, nullptr) < 0) {
        close();
        return false;
    }
    frm_ = av_frame_alloc();
    stream_ = stream;
    SPDLOG_INFO("Stream audio: {} {}Hz {}ch", codec->name, dec_->sample_rate, dec_->ch_layout.nb_channels);
    return true;
}

void StreamAudioDecoder::decode(const AVPacket *pkt) {
    if (!dec_ || pkt->stream_index != stream_ || avcodec_send_packet(dec_, pkt) < 0)
        return;
    while (avcodec_receive_frame(dec_, frm_) >= 0) {
        std::uint32_t rate;
        std::uint16_t channels;
        buffer_->target(rate, channels);
        if (!swr_ || rate != out_rate_ || channels != out_channels_ || frm_->sample_rate != in_rate_ ||
            frm_->format != in_format_ || frm_->ch_layout.nb_channels != in_channels_) {
            swr_free(&swr_);
            AVChannelLayout in_layout, out_layout;
            // 裸 PCM（如 WAV）可能没有声道布局，按声道数取默认布局
            if (frm_->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
                av_channel_layout_default(&in_layout, frm_->ch_layout.nb_channels);
            else
                av_channel_layout_copy(&in_layout, &frm_->ch_layout);
            av_channel_layout_default(&out_layout, channels);
            int ret = swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_S16, static_cast<int>(rate), &in_layout,
                                          static_cast<AVSampleFormat>(frm_->format), frm_->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&in_layout);
            if (ret < 0 || swr_init(swr_) < 0) {
                SPDLOG_WARN("Stream audio: cannot create resampler {}Hz -> {}Hz", frm_->sample_rate, rate);
                swr_free(&swr_);
                av_frame_unref(frm_);
                continue;
            }
            in_rate_ = frm_->sample_rate;
            in_format_ = frm_->format;
            in_channels_ = frm_->ch_layout.nb_channels;
            out_rate_ = rate;
            out_channels_ = channels;
        }
        int max_out = swr_get_out_samples(swr_, frm_->nb_samples);
        if (max_out > 0) {
            out_.resize(static_cast<std::size_t>(max_out) * channels);
            auto *dst = reinterpret_cast<std::uint8_t *>(out_.data());
            int n = swr_convert(swr_, &dst, max_out, const_cast<const std::uint8_t **>(frm_->extended_data),
                                frm_->nb_samples);
            if (n > 0)
                buffer_->push(out_.data(), static_cast<std::size_t>(n), rate);
        }
        av_frame_unref(frm_);
    }
}

// ========== IpCameraSource ==========

IpCameraSource::IpCameraSource(std::string url, std::uint16_t width, std::uint16_t height, std::uint8_t fps,
                               std::shared_ptr<StreamAudioBuffer> audio) :
    url_(std::move(url)), audio_(std::move(audio)), width_(width), height_(height), frame_interval_(10'000'000U / fps) {
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
    AVFormatContext *fmt = open_stream_input(url_, &stop_);
    if (!fmt)
        return;

    const AVCodec *codec = nullptr;
    AVCodecContext *dec = nullptr;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm = av_frame_alloc();
    SwsContext *sws = nullptr;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width_) * height_ * 2);
    StreamAudioDecoder audio(audio_);
    int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream >= 0 && (dec = avcodec_alloc_context3(codec)) != nullptr &&
        avcodec_parameters_to_context(dec, fmt->streams[stream]->codecpar) >= 0 &&
        avcodec_open2(dec, codec, nullptr) >= 0) {
        SPDLOG_INFO("IP camera: connected {} — {} {}x{} → YUY2 {}x{}", url_, codec->name, dec->width, dec->height,
                    width_, height_);
        if (audio_ && !audio.open(fmt))
            SPDLOG_WARN("IP camera: {} has no audio stream, microphone stays silent", url_);
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
            else if (audio_ && pkt->stream_index == audio.stream()) {
                audio.decode(pkt);
            }
            av_packet_unref(pkt);
        }
        if (!stop_)
            SPDLOG_WARN("IP camera: stream {} ended, reconnecting", url_);
    }
    else {
        SPDLOG_WARN("IP camera: no decodable video stream in {}, retrying", url_);
    }

    if (audio_)
        audio_->clear();
    sws_freeContext(sws);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
}

// ========== IpAudioStream ==========

IpAudioStream::IpAudioStream(std::string url, std::shared_ptr<StreamAudioBuffer> buffer) :
    url_(std::move(url)), buffer_(std::move(buffer)) {
    thread_ = std::thread([this] { run(); });
}

IpAudioStream::~IpAudioStream() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}

void IpAudioStream::run() {
    while (!stop_) {
        stream_once();
        for (int i = 0; i < 20 && !stop_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void IpAudioStream::stream_once() {
    AVFormatContext *fmt = open_stream_input(url_, &stop_);
    if (!fmt)
        return;
    StreamAudioDecoder audio(buffer_);
    if (audio.open(fmt)) {
        SPDLOG_INFO("Audio stream: connected {}", url_);
        AVPacket *pkt = av_packet_alloc();
        while (!stop_ && av_read_frame(fmt, pkt) >= 0) {
            audio.decode(pkt);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        if (!stop_)
            SPDLOG_WARN("Audio stream: {} ended, reconnecting", url_);
    }
    else {
        SPDLOG_WARN("Audio stream: no audio in {}, retrying", url_);
    }
    buffer_->clear();
    avformat_close_input(&fmt);
}

} // namespace usbipdcpp
