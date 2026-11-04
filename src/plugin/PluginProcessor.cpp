#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>

namespace {
constexpr float silencePower = 1.0e-12f;
constexpr std::array<const char*,15> parameterIds {"group","session","priority","peakAmount","toneAmount",
    "widthAmount","ceiling","conflictAmount","protect","peakEnabled","toneEnabled","widthEnabled",
    "safetyEnabled","cooperative","bypass"};
std::array<float,12> normalizedDb(const std::array<float,12>& powers) {
    std::array<float,12> result{};
    float total = 0;
    for (float p : powers) total += std::max(0.0f,p);
    for (std::size_t b=0;b<12;++b)
        result[b] = total > silencePower ? 10.0f*std::log10(std::max(silencePower,powers[b])/total) : -100.0f;
    return result;
}
}

juce::AudioProcessorValueTreeState::ParameterLayout NoPeaksAudioProcessor::makeParameters() {
    juce::AudioProcessorValueTreeState::ParameterLayout p;
    auto number = [&](const char* id,const char* name,float lo,float hi,float value) {
        p.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{id,1}, name,
            juce::NormalisableRange<float>{lo,hi,0.01f}, value));
    };
    auto toggle = [&](const char* id,const char* name,bool value) {
        p.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{id,1},name,value));
    };
    juce::StringArray groups;
    for (int i=1;i<=16;++i) groups.add(juce::String(i));
    p.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{"group",1},"Link group",groups,0,
        juce::AudioParameterChoiceAttributes{}.withAutomatable(false)));
    p.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID{"session",1},"Project ID",1,9999,1,
        juce::AudioParameterIntAttributes{}.withAutomatable(false)));
    number("priority","Priority",0,100,50);
    number("peakAmount","Peak amount",0,100,100);
    number("toneAmount","Tone amount",0,100,50);
    number("widthAmount","Width amount",0,100,50);
    number("ceiling","Ceiling",-18,0,-1);
    number("conflictAmount","Conflict amount",0,100,20);
    toggle("protect","Protect",false);
    toggle("peakEnabled","Peaks",true);
    toggle("toneEnabled","Tone",true);
    toggle("widthEnabled","Width",true);
    toggle("safetyEnabled","Master safety",true);
    toggle("cooperative","Track control",false);
    toggle("bypass","Bypass",false);
    return p;
}

