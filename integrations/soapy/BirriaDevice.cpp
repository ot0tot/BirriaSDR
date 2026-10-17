// SPDX-License-Identifier: MIT
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Version.hpp>
#include "birriasdr.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void check(int result)
{
    if (result < 0) throw std::runtime_error(std::string("BirriaSDR: ") + birriasdr_strerror(result));
}
unsigned number(const std::string &text, unsigned maximum)
{
    size_t used = 0;
    unsigned long value = std::stoul(text, &used);
    if (used != text.size() || value > maximum) throw std::invalid_argument("Invalid BirriaSDR integer: " + text);
    return static_cast<unsigned>(value);
}
class BirriaDevice final : public SoapySDR::Device {
    static constexpr size_t block = 16384, depth = 1024;
    size_t queueDepth=128;
    birriasdr_dev_t *dev = nullptr;
    std::mutex control, mutex;
    std::condition_variable ready;
    std::thread worker;
    std::vector<int16_t> queue;
    size_t lengths[depth] = {}, head = 0, tail = 0, available = 0, offset = 0;
    bool configured = false, cf32 = true, stopping = false, finished = true;
    int terminal = 0;
    uint64_t minimum = 0, maximum = 0;
    std::vector<birriasdr_tuning_range> ranges;
    birriasdr_gain gain{1, 4, 0};
    bool manual = true;
    bool allow80 = false;
    char streamToken{};
    SoapySDR::Stream *stream() { return reinterpret_cast<SoapySDR::Stream *>(&streamToken); }
    void channel(int direction, size_t index) const
    {
        if (direction != SOAPY_SDR_RX || index != 0) throw std::invalid_argument("BirriaSDR has one receive channel");
    }
    void handle(SoapySDR::Stream *value) { if (value != stream() || !configured) throw std::invalid_argument("Invalid BirriaSDR stream"); }
    static void receive(const int16_t *iq, uint32_t count, uint64_t, void *context) noexcept
    {
        auto &self = *static_cast<BirriaDevice *>(context);
        bool cancel = false;
        {
            std::lock_guard<std::mutex> lock(self.mutex);
            if (self.stopping) return;
            if (count > block || self.available == self.queueDepth) {
                self.terminal = SOAPY_SDR_OVERFLOW;
                self.stopping = true; cancel = true;
            } else {
                std::memcpy(self.queue.data() + self.tail*block*2, iq, count*4);
                self.lengths[self.tail] = count;
                self.tail = (self.tail+1)%self.queueDepth; ++self.available;
            }
            self.ready.notify_all();
        }
        if (cancel) {
            SoapySDR::log(SOAPY_SDR_ERROR,"BirriaSDR consumer queue overrun");
            birriasdr_cancel_async(self.dev);
        }
    }
    void stop()
    {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; ready.notify_all(); }
        if (worker.joinable()) {
            for (;;) {
                birriasdr_cancel_async(dev);
                std::unique_lock<std::mutex> lock(mutex);
                if (finished) break;
                ready.wait_for(lock, std::chrono::milliseconds(20));
            }
            worker.join();
        }
        std::lock_guard<std::mutex> lock(mutex);
        available = offset = head = tail = 0;
    }
