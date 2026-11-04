#include "PluginProcessor.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

namespace {
std::atomic<bool> watchAllocation{false};
std::atomic<unsigned> audioAllocations{0};
void require(bool okay,const char* message){if(!okay)throw std::runtime_error(message);}
class Playhead final:public juce::AudioPlayHead {
public:
    std::int64_t sample=0;bool playing=true;
    juce::Optional<PositionInfo> getPosition()const override{
        PositionInfo p;p.setTimeInSamples(sample);p.setTimeInSeconds(sample/48000.0);p.setIsPlaying(playing);return p;
    }
};
void set(NoPeaksAudioProcessor& p,const char* id,float value){
    auto* param=p.parameters.getParameter(id);require(param!=nullptr,"Missing parameter");
    param->setValueNotifyingHost(param->convertTo0to1(value));
}
float source(int track,std::int64_t n){
    constexpr double pi=3.14159265358979323846;
    if(track==0)return 0.71f*static_cast<float>(std::sin(2*pi*220*n/48000.0))+((n%8192)<20?0.22f:0);
    return 0.68f*static_cast<float>(std::sin(2*pi*330*n/48000.0));
}
void screenshots(const std::string& folder){
    std::filesystem::create_directories(folder);
    auto master=std::make_unique<NoPeaksAudioProcessor>(true);
    auto first=std::make_unique<NoPeaksAudioProcessor>(false);
    auto second=std::make_unique<NoPeaksAudioProcessor>(false);
    Playhead mt,ft,st;master->setPlayHead(&mt);first->setPlayHead(&ft);second->setPlayHead(&st);
    for(auto* p:{master.get(),first.get(),second.get()}){set(*p,"session",4320);p->prepareToPlay(48000,128);}
    set(*master,"cooperative",1);set(*first,"priority",95);set(*first,"protect",1);set(*second,"priority",35);
    first->setTrackName("Lead vocal");second->setTrackName("Instrument bus");
    juce::AudioBuffer<float> a(2,128),b(2,128),mix(2,128);juce::MidiBuffer midi;
    std::int64_t clock=0;
    for(int frame=0;frame<1250;++frame){
        mt.sample=ft.sample=st.sample=clock;
        for(int n=0;n<128;++n){
            a.setSample(0,n,source(0,clock+n));a.setSample(1,n,source(0,clock+n));
            b.setSample(0,n,source(1,clock+n));b.setSample(1,n,-0.5f*source(1,clock+n));
        }
        first->processBlock(a,midi);second->processBlock(b,midi);mix.makeCopyOf(a,true);
        mix.addFrom(0,0,b,0,0,128);mix.addFrom(1,0,b,1,0,128);master->processBlock(mix,midi);
        if(frame==180)master->captureTarget();
        clock+=128;
    }
    for(auto* processor:{master.get(),second.get()}){
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor->createEditor());
        juce::Image image(juce::Image::ARGB,editor->getWidth(),editor->getHeight(),true);
        juce::Graphics graphics(image);editor->paintEntireComponent(graphics,true);
        juce::FileOutputStream stream(juce::File(juce::String(folder+(processor->isMaster()?"/master.png":"/track.png"))));
        require(stream.openedOk(),"Cannot write editor screenshot");
        stream.setPosition(0);
        require(stream.truncate().wasOk(),"Cannot truncate editor screenshot");
        require(juce::PNGImageFormat().writeImageToStream(image,stream),"Cannot encode editor screenshot");
    }
}
void verifyWidthAmount(){
    auto master=std::make_unique<NoPeaksAudioProcessor>(true);
    auto side=std::make_unique<NoPeaksAudioProcessor>(false);
    auto mid=std::make_unique<NoPeaksAudioProcessor>(false);
    Playhead mt,st,ct;master->setPlayHead(&mt);side->setPlayHead(&st);mid->setPlayHead(&ct);
    const int project=8000+static_cast<int>(nopeaks::SharedBus::clockNanoseconds()%1000);
    for(auto* p:{master.get(),side.get(),mid.get()}){set(*p,"session",static_cast<float>(project));set(*p,"group",13);p->prepareToPlay(48000,128);}
    set(*master,"cooperative",1);set(*master,"peakEnabled",0);set(*master,"toneEnabled",0);set(*master,"widthAmount",100);
    set(*mid,"protect",1);set(*side,"priority",0);
    juce::AudioBuffer<float> a(2,128),b(2,128),mix(2,128);juce::MidiBuffer midi;
    std::int64_t clock=0;
    auto render=[&](int frames){for(int frame=0;frame<frames;++frame){
        mt.sample=st.sample=ct.sample=clock;
        for(int n=0;n<128;++n){
            const float s=0.5f*std::sin(static_cast<float>(6.283185307179586*630*(clock+n)/48000.0));
            const float m=0.11f*std::sin(static_cast<float>(6.283185307179586*270*(clock+n)/48000.0));
            a.setSample(0,n,s);a.setSample(1,n,-s);b.setSample(0,n,m);b.setSample(1,n,m);
        }
        side->processBlock(a,midi);mid->processBlock(b,midi);mix.makeCopyOf(a,true);
        mix.addFrom(0,0,b,0,0,128);mix.addFrom(1,0,b,1,0,128);master->processBlock(mix,midi);clock+=128;
    }};
    render(600);
    require(master->getUiState().routingValid,"Stereo session routing must validate");
    auto before=side->getUiState();float applied=0;for(float db:before.sideDb)applied=std::min(applied,db);
    require(applied < -0.1f,"Negative correlation must produce a real side-band intervention");
    set(*master,"widthAmount",0);render(600);
    auto after=side->getUiState();float remaining=0;for(float db:after.sideDb)remaining=std::max(remaining,std::abs(db));
    require(remaining < 0.03f,"Width Amount zero must return to neutral even while correlation stays negative");
    std::cout<<"Width Amount100->0: strongest setting="<<applied<<"dB, remaining="<<remaining<<"dB\n";
}
void verifyStoppedMonitoring(){
    auto track=std::make_unique<NoPeaksAudioProcessor>(false);
    Playhead playhead;playhead.playing=false;track->setPlayHead(&playhead);track->prepareToPlay(48000,128);
    juce::AudioBuffer<float> audio(2,128);juce::MidiBuffer midi;
    double energy=0;
    for(int block=0;block<100;++block){
        audio.clear();for(int ch=0;ch<2;++ch)for(int n=0;n<128;++n)audio.setSample(ch,n,0.25f);
        track->processBlock(audio,midi);
        if(block>70)energy+=audio.getMagnitude(0,128);
    }
    require(energy>1,"Stopped transport must preserve delayed live monitoring");
    require(!track->getUiState().routingValid,"Stopped transport cannot authorize routing");
}
void verifyBypass(){
    for(bool master:{false,true}){
        auto p=std::make_unique<NoPeaksAudioProcessor>(master);
        p->prepareToPlay(48000,128);juce::AudioBuffer<float> audio(2,128);juce::MidiBuffer midi;
        const auto latency=p->getLatencySamples();
        for(int b=0;b<75;++b){
            audio.clear();if(b==0)audio.setSample(0,0,0.5f);
            p->processBlockBypassed(audio,midi);
            for(int n=0;n<128;++n){const int absolute=b*128+n;
                require(std::abs(audio.getSample(0,n)-(absolute==latency?0.5f:0))<1e-6f,"Host bypass must retain declared latency and exact impulse");}
        }
    }
}
void verifySession(){
    auto master=std::make_unique<NoPeaksAudioProcessor>(true);
    auto first=std::make_unique<NoPeaksAudioProcessor>(false);
    auto second=std::make_unique<NoPeaksAudioProcessor>(false);
    Playhead masterTime,firstTime,secondTime;
    master->setPlayHead(&masterTime);first->setPlayHead(&firstTime);second->setPlayHead(&secondTime);
    const int session=4000+static_cast<int>(nopeaks::SharedBus::clockNanoseconds()%4000);
    for(auto* p:{master.get(),first.get(),second.get()}){set(*p,"session",static_cast<float>(session));set(*p,"group",14);p->prepareToPlay(48000,512);}
    set(*master,"cooperative",1);set(*master,"toneEnabled",0);set(*master,"widthEnabled",0);
    set(*master,"conflictAmount",0);set(*master,"ceiling",-1);set(*first,"priority",100);set(*first,"protect",1);set(*second,"priority",0);
    first->setTrackName("Protected vocal");second->setTrackName("Guitar");
    juce::AudioBuffer<float> a(2,128),b(2,128),mix(2,128);juce::MidiBuffer midi;
    float maximum=0;double protectedError=0,otherReduction=0;bool valid=false;
    std::int64_t clock=0;
    for(int block=0;block<1450;++block){
        for(int n=0;n<128;++n)for(int ch=0;ch<2;++ch){a.setSample(ch,n,source(0,clock+n));b.setSample(ch,n,source(1,clock+n));}
        firstTime.sample=secondTime.sample=masterTime.sample=clock;
        watchAllocation=true;
        if(block%2){first->processBlock(a,midi);second->processBlock(b,midi);}else{second->processBlock(b,midi);first->processBlock(a,midi);}
        mix.makeCopyOf(a,true);mix.addFrom(0,0,b,0,0,128);mix.addFrom(1,0,b,1,0,128);
        master->processBlock(mix,midi);watchAllocation=false;
        maximum=std::max(maximum,mix.getMagnitude(0,128));
        if(block>150){
            for(int n=0;n<128;++n){
                const auto old=clock+n-static_cast<std::int64_t>(nopeaks::kTrackDelaySamples);
                protectedError+=std::abs(a.getSample(0,n)-source(0,old));
                otherReduction+=std::abs(source(1,old))-std::abs(b.getSample(0,n));
            }
        }
        if(block==200){valid=master->getUiState().routingValid;master->captureTarget();}
        // An independent Track reprepare changes its epoch; all connections must recover.
        if(block==900)second->prepareToPlay(48000,512);
        clock+=128;
    }
    const auto ui=master->getUiState();
    require(valid,"Correct unity topology must validate");
    require(ui.routingValid,"Independent Track epoch must recover after reprepare");
    require(ui.targetCaptured,"Capture must finish after two seconds of active source frames");
    require(ui.connectedTracks==2,"Master must list two linked Tracks");
    require(ui.tracks[0].name.isNotEmpty(),"Track metadata must be readable");
    require(maximum<=nopeaks::dbToGain(-1)+2e-6f,"Actual Master output sample peak exceeded ceiling");
    // Exclude restart discontinuity in protect assertion: protected first never restarted.
    require(protectedError<0.01,"Hard-protected Track must null against delayed original");
    require(otherReduction>1,"Cooperative solver must reduce an actual low-priority Track");
    std::cout<<"Host render: peak="<<nopeaks::gainToDb(maximum)<<"dBFS, protected error="<<protectedError<<", low-priority reduction sum="<<otherReduction<<"\n";
    // Downstream gain is outside Track observation and must revoke trust.
    for(int block=0;block<100;++block){
        firstTime.sample=secondTime.sample=masterTime.sample=clock;
        for(int n=0;n<128;++n)for(int ch=0;ch<2;++ch){a.setSample(ch,n,source(0,clock+n));b.setSample(ch,n,source(1,clock+n));}
        first->processBlock(a,midi);second->processBlock(b,midi);
        mix.makeCopyOf(a,true);mix.addFrom(0,0,b,0,0,128);mix.addFrom(1,0,b,1,0,128);mix.applyGain(0.5f);
        master->processBlock(mix,midi);clock+=128;
    }
    require(!master->getUiState().routingValid,"Post-insert fader mismatch must revoke Track control");
    require(audioAllocations.load()==0,"Audio callbacks allocated memory");
    juce::MemoryBlock state;master->getStateInformation(state);master->resetTarget();master->setStateInformation(state.getData(),static_cast<int>(state.getSize()));
    require(master->getUiState().targetCaptured,"Captured target must survive state restoration");
}
}
void* operator new(std::size_t size){if(watchAllocation.load())++audioAllocations;if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
int main(int argc,char** argv){
    try{
        juce::ScopedJuceInitialiser_GUI juce;
        if(argc==3 && std::string(argv[1])=="--screenshots"){screenshots(argv[2]);return 0;}
        verifyStoppedMonitoring();verifyBypass();verifySession();verifyWidthAmount();
        std::cout<<"Actual processor host tests passed; zero callback allocations\n";return 0;
    }catch(const std::exception& e){watchAllocation=false;std::cerr<<"Host test failed: "<<e.what()<<'\n';return 1;}
}