NoPeaksAudioProcessor::NoPeaksAudioProcessor(bool master)
    : AudioProcessor(BusesProperties().withInput("Input",juce::AudioChannelSet::stereo(),true)
                                      .withOutput("Output",juce::AudioChannelSet::stereo(),true)),
      parameters(*this,nullptr,"NOPEAKS_STATE",makeParameters()), master_(master) {
    for(std::size_t i=0;i<parameterIds.size();++i)
        parameterValues_[i]=parameters.getRawParameterValue(juce::String(parameterIds[i]));
    for (std::size_t b=0;b<12;++b) { uiBands_[b]=-100; uiLinkedBands_[b]=-100; targetDb_[b]=-100; }
    setLatencySamples(master_ ? nopeaks::kSafetyLatencySamples : nopeaks::kTrackDelaySamples);
    refreshConnection();
    startTimerHz(10);
}
NoPeaksAudioProcessor::Connection::~Connection() {
    if (bus) {
        if (track) bus->releaseTrack(track);
        if (masterToken) bus->releaseMaster(masterToken);
    }
}
NoPeaksAudioProcessor::~NoPeaksAudioProcessor() {
    stopTimer(); connection_.store(nullptr);
    connections_.clear();
}
const juce::String NoPeaksAudioProcessor::getName() const { return master_?"NOPEAKS Master":"NOPEAKS Track"; }
float NoPeaksAudioProcessor::parameter(const char* id) const noexcept {
    for(std::size_t i=0;i<parameterIds.size();++i)
        if(std::strcmp(id,parameterIds[i])==0)
            return parameterValues_[i]->load(std::memory_order_relaxed);
    return 0;
}
bool NoPeaksAudioProcessor::isBusesLayoutSupported(const BusesLayout& l) const {
    const auto in=l.getMainInputChannelSet(),out=l.getMainOutputChannelSet();
    return in==out && (out==juce::AudioChannelSet::mono() || out==juce::AudioChannelSet::stereo());
}
void NoPeaksAudioProcessor::refreshConnection() {
    const std::lock_guard<std::mutex> lock(setupMutex_);
    const int group=juce::jlimit(1,16,1+static_cast<int>(parameter("group")));
    const int session=juce::jlimit(1,9999,static_cast<int>(parameter("session")));
    auto* old=connection_.load();
    if (old && old->group==group && old->session==session && old->bus->isOpen()) {
        if (master_) {
            if (!old->masterToken || !old->bus->heartbeatMaster(old->masterToken))
                old->masterToken=old->bus->acquireMaster();
            connections_.erase(std::remove_if(connections_.begin(),connections_.end(),[&](const auto& item){
                return item.get()!=old && item.get()!=audioHazard_.load(std::memory_order_seq_cst);
            }),connections_.end());
            return;
        } else if (old->track && old->bus->heartbeatTrack(old->track)) {
            connections_.erase(std::remove_if(connections_.begin(),connections_.end(),[&](const auto& item){
                return item.get()!=old && item.get()!=audioHazard_.load(std::memory_order_seq_cst);
            }),connections_.end());
            return;
        }
    }
    auto next=std::make_unique<Connection>();
    next->group=group;next->session=session;
    next->generation=++connectionGeneration_;
    next->bus=std::make_unique<nopeaks::SharedBus>(group,static_cast<std::uint64_t>(session));
    if (next->bus->isOpen()) {
        if (master_) next->masterToken=next->bus->acquireMaster();
        else {
            next->track=next->bus->claimTrack();
            next->bus->setTrackName(next->track,getTrackName().toRawUTF8());
        }
    }
    auto* raw=next.get();
    connections_.push_back(std::move(next));
    connection_.store(raw,std::memory_order_seq_cst);
    // Hazard publication and recheck in the callback protect the one mapping
    // currently in use. Retired mappings are reclaimed off the audio thread.
    if (old) {
        if (old->track) old->bus->releaseTrack(old->track);
        if (old->masterToken) old->bus->releaseMaster(old->masterToken);
    }
    connections_.erase(std::remove_if(connections_.begin(),connections_.end(),[&](const auto& item){
        return item.get()!=raw && item.get()!=audioHazard_.load(std::memory_order_seq_cst);
    }),connections_.end());
}
void NoPeaksAudioProcessor::timerCallback() { refreshConnection(); }
void NoPeaksAudioProcessor::prepareToPlay(double sr,int maximumBlockSize) {
    rate_=sr;maximumBlock_=maximumBlockSize;
    analyzer_.prepare(sr);tonal_.prepare(sr);delay_.prepare(nopeaks::kTrackDelaySamples);limiter_.prepare(sr);
    havePosition_=false;haveSolved_=false;frameFill_=0;routingGood_=0;routingValid_=false;
    refreshConnection();
}
void NoPeaksAudioProcessor::releaseResources() { havePosition_=false;routingValid_=false; }
std::int64_t NoPeaksAudioProcessor::frameStart(std::int64_t p) noexcept {
    auto rem=p%static_cast<std::int64_t>(nopeaks::kFrameSize);
    if (rem<0) rem+=static_cast<std::int64_t>(nopeaks::kFrameSize);
    return p-rem;
}
void NoPeaksAudioProcessor::resetTimeline(std::int64_t position) {
    ++epoch_; if (!epoch_) ++epoch_;
    expectedPosition_=position;havePosition_=true;frameFill_=0;
    analyzer_.reset();delay_.reset();tonal_.reset();limiter_.reset();
    smoothedStatistics_={};
    slowTone_.fill(0);slowSide_.fill(0);currentGain_=previousGain_=nextGain_=1;
    routingGood_=routingMisses_=0;routingValid_=false;haveSolved_=false;
    widthState_={};widthTokens_.fill(0);
    captureFramesRemaining_=0;targetCapturing_=false;
}
void NoPeaksAudioProcessor::publishBands(const nopeaks::BandStatistics& stats) {
    const float alpha=1.0f-std::exp(-static_cast<float>(stats.samples)/(static_cast<float>(rate_)*0.25f));
    auto smooth=[&](auto& state,const auto& input){for(std::size_t b=0;b<12;++b)state[b]+=alpha*(input[b]-state[b]);};
    smooth(smoothedStatistics_.leftPower,stats.leftPower);smooth(smoothedStatistics_.rightPower,stats.rightPower);
    smooth(smoothedStatistics_.midPower,stats.midPower);smooth(smoothedStatistics_.sidePower,stats.sidePower);
    smooth(smoothedStatistics_.crossPower,stats.crossPower);
    std::array<float,12> powers{};
    for(std::size_t b=0;b<12;++b)powers[b]=0.5f*(smoothedStatistics_.leftPower[b]+smoothedStatistics_.rightPower[b]);
    const auto shape=normalizedDb(powers);
    for (std::size_t b=0;b<12;++b) {
        const float power=powers[b];
        const float db=10.0f*std::log10(std::max(silencePower,power));
        uiBands_[b].store(master_?shape[b]:db,std::memory_order_relaxed);
        masterMid_[b]=smoothedStatistics_.midPower[b]; masterSide_[b]=smoothedStatistics_.sidePower[b];
    }
}
float NoPeaksAudioProcessor::commandGain(Connection* c,nopeaks::TrackHandle track,
    std::int64_t tag,std::uint64_t epoch,unsigned offset) const noexcept {
    if (!c || !c->bus || !synchronized_) return 1;
    nopeaks::ControlFrame now,prev,next;
    const float g=c->bus->readControl(track,tag,epoch,now) ? now.attenuation : 1.0f;
    const float pg=c->bus->readControl(track,tag-128,epoch,prev) ? prev.attenuation : 1.0f;
    const float ng=c->bus->readControl(track,tag+128,epoch,next) ? next.attenuation : 1.0f;
    float result=g;
    constexpr unsigned ramp=32;
    if (offset<ramp && pg<g) result=std::min(result,pg+(g-pg)*static_cast<float>(offset)/ramp);
    if (offset>=128-ramp && ng<g) result=std::min(result,g+(ng-g)*static_cast<float>(offset-(128-ramp))/ramp);
    return juce::jlimit(0.0f,1.0f,result);
}
void NoPeaksAudioProcessor::trackSample(float& left,float& right,std::int64_t position,Connection* c) {
    const bool bypass=blockBypass_;
    const auto tag=frameStart(position);
    const unsigned offset=static_cast<unsigned>(position-tag);
    if (offset==0) {
        frame_=nopeaks::TelemetryFrame{};frame_.samplePosition=tag;frame_.timelineEpoch=epoch_;
        frame_.sampleRate=rate_;frame_.priority=parameter("priority")*0.01f;
        frame_.protectedTrack=blockProtect_ || bypass;
        frameFill_=0;
        nopeaks::ControlFrame control;
        const bool live=c && c->track && synchronized_ &&
            c->bus->readLatestControl(c->track,control) && control.timelineEpoch==epoch_ &&
            control.samplePosition<=position && position-control.samplePosition<4096 && !frame_.protectedTrack;
        for (std::size_t b=0;b<12;++b) {
            slowTone_[b]=live?control.toneDb[b]:0;
            slowSide_[b]=live?control.sideDb[b]:0;
        }
        float toneBudget=0, sideBudget=0;
        for (std::size_t b=0;b<12;++b) {toneBudget-=slowTone_[b];sideBudget-=slowSide_[b];}
        for (std::size_t b=0;b<12;++b) {
            if(toneBudget>6) slowTone_[b]*=6.0f/toneBudget;
            if(sideBudget>12) slowSide_[b]*=12.0f/sideBudget;
        }
        tonal_.setTargets(slowTone_,slowSide_);
        const auto outputTag=tag-static_cast<std::int64_t>(nopeaks::kTrackDelaySamples);
        nopeaks::ControlFrame now,prev,next;
        currentGain_=c && synchronized_ && c->bus->readControl(c->track,outputTag,epoch_,now)?now.attenuation:1;
        previousGain_=c && synchronized_ && c->bus->readControl(c->track,outputTag-128,epoch_,prev)?prev.attenuation:1;
        nextGain_=c && synchronized_ && c->bus->readControl(c->track,outputTag+128,epoch_,next)?next.attenuation:1;
        if (bypass || frame_.protectedTrack) currentGain_=previousGain_=nextGain_=1;
    }
    // Analysis powers describe the unmodified source; samples describe the
    // actual post-EQ signal that is held in the lookahead delay.
    analyzer_.processSample(left,right);
    const auto processed=tonal_.processSample(left,right);
    const auto wet=frame_.protectedTrack ? nopeaks::StereoSample{left,right} : processed;
    if(offset==127)for(std::size_t b=0;b<12;++b) {
        uiTone_[b]=frame_.protectedTrack?0:tonal_.currentToneDb()[b];
        uiSide_[b]=frame_.protectedTrack?0:tonal_.currentSideDb()[b];
    }
    frame_.left[offset]=wet.left;frame_.right[offset]=wet.right;
    ++frameFill_;
    if (offset==127) {
        const auto stats=analyzer_.statisticsAndReset();
        publishBands(stats);
        for (std::size_t b=0;b<12;++b) {
            frame_.power[b]=0.5f*(smoothedStatistics_.leftPower[b]+smoothedStatistics_.rightPower[b]);
            frame_.midPower[b]=smoothedStatistics_.midPower[b];frame_.sidePower[b]=smoothedStatistics_.sidePower[b];
            frame_.cross[b]=smoothedStatistics_.crossPower[b];
        }
        if (frameFill_==128 && c && c->track && synchronized_) c->bus->publish(c->track,frame_);
    }
    auto out=delay_.processSample(wet.left,wet.right);
    float gain=currentGain_;
    if (offset<32 && previousGain_<gain) gain=std::min(gain,previousGain_+(currentGain_-previousGain_)*offset/32.0f);
    if (offset>=96 && nextGain_<gain) gain=std::min(gain,currentGain_+(nextGain_-currentGain_)*(offset-96)/32.0f);
    left=out.left*gain;right=out.right*gain;
    uiGr_.store(-nopeaks::gainToDb(std::max(1.0e-6f,gain)),std::memory_order_relaxed);
}
void NoPeaksAudioProcessor::masterSample(float& left,float& right,std::int64_t position,Connection* c) {
    const auto tag=frameStart(position);
    const auto offset=static_cast<unsigned>(position-tag);
    if (offset==0) {frame_.samplePosition=tag;frameFill_=0;}
    frame_.left[offset]=left;frame_.right[offset]=right;++frameFill_;
    analyzer_.processSample(left,right);
    if (offset==127) {
        publishBands(analyzer_.statisticsAndReset());
        if (frameFill_==128 && synchronized_) completeMasterFrame(c);
    }
    const auto out=limiter_.processSample(left,right);
    left=out.left;right=out.right;
    uiGr_.store(limiter_.gainReductionDb(),std::memory_order_relaxed);
}
bool NoPeaksAudioProcessor::getAlignedFrames(Connection* c,std::int64_t tag,std::uint64_t) {
    if (!trackCount_) return false;
    for (std::size_t i=0;i<trackCount_;++i)
        if (!c->bus->readFrame(trackInfos_[i].handle,tag,trackInfos_[i].timelineEpoch,workFrames_[i]) ||
            std::abs(workFrames_[i].sampleRate-rate_)>0.1 || workFrames_[i].sampleCount!=128) return false;
    return true;
}
void NoPeaksAudioProcessor::updateRouting(Connection* c) {
    if (!c || !c->masterToken || !trackCount_) {routingValid_=false;return;}
    const auto oldTag=frame_.samplePosition-static_cast<std::int64_t>(nopeaks::kTrackDelaySamples);
    const auto sharedEpoch=trackInfos_[0].timelineEpoch;
    if (!getAlignedFrames(c,oldTag,sharedEpoch)) {
        if (++routingMisses_>72) {routingGood_=0;routingValid_=false;}
        return;
    }
    predictedLeft_.fill(0);predictedRight_.fill(0);
    for (std::size_t t=0;t<trackCount_;++t) {
        nopeaks::ControlFrame now,prev,next;
        const auto handle=trackInfos_[t].handle;
        const auto e=workFrames_[t].timelineEpoch;
        const float current=c->bus->readControl(handle,oldTag,e,now)?now.attenuation:1;
        const float previous=c->bus->readControl(handle,oldTag-128,e,prev)?prev.attenuation:1;
        const float following=c->bus->readControl(handle,oldTag+128,e,next)?next.attenuation:1;
        for (unsigned n=0;n<128;++n) {
            float g=current;
            if(n<32 && previous<g)g=std::min(g,previous+(current-previous)*n/32.0f);
            if(n>=96 && following<g)g=std::min(g,current+(following-current)*(n-96)/32.0f);
            predictedLeft_[n]+=g*workFrames_[t].left[n];predictedRight_[n]+=g*workFrames_[t].right[n];
        }
    }
    const auto match=nopeaks::routingMatch(predictedLeft_.data(),predictedRight_.data(),
        frame_.left.data(),frame_.right.data(),128);
    routingMisses_=0;
    if (match.trusted) {if (routingGood_<1000) ++routingGood_;}
    else {routingGood_=0;routingValid_=false;}
    if (routingGood_>=24) routingValid_=true;
}
void NoPeaksAudioProcessor::completeMasterFrame(Connection* c) {
    if (!c || !c->bus->isOpen() || !c->masterToken ||
        c->bus->currentMasterToken()!=c->masterToken) {routingValid_=false;return;}
    trackCount_=c->bus->activeTracks(trackInfos_);
    // Track starts and stops invalidate the routing proof; validation must be
    // earned again even if the total number happened to remain unchanged.
    std::uint64_t topology=1469598103934665603ULL;
    for (std::size_t i=0;i<trackCount_;++i) {
        topology=(topology^trackInfos_[i].handle.token)*1099511628211ULL;
        topology=(topology^trackInfos_[i].timelineEpoch)*1099511628211ULL;
    }
    if (topology!=topology_) {routingGood_=0;routingValid_=false;topology_=topology;}
    updateRouting(c);
    coordinate(c);
}
void NoPeaksAudioProcessor::coordinate(Connection* c) {
    if (!trackCount_) return;
    auto latest=std::numeric_limits<std::int64_t>::max();
    const auto epoch=trackInfos_[0].timelineEpoch;
    for (std::size_t i=0;i<trackCount_;++i) {
        if (!trackInfos_[i].hasFrame) return;
        latest=std::min(latest,trackInfos_[i].latestSamplePosition);
    }
    if (!haveSolved_ || solveEpoch_!=epoch || latest<lastSolved_) {
        lastSolved_=latest-128;haveSolved_=true;solveEpoch_=epoch;
    }
    if (latest-lastSolved_>8192) {routingValid_=false;routingGood_=0;lastSolved_=latest-128;}
    const bool enabled=routingValid_.load() && parameter("cooperative")>0.5f &&
        !blockBypass_ && maximumBlock_<=4096;
    // Up to64 fresh frames per callback, independent of host callback ordering.
    for (unsigned pass=0;pass<64 && lastSolved_<latest;++pass) {
        const auto tag=lastSolved_+128;
        if (!getAlignedFrames(c,tag,epoch)) {lastSolved_=tag;continue;}
        std::array<float,12> aggregate{};
        for (std::size_t t=0;t<trackCount_;++t) {
            auto& v=peakViews_[t];const auto& f=workFrames_[t];
            v.left=f.left.data();v.right=f.right.data();v.priority=f.priority;
            v.protect=f.protectedTrack;v.minimumGain=0.25f;
            bandTracks_[t].power=f.power;bandTracks_[t].midPower=f.midPower;
            bandTracks_[t].sidePower=f.sidePower;bandTracks_[t].priority=f.priority;
            bandTracks_[t].protect=f.protectedTrack;
            for (std::size_t b=0;b<12;++b) aggregate[b]+=f.power[b];
        }
        lastAggregateDb_=normalizedDb(aggregate);
        for(std::size_t b=0;b<12;++b)uiLinkedBands_[b]=lastAggregateDb_[b];
        if (resetRequested_.exchange(false)) {captureFramesRemaining_=0;captureRequested_=false;targetCapturing_=false;}
        if (captureRequested_.exchange(false)) {
            capturePower_.fill(0);captureFramesRemaining_=static_cast<unsigned>(std::ceil(rate_*2.0/128.0));
            targetCapturing_=true;
        }
        if (captureFramesRemaining_>0) {
            float energy=0;for(float p:aggregate)energy+=p;
            if(energy>silencePower) {
                for (std::size_t b=0;b<12;++b) capturePower_[b]+=aggregate[b];
                if(--captureFramesRemaining_==0) {
                    const auto captured=normalizedDb(capturePower_);
                    for(std::size_t b=0;b<12;++b)targetDb_[b]=captured[b];
                    targetCaptured_=true;targetCapturing_=false;
                }
            }
        }
        nopeaks::PeakOptions options;
        options.ceiling=nopeaks::dbToGain(parameter("ceiling"));
        options.amount=enabled && parameter("peakEnabled")>0.5f ? parameter("peakAmount")*0.01f : 0;
        options.iterations=16;
        const auto peak=nopeaks::solvePeaks(peakViews_.data(),trackCount_,128,options);
        std::array<float,12> delta{};
        if (enabled && targetCaptured_.load() && parameter("toneEnabled")>0.5f)
            for (std::size_t b=0;b<12;++b)
                delta[b]=std::clamp(targetDb_[b].load()-lastAggregateDb_[b],-3.0f,0.0f)*parameter("toneAmount")*0.01f;
        const auto tone=nopeaks::solveTone(bandTracks_.data(),trackCount_,delta,3.0f);
        const bool toneOn=enabled && parameter("toneEnabled")>0.5f;
        const auto conflicts=nopeaks::conflictCuts(bandTracks_.data(),trackCount_,
            toneOn ? parameter("conflictAmount")*0.01f*parameter("toneAmount")*0.01f : 0,1.5f);
        const auto width=nopeaks::solveWidth(bandTracks_.data(),trackCount_,masterMid_,masterSide_,0,12.0f);
        for (std::size_t t=0;t<trackCount_;++t) {
            const auto index=static_cast<std::size_t>(trackInfos_[t].handle.index);
            if (widthTokens_[index]!=trackInfos_[t].handle.token) {
                widthTokens_[index]=trackInfos_[t].handle.token;widthState_[index].fill(0);
            }
            nopeaks::ControlFrame control;
            control.samplePosition=tag;control.timelineEpoch=workFrames_[t].timelineEpoch;
            control.attenuation=enabled?peak.gains[t]:1;
            for (std::size_t b=0;b<12;++b) {
                // Twelve overlapping bells: budget aggregate maximum coloration.
                control.toneDb[b]=toneOn?std::clamp(tone.gainsDb[t][b]+conflicts.gainsDb[t][b],-1.5f,0.0f):0;
                const bool widthOn=enabled && parameter("widthEnabled")>0.5f && !workFrames_[t].protectedTrack;
                const float desired=widthOn ? width.gainsDb[t][b] : 0;
                auto& held=widthState_[index][b];
                const float frameSeconds=128.0f/static_cast<float>(rate_);
                if (!widthOn) held=0;
                else if (masterSide_[b]>1.01f*masterMid_[b] && masterSide_[b]>silencePower)
                    held=std::max(-6.0f,std::min(held,desired));
                else held=std::min(0.0f,held+0.6f*frameSeconds);
                control.sideDb[b]=held*parameter("widthAmount")*0.01f;
            }
            c->bus->writeControl(c->masterToken,trackInfos_[t].handle,control);
        }
        lastSolved_=tag;
    }
}
void NoPeaksAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    const int samples=buffer.getNumSamples(),channels=getTotalNumInputChannels();
    for (int ch=channels;ch<buffer.getNumChannels();++ch) buffer.clear(ch,0,samples);
    if (channels<1 || samples<1) return;
    Connection* c=nullptr;
    for(unsigned attempt=0;attempt<3;++attempt) {
        auto* candidate=connection_.load(std::memory_order_seq_cst);
        audioHazard_.store(candidate,std::memory_order_seq_cst);
        if(candidate==connection_.load(std::memory_order_seq_cst)){c=candidate;break;}
    }
    if(!c)audioHazard_.store(nullptr,std::memory_order_seq_cst);
    if(c && c->masterToken)c->bus->heartbeatMaster(c->masterToken);
    std::int64_t start=expectedPosition_;
    bool transport=false;
    const bool previouslySynchronized=synchronized_;
    if (auto* playhead=getPlayHead()) if (auto p=playhead->getPosition())
        if (auto time=p->getTimeInSamples()) {
            transport=p->getIsPlaying();
            if(transport) start=*time;
        }
    synchronized_=transport;transportAvailable_=transport;
    if ((c?c->generation:0)!=audioGeneration_ || !havePosition_ || start!=expectedPosition_ || previouslySynchronized!=transport) {
        audioConnection_=c;audioGeneration_=c?c->generation:0;resetTimeline(start);
    }
    expectedPosition_=start+samples;
    blockBypass_=hostBypassed_ || parameter("bypass")>0.5f;
    blockProtect_=parameter("protect")>0.5f;
    if (!transport) routingValid_=false;
    if (master_) {
        limiter_.setCeilingDb(parameter("ceiling"));
        limiter_.setEnabled(parameter("safetyEnabled")>0.5f && !blockBypass_);
    }
    auto* l=buffer.getWritePointer(0);auto* r=channels>1?buffer.getWritePointer(1):nullptr;
    float inPeak=0,outPeak=0;
    for (int n=0;n<samples;++n) {
        float left=nopeaks::sanitizeSample(l[n]);float right=r?nopeaks::sanitizeSample(r[n]):left;
        inPeak=std::max(inPeak,std::max(std::abs(left),std::abs(right)));
        if (master_) masterSample(left,right,start+n,c);else trackSample(left,right,start+n,c);
        outPeak=std::max(outPeak,std::max(std::abs(left),std::abs(right)));
        l[n]=left;if(r)r[n]=right;
    }
    const float meterRelease=std::exp(-static_cast<float>(samples)/(static_cast<float>(rate_)*0.65f));
    uiInput_.store(std::max(inPeak,uiInput_.load()*meterRelease));
    uiOutput_.store(std::max(outPeak,uiOutput_.load()*meterRelease));
    audioHazard_.store(nullptr,std::memory_order_seq_cst);
}
void NoPeaksAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer,juce::MidiBuffer& midi) {
    hostBypassed_=true;processBlock(buffer,midi);hostBypassed_=false;
}
UiState NoPeaksAudioProcessor::getUiState() const {
    const std::lock_guard<std::mutex> lock(setupMutex_);
    UiState ui;
    ui.inputPeak=uiInput_;ui.outputPeak=uiOutput_;ui.gainReduction=uiGr_;
    ui.routingValid=routingValid_;ui.transportAvailable=transportAvailable_;ui.targetCaptured=targetCaptured_;
    ui.targetCapturing=targetCapturing_;
    for(std::size_t b=0;b<12;++b){ui.bandDb[b]=uiBands_[b];ui.toneDb[b]=uiTone_[b];ui.sideDb[b]=uiSide_[b];ui.targetDb[b]=targetDb_[b];}
    auto* c=connection_.load();if(!c)return ui;
    ui.connectionError=juce::String(c->bus->error());
    ui.masterConnected=c->bus->hasMaster();
    ui.duplicateMaster=master_ && c->masterToken==0 && ui.masterConnected;
    std::array<nopeaks::TrackInfo,64> infos{};
    auto count=c->bus->activeTracks(infos);ui.connectedTracks=static_cast<int>(count);
    if(master_ && count>0)for(std::size_t b=0;b<12;++b)ui.bandDb[b]=uiLinkedBands_[b];
    for(std::size_t t=0;t<count;++t) {
        nopeaks::TelemetryFrame f;nopeaks::ControlFrame control;
        if(!c->bus->readLatest(infos[t].handle,f))continue;
        auto& entry=ui.tracks[static_cast<std::size_t>(ui.trackCount++)];
        entry.name=juce::String::fromUTF8(infos[t].name.data());
        if(entry.name.isEmpty()) entry.name="Track "+juce::String(infos[t].handle.index+1);
        entry.priority=f.priority;entry.protect=f.protectedTrack;entry.connected=true;
        float peak=0;for(std::size_t n=0;n<128;++n)peak=std::max(peak,std::max(std::abs(f.left[n]),std::abs(f.right[n])));
        entry.peakDb=nopeaks::gainToDb(peak);
        if(c->bus->readLatestControl(infos[t].handle,control)) {
            float toneBudget=0,sideBudget=0;
            for(std::size_t b=0;b<12;++b){toneBudget-=control.toneDb[b];sideBudget-=control.sideDb[b];}
            for(std::size_t b=0;b<12;++b){
                const float tone=control.toneDb[b]*(toneBudget>6?6/toneBudget:1);
                const float side=control.sideDb[b]*(sideBudget>12?12/sideBudget:1);
                entry.toneReduction=std::max(entry.toneReduction,-tone);entry.sideReduction=std::max(entry.sideReduction,-side);
                if(master_){ui.toneDb[b]=std::min(ui.toneDb[b],tone);ui.sideDb[b]=std::min(ui.sideDb[b],side);}
            }
        }
    }
    return ui;
}
void NoPeaksAudioProcessor::setTrackName(const juce::String& name) {
    const std::lock_guard<std::mutex> setupLock(setupMutex_);
    const juce::ScopedLock lock(nameLock_);trackName_=name.trim().substring(0,47);
    if(auto* c=connection_.load())if(c->track)c->bus->setTrackName(c->track,trackName_.toRawUTF8());
}
juce::String NoPeaksAudioProcessor::getTrackName() const {
    const juce::ScopedLock lock(nameLock_);return trackName_;
}
void NoPeaksAudioProcessor::getStateInformation(juce::MemoryBlock& dest) {
    auto state=parameters.copyState();state.setProperty("trackName",getTrackName(),nullptr);
    state.setProperty("targetCaptured",targetCaptured_.load(),nullptr);
    for(int b=0;b<12;++b)state.setProperty("target"+juce::String(b),targetDb_[static_cast<std::size_t>(b)].load(),nullptr);
    auto xml=state.createXml();copyXmlToBinary(*xml,dest);
}
void NoPeaksAudioProcessor::setStateInformation(const void* data,int size) {
    if(auto xml=getXmlFromBinary(data,size))if(xml->hasTagName(parameters.state.getType())) {
        auto state=juce::ValueTree::fromXml(*xml);
        setTrackName(state.getProperty("trackName","Track").toString());
        targetCaptured_=state.getProperty("targetCaptured",false);
        for(int b=0;b<12;++b)targetDb_[static_cast<std::size_t>(b)]=std::clamp(static_cast<float>(state.getProperty("target"+juce::String(b),-100.0f)),-100.0f,0.0f);
        parameters.replaceState(state);
    }
}
juce::AudioProcessorEditor* NoPeaksAudioProcessor::createEditor() {return new NoPeaksAudioProcessorEditor(*this);}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {return new NoPeaksAudioProcessor();}