public:
    explicit BirriaDevice(const SoapySDR::Kwargs &args)
    {
        unsigned index = args.count("index") ? number(args.at("index"), UINT32_MAX) : 0;
        uint32_t flags = args.count("experimental_mt7961") && args.at("experimental_mt7961") == "1" ? BIRRIASDR_OPEN_EXPERIMENTAL_MT7961 : 0;
        if (args.count("experimental_tuning") && args.at("experimental_tuning")=="1") flags|=BIRRIASDR_OPEN_EXPERIMENTAL_TUNING;
        allow80=args.count("experimental_80msps") && args.at("experimental_80msps")=="1";
        if(allow80) flags|=BIRRIASDR_OPEN_EXPERIMENTAL_80MSPS;
        queueDepth=allow80 ? depth : 128;
        queue.resize(queueDepth*block*2);
        check(birriasdr_open_ex(&dev, index, flags));
        try {
            check(birriasdr_get_tuning_range(dev, &minimum, &maximum));
            int count=birriasdr_get_tuning_ranges(dev,nullptr,0);check(count);ranges.resize(static_cast<size_t>(count));
            check(birriasdr_get_tuning_ranges(dev,ranges.data(),static_cast<uint32_t>(ranges.size())));
            check(birriasdr_set_tuner_gain_mode(dev, 1));
            check(birriasdr_set_gain_codes(dev, &gain));
        } catch (...) { birriasdr_close(dev); throw; }
    }
    ~BirriaDevice() override
    {
        stop();
        int result = birriasdr_close(dev);
        if (result < 0) SoapySDR::logf(SOAPY_SDR_ERROR, "BirriaSDR close: %s", birriasdr_strerror(result));
    }
    std::string getDriverKey() const override { return "birria"; }
    std::string getHardwareKey() const override { return "MT7961/MT7972"; }
    SoapySDR::Kwargs getHardwareInfo() const override
    {
        return {{"library", birriasdr_get_version()}, {"capture", "raw14, receive only, uncalibrated RF"}};
    }
    size_t getNumChannels(int direction) const override { return direction == SOAPY_SDR_RX ? 1 : 0; }
    bool getFullDuplex(int direction, size_t index) const override { channel(direction,index); return false; }
    std::vector<std::string> getStreamFormats(int direction,size_t index) const override
    { channel(direction,index); return {SOAPY_SDR_CF32, SOAPY_SDR_CS16}; }
    std::string getNativeStreamFormat(int direction,size_t index,double &fullScale) const override
    { channel(direction,index); fullScale = 8192; return SOAPY_SDR_CS16; }
    SoapySDR::Stream *setupStream(int direction, const std::string &format,
        const std::vector<size_t> &channels, const SoapySDR::Kwargs &args) override
    {
        channel(direction,0);
        if ((!channels.empty() && channels != std::vector<size_t>{0}) || !args.empty()) throw std::invalid_argument("Unsupported BirriaSDR stream arguments");
        if (format != SOAPY_SDR_CF32 && format != SOAPY_SDR_CS16) throw std::invalid_argument("Use CF32 or CS16");
        std::lock_guard<std::mutex> lock(control);
        if (configured) throw std::runtime_error("BirriaSDR supports one stream");
        configured = true; cf32 = format == SOAPY_SDR_CF32; return stream();
    }
    void closeStream(SoapySDR::Stream *value) override
    { std::lock_guard<std::mutex> lock(control); handle(value); stop(); configured = false; }
    size_t getStreamMTU(SoapySDR::Stream *) const override { return block; }
    int activateStream(SoapySDR::Stream *value,int flags,long long,size_t numElems) override
    {
        std::lock_guard<std::mutex> lock(control); handle(value);
        if (flags || numElems) return SOAPY_SDR_NOT_SUPPORTED;
        { std::lock_guard<std::mutex> guard(mutex); if (!finished) return SOAPY_SDR_STREAM_ERROR; }
        if (worker.joinable()) worker.join();
        { std::lock_guard<std::mutex> guard(mutex); head=tail=available=offset=0; stopping=false; finished=false; terminal=0; }
        try {
            worker = std::thread([this] {
                int result = birriasdr_read_async(dev, receive, this, 0, static_cast<uint32_t>(block));
                std::lock_guard<std::mutex> guard(mutex);
                if (!terminal && result < 0) terminal = result == BIRRIASDR_ERROR_OVERRUN ? SOAPY_SDR_OVERFLOW : SOAPY_SDR_STREAM_ERROR;
                if (result < 0) SoapySDR::logf(SOAPY_SDR_ERROR, "BirriaSDR read: %s", birriasdr_strerror(result));
                finished = true; ready.notify_all();
            });
        } catch (...) { std::lock_guard<std::mutex> guard(mutex); finished=true; throw; }
        return 0;
    }
    int deactivateStream(SoapySDR::Stream *value,int flags,long long) override
    { std::lock_guard<std::mutex> lock(control); handle(value); if (flags) return SOAPY_SDR_NOT_SUPPORTED; stop(); return 0; }
    int readStream(SoapySDR::Stream *value,void *const *buffs,size_t numElems,int &flags,long long &timeNs,long timeoutUs) override
    {
        handle(value); flags = 0; timeNs = 0;
        if (!numElems) return 0;
        if (!buffs || !buffs[0]) return SOAPY_SDR_STREAM_ERROR;
        std::unique_lock<std::mutex> lock(mutex);
        if (!available) ready.wait_for(lock, std::chrono::microseconds(std::max(0L,timeoutUs)), [this] { return available || terminal; });
        if (!available) return terminal ? terminal : SOAPY_SDR_TIMEOUT;
        size_t count = std::min(numElems, lengths[head]-offset);
        const int16_t *input = queue.data() + head*block*2 + offset*2;
        if (cf32) {
            float *output = static_cast<float *>(buffs[0]);
            for (size_t i=0;i<count*2;++i) output[i] = input[i]/8192.0f;
        } else std::memcpy(buffs[0], input, count*4);
        offset += count;
        if (offset == lengths[head]) { offset=0; head=(head+1)%queueDepth; --available; }
        if (!available && finished) flags |= SOAPY_SDR_END_BURST;
        return static_cast<int>(count);
    }
    void setSampleRate(int direction,size_t index,double value) override
    {
        channel(direction,index);
        if(value!=40000000 && !(allow80 && value==80000000)) throw std::invalid_argument("Unsupported BirriaSDR sample rate");
        std::lock_guard<std::mutex> lock(control);
        if(worker.joinable()) throw std::runtime_error("Deactivate the stream before changing its rate");
        check(birriasdr_set_sample_rate(dev,static_cast<uint32_t>(value)));
    }
    double getSampleRate(int direction,size_t index) const override { channel(direction,index); return birriasdr_get_sample_rate(dev); }
    std::vector<double> listSampleRates(int direction,size_t index) const override { channel(direction,index);return allow80 ? std::vector<double>{40000000,80000000} : std::vector<double>{40000000}; }
    SoapySDR::RangeList getSampleRateRange(int direction,size_t index) const override
    { channel(direction,index);auto result=SoapySDR::RangeList{SoapySDR::Range(40000000,40000000)};if(allow80) result.emplace_back(80000000,80000000);return result; }
    std::vector<std::string> listFrequencies(int direction,size_t index) const override { channel(direction,index); return {"RF"}; }
    void setFrequency(int direction,size_t index,const std::string &name,double value,const SoapySDR::Kwargs &) override
    {
        channel(direction,index);
        if (name != "RF" || !std::isfinite(value) || value < double(minimum) || value > double(maximum)) throw std::invalid_argument("Unsupported BirriaSDR center");
        std::lock_guard<std::mutex> lock(control); check(birriasdr_set_center_freq(dev,static_cast<uint64_t>(std::llround(value))));
    }
    double getFrequency(int direction,size_t index,const std::string &name) const override
    { channel(direction,index); if(name!="RF") throw std::invalid_argument("Unknown frequency element"); return double(birriasdr_get_center_freq(dev)); }
    SoapySDR::RangeList getFrequencyRange(int direction,size_t index,const std::string &name) const override
    { channel(direction,index); if(name!="RF") throw std::invalid_argument("Unknown frequency element"); SoapySDR::RangeList result;for(const auto &range:ranges) result.emplace_back(double(range.minimum_hz),double(range.maximum_hz));return result; }
    void setBandwidth(int direction,size_t index,double value) override
    {
        channel(direction,index);
        if(value==0) value=40000000;
        if(value!=40000000 && value!=80000000) throw std::invalid_argument("Use a nominal 40 or 80 MHz filter profile");
        std::lock_guard<std::mutex> lock(control);
        if(worker.joinable()) throw std::runtime_error("Deactivate the stream before changing its filter");
        check(birriasdr_set_tuner_bandwidth(dev,static_cast<uint32_t>(value)));
    }
    double getBandwidth(int direction,size_t index) const override { channel(direction,index); return birriasdr_get_tuner_bandwidth(dev); }
    std::vector<double> listBandwidths(int direction,size_t index) const override
    { channel(direction,index); return allow80 ? std::vector<double>{40000000,80000000} : std::vector<double>{40000000}; }
    std::vector<std::string> listAntennas(int direction,size_t index) const override { channel(direction,index); return {"RX"}; }
    void setAntenna(int direction,size_t index,const std::string &name) override
    { channel(direction,index); if(name!="RX") throw std::invalid_argument("Antenna switching is unsupported"); }
    std::string getAntenna(int direction,size_t index) const override { channel(direction,index); return "RX"; }
    // Soapy's gain API means dB. Raw hardware indices belong in settings.
    std::vector<std::string> listGains(int direction,size_t index) const override { channel(direction,index); return {}; }
    SoapySDR::ArgInfoList getSettingInfo() const override
    {
        SoapySDR::ArgInfoList result;
        for (const auto &name : {"manual_gain", "lna_code", "lpf_code", "digital_code"}) {
            SoapySDR::ArgInfo info; info.key=name; info.name=name; info.type=SoapySDR::ArgInfo::INT;
            info.description="Uncalibrated hardware index, not dB. Manual mode releases/forces documented overrides, not complete AGC.";
            unsigned max = info.key=="manual_gain" ? 1 : info.key=="lna_code" ? 3 : info.key=="lpf_code" ? 8 : 12;
            info.range=SoapySDR::Range(0,max,1); info.value=readSetting(name); result.push_back(info);
        }
        return result;
    }
    void writeSetting(const std::string &key,const std::string &text) override
    {
        std::lock_guard<std::mutex> lock(control);
        if(key=="manual_gain") { bool mode=number(text,1)!=0; check(birriasdr_set_tuner_gain_mode(dev,mode)); manual=mode; return; }
        auto next=gain;
        if(key=="lna_code") next.lna=static_cast<uint8_t>(number(text,3));
        else if(key=="lpf_code") next.lpf=static_cast<uint8_t>(number(text,8));
        else if(key=="digital_code") next.digital=static_cast<uint8_t>(number(text,12));
        else throw std::invalid_argument("Unknown BirriaSDR setting");
        check(birriasdr_set_gain_codes(dev,&next)); gain=next;
    }
    std::string readSetting(const std::string &key) const override
    {
        birriasdr_gain current{}; int forced=0; check(birriasdr_get_gain_codes(dev,&current,&forced));
        if(key=="manual_gain") return forced ? "1" : "0";
        if(key=="lna_code") return std::to_string(current.lna);
        if(key=="lpf_code") return std::to_string(current.lpf);
        if(key=="digital_code") return std::to_string(current.digital);
        throw std::invalid_argument("Unknown BirriaSDR setting");
    }
};
SoapySDR::KwargsList find(const SoapySDR::Kwargs &args)
{
    SoapySDR::KwargsList result;
    int count=birriasdr_get_device_count();
    for(int i=0;i<count;++i) {
        if(args.count("index") && args.at("index")!=std::to_string(i)) continue;
        birriasdr_device_info info{};
        if(birriasdr_get_device_info(static_cast<uint32_t>(i),&info)<0) continue;
        bool experimental=info.vid==0x0e8d && info.pid==0x7961;
        if(!experimental && !(info.capabilities&BIRRIASDR_CAP_RX)) continue;
        SoapySDR::Kwargs item{{"driver","birria"},{"index",std::to_string(i)},
            {"label",std::string("BirriaSDR ")+info.name+(experimental ? " (experimental MT7961)" : "")}};
        if(experimental) item["experimental_mt7961"]="1";
        result.push_back(item);
    }
    return result;
}
SoapySDR::Device *make(const SoapySDR::Kwargs &args) { return new BirriaDevice(args); }
static SoapySDR::Registry registration("birria", &find, &make, SOAPY_SDR_ABI_VERSION);
}
