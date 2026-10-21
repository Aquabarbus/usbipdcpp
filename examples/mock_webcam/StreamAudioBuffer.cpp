#include "StreamAudioBuffer.h"

#include <algorithm>
#include <cstring>

namespace usbipdcpp {

StreamAudioBuffer::StreamAudioBuffer(std::vector<std::uint32_t> sample_rates, std::uint16_t channels) :
    sample_rates_(std::move(sample_rates)), channels_(channels), sample_rate_(sample_rates_.front()) {}

void StreamAudioBuffer::target(std::uint32_t &sample_rate, std::uint16_t &channels) {
    std::lock_guard lock(mutex_);
    sample_rate = sample_rate_;
    channels = channels_;
}

void StreamAudioBuffer::push(const std::int16_t *data, std::size_t frames, std::uint32_t sample_rate) {
    std::lock_guard lock(mutex_);
    if (sample_rate != sample_rate_)
        return;
    samples_.insert(samples_.end(), data, data + frames * channels_);
    auto per_ms = static_cast<std::size_t>(sample_rate_ / 1000) * channels_;
    if (samples_.size() > per_ms * kMaxMs)
        samples_.erase(samples_.begin(), samples_.begin() + (samples_.size() - per_ms * kTrimToMs));
}

void StreamAudioBuffer::clear() {
    std::lock_guard lock(mutex_);
    samples_.clear();
    playing_ = false;
}

std::vector<AudioFormatInfo> StreamAudioBuffer::formats() const {
    std::vector<AudioFormatInfo> result;
    for (auto rate: sample_rates_)
        result.push_back({channels_, 16, rate});
    return result;
}

AudioFormatInfo StreamAudioBuffer::current_format() const {
    std::lock_guard lock(mutex_);
    return {channels_, 16, sample_rate_};
}

bool StreamAudioBuffer::set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) {
    if (channels != channels_ || bits_per_sample != 16 ||
        std::find(sample_rates_.begin(), sample_rates_.end(), sample_rate) == sample_rates_.end())
        return false;
    std::lock_guard lock(mutex_);
    if (sample_rate != sample_rate_) {
        sample_rate_ = sample_rate;
        samples_.clear(); // 旧采样率的数据作废，生产者看到新 target 后重建重采样
        playing_ = false;
    }
    return true;
}

bool StreamAudioBuffer::get_chunk(AudioChunk &chunk) {
    std::lock_guard lock(mutex_);
    // 每块 1ms（采样率为 1kHz 整数倍时整除，同 AudioVideoFileState）
    auto want = static_cast<std::size_t>(sample_rate_ / 1000) * channels_;
    chunk_buffer_.assign(want * sizeof(std::int16_t), 0);
    if (!playing_ && samples_.size() >= want * kPrebufferMs)
        playing_ = true;
    if (playing_) {
        auto n = std::min(want, samples_.size());
        std::copy_n(samples_.begin(), n, reinterpret_cast<std::int16_t *>(chunk_buffer_.data()));
        samples_.erase(samples_.begin(), samples_.begin() + n);
        if (n < want)
            playing_ = false; // 欠载：本块尾部静音，重新预缓冲
    }
    chunk.data = chunk_buffer_.data();
    chunk.size = chunk_buffer_.size();
    return true;
}

} // namespace usbipdcpp
