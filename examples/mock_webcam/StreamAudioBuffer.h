#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "usbipdcpp/virtual_device/audio_sources/AudioSource.h"

namespace usbipdcpp {

/// 实时音频缓冲：网络流解码线程按当前协商格式（S16LE 交错）push，UAC 按 1ms 块取。
/// 无生产者时恒输出静音（--mic mute）。
///
/// 抖动处理：先攒够 kPrebufferMs 才开始出声，欠载补静音并重新预缓冲；
/// 积压超过 kMaxMs（摄像机时钟快于 USB 时钟）丢弃最旧数据回到 kTrimToMs，延迟有上界
class StreamAudioBuffer {
public:
    StreamAudioBuffer(std::vector<std::uint32_t> sample_rates, std::uint16_t channels);

    // ========== 生产者侧（解码线程） ==========

    /// 当前协商的采样率/声道数（生产者据此配置重采样）
    void target(std::uint32_t &sample_rate, std::uint16_t &channels);

    /// 追加 frames 帧 PCM；sample_rate 与当前协商值不同（刚切换）时丢弃
    void push(const std::int16_t *data, std::size_t frames, std::uint32_t sample_rate);

    /// 清空（断流时调用，避免重连后播放陈旧数据）
    void clear();

    // ========== 消费者侧（StreamAudioSourceView 转发） ==========

    std::vector<AudioFormatInfo> formats() const;
    AudioFormatInfo current_format() const;
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate);
    bool get_chunk(AudioChunk &chunk);

private:
    static constexpr std::uint32_t kPrebufferMs = 60;
    static constexpr std::uint32_t kMaxMs = 300;
    static constexpr std::uint32_t kTrimToMs = 100;

    std::vector<std::uint32_t> sample_rates_;
    std::uint16_t channels_;
    std::uint32_t sample_rate_;

    mutable std::mutex mutex_;
    std::deque<std::int16_t> samples_; // 交错样本
    bool playing_ = false; // 预缓冲已满、正在出声
    std::vector<std::uint8_t> chunk_buffer_;
};

/// AudioSource 视图（UAC 持有 unique_ptr，缓冲本身与解码线程共享）
class StreamAudioSourceView : public AudioSource {
public:
    explicit StreamAudioSourceView(std::shared_ptr<StreamAudioBuffer> buffer) : buffer_(std::move(buffer)) {}

    std::vector<AudioFormatInfo> supported_formats() const override {
        return buffer_->formats();
    }
    AudioFormatInfo current_format() const override {
        return buffer_->current_format();
    }
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override {
        return buffer_->set_format(channels, bits_per_sample, sample_rate);
    }
    bool get_chunk(AudioChunk &chunk) override {
        return buffer_->get_chunk(chunk);
    }

private:
    std::shared_ptr<StreamAudioBuffer> buffer_;
};

} // namespace usbipdcpp
