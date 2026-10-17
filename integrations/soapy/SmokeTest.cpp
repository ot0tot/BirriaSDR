// SPDX-License-Identifier: MIT
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Errors.hpp>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

int main(int argc,char **argv)
{
    uint32_t rate=argc>1 ? static_cast<uint32_t>(std::stoul(argv[1])) : 40000000;
    std::unique_ptr<SoapySDR::Device,void(*)(SoapySDR::Device*)> dev(
        SoapySDR::Device::make("driver=birria,index=0,experimental_mt7961=1,experimental_tuning=1,experimental_80msps=1"),
        [](SoapySDR::Device *p) { SoapySDR::Device::unmake(p); });
    if (!dev) return 1;
    std::cerr<<"stage=rate"<<std::endl;
    dev->setSampleRate(SOAPY_SDR_RX,0,rate);
    if(dev->getSampleRate(SOAPY_SDR_RX,0)!=rate) return 1;
    uint32_t bandwidth=argc>2 ? static_cast<uint32_t>(std::stoul(argv[2])) : 40000000;
    dev->setBandwidth(SOAPY_SDR_RX,0,bandwidth);
    if(dev->getBandwidth(SOAPY_SDR_RX,0)!=bandwidth) return 1;
    for (const auto &format : {SOAPY_SDR_CF32,SOAPY_SDR_CS16}) {
        auto *stream = dev->setupStream(SOAPY_SDR_RX,format,{0});
        bool active=false;
        try {
            std::vector<float> iq(16384*2);
            void *buffers[]={iq.data()}; int flags=0; long long timestamp=0;
            auto before=std::chrono::steady_clock::now();
            int n=dev->readStream(stream,buffers,137,flags,timestamp,20000);
            if(n!=SOAPY_SDR_TIMEOUT || std::chrono::steady_clock::now()-before<std::chrono::milliseconds(15)) throw std::runtime_error("Inactive timeout");
            if(dev->activateStream(stream)) throw std::runtime_error("Activation");
            std::cerr<<"stage=active format="<<format<<std::endl;
            active=true;
            try { dev->setBandwidth(SOAPY_SDR_RX,0,40000000); throw std::logic_error("Live filter change accepted"); }
            catch(const std::runtime_error &) { }
            try { dev->setSampleRate(SOAPY_SDR_RX,0,rate); throw std::logic_error("Active rate change accepted"); }
            catch(const std::runtime_error &) { }
            if(dev->getBandwidth(SOAPY_SDR_RX,0)!=bandwidth || dev->getSampleRate(SOAPY_SDR_RX,0)!=rate) throw std::runtime_error("Rejected control changed metadata");
            uint64_t total=0;double peak=0;
            auto start=std::chrono::steady_clock::now();
            while(std::chrono::steady_clock::now()-start<std::chrono::seconds(3)) {
                // Deliberately request partial blocks, checking queue offsets.
                n=dev->readStream(stream,buffers,4093,flags,timestamp,1000000);
                if(n<0) throw std::runtime_error("readStream error "+std::to_string(n));
                if(flags&SOAPY_SDR_HAS_TIME) throw std::runtime_error("Invented timestamp");
                if(std::string(format)==SOAPY_SDR_CF32) {
                    for(int i=0;i<n*2;++i) {
                        if(!std::isfinite(iq[i]) || iq[i]<-1 || iq[i]>=1) throw std::runtime_error("Float scaling");
                        peak=std::max(peak,std::abs(double(iq[i])));
                    }
                } else {
                    auto *raw=reinterpret_cast<int16_t *>(iq.data());
                    for(int i=0;i<n*2;++i) {
                        if(raw[i]<-8192 || raw[i]>8191) throw std::runtime_error("Raw14 scaling");
                        peak=std::max(peak,std::abs(double(raw[i])));
                    }
                }
                bool retune=total<4000000 && total+uint64_t(n)>=4000000;
                total+=uint64_t(n);
                if(retune) {
                    std::cerr<<"stage=retune"<<std::endl;
                    dev->setFrequency(SOAPY_SDR_RX,0,5800000000);
                    std::cerr<<"stage=gain"<<std::endl;
                    dev->writeSetting("lpf_code","5");
                }
            }
            if(dev->deactivateStream(stream)) throw std::runtime_error("Deactivation");
            active=false;
            dev->closeStream(stream);stream=nullptr;
            std::cout<<"{\"format\":\""<<format<<"\",\"samples\":"<<total<<",\"peak\":"<<peak<<"}"<<std::endl;
            if(total<uint64_t(rate)*2 || peak==0) throw std::runtime_error("Insufficient samples");
        } catch(const std::exception &error) {
            if(active) dev->deactivateStream(stream);
            if(stream) dev->closeStream(stream);
            std::cerr<<error.what()<<std::endl; return 1;
        }
    }
    return 0;
}
