#include "TruceAAX_Parameters.h"

#include "AAX_CLinearTaperDelegate.h"
#include "AAX_CLogTaperDelegate.h"
#include "AAX_CNumberDisplayDelegate.h"
#include "AAX_CUnitDisplayDelegateDecorator.h"
#include "AAX_CBinaryTaperDelegate.h"
#include "AAX_CBinaryDisplayDelegate.h"
#include "AAX_IDisplayDelegate.h"
#include "AAX_ITaperDelegate.h"
#include "AAX_IMIDINode.h"
#include "AAX_IController.h"
#include "AAX_ITransport.h"
#include "AAX_Enums.h"

#include <cstring>
#include <cstdio>
#include <sstream>
#include <memory>

// ---------------------------------------------------------------------------
// Display delegate
// ---------------------------------------------------------------------------

// Routes AAX parameter display *and* text entry through the plugin's own
// formatter/parser (truce format_value / parse_value) instead of the
// SDK's generic number delegate, so custom units, dB "-inf", enum names,
// etc. round-trip both ways. Captures the instance ctx + param id; AAX
// clones the delegate into each AAX_CParameter, so a stack-local at
// registration is fine.
class TruceAAXDisplayDelegate : public AAX_IDisplayDelegate<float> {
public:
    TruceAAXDisplayDelegate(void* ctx, uint32_t paramID)
        : mCtx(ctx), mParamID(paramID) {}

    AAX_IDisplayDelegate<float>* Clone() const AAX_OVERRIDE {
        return new TruceAAXDisplayDelegate(*this);
    }

    bool ValueToString(float value, AAX_CString* valueString) const AAX_OVERRIDE {
        if (!g_bridge_loaded || !mCtx || !valueString) return false;
        char buf[128];
        buf[0] = 0;
        g_bridge.format_param(mCtx, mParamID, (double)value, buf, sizeof(buf));
        if (buf[0] == 0) return false;
        *valueString = AAX_CString(buf);
        return true;
    }

    bool ValueToString(float value, int32_t /*maxNumChars*/,
                       AAX_CString* valueString) const AAX_OVERRIDE {
        // truce's formatted strings are already compact; the width hint
        // isn't needed to keep them within Pro Tools' field.
        return ValueToString(value, valueString);
    }

    bool StringToValue(const AAX_CString& valueString, float* value) const AAX_OVERRIDE {
        if (!g_bridge_loaded || !mCtx || !value || !g_bridge.parse_param) return false;
        double plain = 0.0;
        if (!g_bridge.parse_param(mCtx, mParamID, valueString.Get(), &plain))
            return false;
        *value = (float)plain;
        return true;
    }

private:
    void* mCtx;
    uint32_t mParamID;
};

// Routes AAX's coefficient<->plain mapping through the plugin's own
// ParamRange (truce_aax_normalize / _denormalize) for skewed shapes AAX
// has no native taper for. Without it, AAX reproduces a skewed param with
// a linear taper while truce's editor uses the skew curve - the two
// disagree, so the knob jumps and recorded automation reads back wrong.
class TruceAAXTaperDelegate : public AAX_ITaperDelegate<float> {
public:
    TruceAAXTaperDelegate(void* ctx, uint32_t paramID, float minV, float maxV)
        : mCtx(ctx), mParamID(paramID), mMin(minV), mMax(maxV) {}

    AAX_ITaperDelegate<float>* Clone() const AAX_OVERRIDE {
        return new TruceAAXTaperDelegate(*this);
    }
    float GetMinimumValue() const AAX_OVERRIDE { return mMin; }
    float GetMaximumValue() const AAX_OVERRIDE { return mMax; }
    float ConstrainRealValue(float value) const AAX_OVERRIDE {
        const float lo = mMin < mMax ? mMin : mMax;
        const float hi = mMin < mMax ? mMax : mMin;
        return value < lo ? lo : (value > hi ? hi : value);
    }
    float NormalizedToReal(double n) const AAX_OVERRIDE {
        if (!g_bridge_loaded || !mCtx || !g_bridge.denormalize)
            return (float)(mMin + n * (mMax - mMin));
        return (float)g_bridge.denormalize(mCtx, mParamID, n);
    }
    double RealToNormalized(float real) const AAX_OVERRIDE {
        if (!g_bridge_loaded || !mCtx || !g_bridge.normalize)
            return mMax != mMin ? (double)((real - mMin) / (mMax - mMin)) : 0.0;
        return g_bridge.normalize(mCtx, mParamID, (double)real);
    }

private:
    void* mCtx;
    uint32_t mParamID;
    float mMin;
    float mMax;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

AAX_CEffectParameters* AAX_CALLBACK TruceAAX_Parameters::Create() {
    return new TruceAAX_Parameters();
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

TruceAAX_Parameters::TruceAAX_Parameters()
    : AAX_CMonolithicParameters()
    , mRustCtx(nullptr) {
}

TruceAAX_Parameters::~TruceAAX_Parameters() {
    if (mRustCtx && g_bridge_loaded) {
        g_bridge.destroy(mRustCtx);
        mRustCtx = nullptr;
    }
}

// ---------------------------------------------------------------------------
// EffectInit - define parameters
// ---------------------------------------------------------------------------

AAX_Result TruceAAX_Parameters::EffectInit() {
    if (!g_bridge_loaded) return AAX_ERROR_NULL_OBJECT;

    // Create the Rust plugin instance
    mRustCtx = g_bridge.create();
    if (!mRustCtx) return AAX_ERROR_NULL_OBJECT;

    // Initialize plugin with sample rate. Pre-size for the
    // worst-case Pro Tools H/W buffer (8192 samples - the cap
    // exposed in the session settings, also used for offline
    // bounce). The plugin allocates internal scratch up to this
    // bound; per-block work in RenderAudio uses the actual
    // `*ioRenderInfo->mNumSamples`. If a host ever delivers a
    // larger block we re-reset there as a defensive fallback.
    AAX_CSampleRate sr = 44100.0;
    Controller()->GetSampleRate(&sr);
    mMaxBlockSize = 8192;
    g_bridge.reset(mRustCtx, (double)sr, mMaxBlockSize);
    // Silence for unpatched/missing input channels (see mSilence).
    mSilence.assign(mMaxBlockSize, 0.0f);
    mNativeEvents.reserve(TRUCE_AAX_NATIVE_EVENT_CAP);
    mNativeSysex.reserve(TRUCE_AAX_SYSEX_POOL_CAP);

    // Channel counts come from the stem format this instance was
    // instantiated with, not the descriptor (which carries only the
    // first layout). Fall back to the descriptor counts if the host
    // reports no stem (older hosts / describe-time defaults).
    AAX_EStemFormat inStem = AAX_eStemFormat_None;
    AAX_EStemFormat outStem = AAX_eStemFormat_None;
    Controller()->GetInputStemFormat(&inStem);
    Controller()->GetOutputStemFormat(&outStem);
    mNumInputChannels = inStem != AAX_eStemFormat_None
        ? AAX_STEM_FORMAT_CHANNEL_COUNT(inStem) : g_descriptor.num_inputs;
    mNumOutputChannels = outStem != AAX_eStemFormat_None
        ? AAX_STEM_FORMAT_CHANNEL_COUNT(outStem) : g_descriptor.num_outputs;

    // Report the plugin's initial latency to the host up front (the
    // idle TimerWakeup then tracks any later changes).
    PushLatencyIfChanged();

    // Register parameters with AAX
    for (uint32_t i = 0; i < g_descriptor.num_params; i++) {
        TruceAaxParamInfo info = {};
        g_bridge.get_param_info(i, &info);

        // Pro Tools' master-bypass UI binds to the well-known
        // parameter ID `cDefaultMasterBypassID`. Use that string for
        // the IS_BYPASS-flagged param so the host's bypass button
        // tracks the param value; everything else gets `truce_p<id>`.
        AAX_CString paramID;
        if (info.id == g_descriptor.bypass_param_id) {
            paramID = cDefaultMasterBypassID;
        } else {
            std::ostringstream idStr;
            idStr << "truce_p" << info.id;
            paramID = AAX_CString(idStr.str().c_str());
        }

        // Pick the taper that matches Rust's `ParamRange` for this
        // param. AAX_CParameter stores the taper internally via
        // `Clone()`, so a stack-local instance per branch is fine -
        // the constructor copies before this scope ends. A
        // matched taper is what stops a log-ranged knob from
        // fighting the editor: with the default linear taper,
        // AAX's normalize/denormalize disagree with Rust's, and
        // the next render block writes back a different plain
        // value than the editor just stored.
        std::unique_ptr<AAX_IParameter> param;
        // truce's format_value already includes the unit, so this routes
        // display + text entry through the plugin (no SDK number/unit
        // decorator, which would double the unit).
        TruceAAXDisplayDelegate display(mRustCtx, info.id);
        if (info.range_type == TRUCE_AAX_RANGE_CUSTOM) {
            // Skewed shapes: route the taper through truce's ParamRange so
            // AAX's coefficient<->plain matches the editor exactly.
            param.reset(new AAX_CParameter<float>(
                paramID,
                AAX_CString(info.name),
                (float)info.default_value,
                TruceAAXTaperDelegate(mRustCtx, info.id,
                                      (float)info.min, (float)info.max),
                display,
                true));
        } else if (info.range_type == TRUCE_AAX_RANGE_LOG) {
            param.reset(new AAX_CParameter<float>(
                paramID,
                AAX_CString(info.name),
                (float)info.default_value,
                AAX_CLogTaperDelegate<float>((float)info.min, (float)info.max),
                display,
                true));
        } else {
            // Linear and Discrete both use a linear taper over
            // [min, max]; for Discrete the step quantization comes
            // from `SetNumberOfSteps` below, not the taper.
            param.reset(new AAX_CParameter<float>(
                paramID,
                AAX_CString(info.name),
                (float)info.default_value,
                AAX_CLinearTaperDelegate<float>((float)info.min, (float)info.max),
                display,
                true));
        }

        // AAX counts step *positions*, not intervals: a binary param has 2
        // steps, an N-value enum has N. `info.step_count` is intervals
        // (values - 1, matching VST3's stepCount and AU's value-string
        // loop), so a Discrete param needs `step_count + 1`. A Continuous
        // param takes a plain automation resolution (128).
        param->SetNumberOfSteps(info.step_count > 0 ? info.step_count + 1 : 128);
        // A stepped param (bool / enum / discrete int) is Discrete so Pro
        // Tools quantizes automation to the steps and offers step
        // navigation instead of treating it as a continuous knob. Its
        // per-step names come from the display delegate (format_value).
        param->SetType(info.step_count > 0 ? AAX_eParameterType_Discrete
                                           : AAX_eParameterType_Continuous);

        AAX_IParameter* rawParam = param.release();
        mParameterManager.AddParameter(rawParam);
        AddSynchronizedParameter(*rawParam);
        mParamIDs.push_back(info.id);
    }

    return AAX_SUCCESS;
}

// ---------------------------------------------------------------------------
// NotificationReceived - host events (offline-bounce render mode)
// ---------------------------------------------------------------------------

AAX_Result TruceAAX_Parameters::NotificationReceived(
    AAX_CTypeID inNotificationType,
    const void* inNotificationData,
    uint32_t inNotificationDataSize) {
    // Pro Tools brackets an offline bounce with these two events even on
    // a real-time (Native) insert - the AAX analogue of AU's
    // OfflineRender flag. Forward the mode; Rust reads it in reset /
    // process. 2 = ProcessMode::Offline, 0 = ProcessMode::Realtime.
    if (inNotificationType == AAX_eNotificationEvent_EnteringOfflineMode ||
        inNotificationType == AAX_eNotificationEvent_ExitingOfflineMode) {
        if (mRustCtx && g_bridge_loaded && g_bridge.set_render_mode) {
            uint32_t mode =
                (inNotificationType == AAX_eNotificationEvent_EnteringOfflineMode) ? 2u : 0u;
            g_bridge.set_render_mode(mRustCtx, mode);
        }
    }
    // Always defer to the base so its own notification bookkeeping runs.
    return AAX_CMonolithicParameters::NotificationReceived(
        inNotificationType, inNotificationData, inNotificationDataSize);
}

// ---------------------------------------------------------------------------
// Dynamic latency - push plugin.latency() changes to the host
// ---------------------------------------------------------------------------

void TruceAAX_Parameters::PushLatencyIfChanged() {
    if (!mRustCtx || !g_bridge_loaded || !g_bridge.latency) return;
    int32_t latency = (int32_t)g_bridge.latency(mRustCtx);
    if (latency == mLastReportedLatency) return;
    // Controller() is valid off the audio thread (EffectInit + the idle
    // TimerWakeup). SetSignalLatency asks the host to recompute delay
    // compensation; it confirms with an AAX_eNotificationEvent_
    // SignalLatencyChanged. Only advance our cache on a clean accept so
    // a rejected push retries on the next tick.
    AAX_IController* controller = Controller();
    if (controller && controller->SetSignalLatency(latency) == AAX_SUCCESS)
        mLastReportedLatency = latency;
}

AAX_Result TruceAAX_Parameters::TimerWakeup() {
    PushLatencyIfChanged();
    return AAX_CMonolithicParameters::TimerWakeup();
}

// ---------------------------------------------------------------------------
// RenderAudio - main processing callback
// ---------------------------------------------------------------------------

void TruceAAX_Parameters::RenderAudio(
    AAX_SInstrumentRenderInfo* ioRenderInfo,
    const TParamValPair* inSynchronizedParamValues[],
    int32_t inNumSynchronizedParamValues)
{
    if (!mRustCtx || !g_bridge_loaded) return;

    // Sync parameter values from AAX to Rust
    for (int32_t i = 0; i < inNumSynchronizedParamValues; i++) {
        const TParamValPair& pv = *inSynchronizedParamValues[i];
        // Extract param index from ID string "truce_pN"
        const char* idStr = pv.first;
        if (strncmp(idStr, "truce_p", 7) == 0) {
            uint32_t id = (uint32_t)atoi(idStr + 7);
            float val;
            if (pv.second && pv.second->GetValueAsFloat(&val))
                g_bridge.set_param(mRustCtx, id, (double)val);
        }
    }

    // Get audio buffers
    int32_t bufferSize = *ioRenderInfo->mNumSamples;

    // A host block beyond the declared cap cannot be made safe by resetting
    // here: reset and scratch growth allocate on the audio thread. Fail closed
    // with silence; a compliant host never takes this path.
    if (bufferSize > 0 && (uint32_t)bufferSize > mMaxBlockSize) {
        if (ioRenderInfo->mAudioOutputs) {
            for (uint32_t ch = 0; ch < mNumOutputChannels; ch++) {
                float* output = ioRenderInfo->mAudioOutputs[ch];
                if (output) std::memset(output, 0, (size_t)bufferSize * sizeof(float));
            }
        }
        return;
    }

    // Build channel pointers. This instance's channel count comes from
    // the stem format captured in EffectInit, so a surround component
    // wires all 6 (or up to 7.1's 8) host pointers - not a stereo-max
    // subset. Passing more channels to Rust than the arrays hold would
    // read past their end, so the counts and the fill loops share the
    // same clamp.
    constexpr uint32_t kMaxChannels = 8; // 7.1 DTS, the widest main stem we register
    // The input array carries the appended sidechain after the main
    // channels, so it needs room for a full-width main PLUS the declared
    // sidechain width. Sizing it to kMaxChannels alone silently dropped the
    // sidechain of a 7.1 main (numIn already == kMaxChannels), feeding Rust
    // fewer channels than the negotiated layout - an out-of-range read the
    // Rust bridge panics on, leaving the track permanently silent. The
    // sidechain never exceeds a full-width bus, so cap it at kMaxChannels.
    constexpr uint32_t kMaxInputs = kMaxChannels + kMaxChannels;
    const float* inputs[kMaxInputs] = {};
    float* outputs[kMaxChannels] = {};

    uint32_t numIn = mNumInputChannels < kMaxChannels ? mNumInputChannels : kMaxChannels;
    uint32_t numOut = mNumOutputChannels < kMaxChannels ? mNumOutputChannels : kMaxChannels;

    if (ioRenderInfo->mAudioInputs) {
        for (uint32_t ch = 0; ch < numIn; ch++)
            inputs[ch] = ioRenderInfo->mAudioInputs[ch];
    }

    // Append the AAX side-chain after the main input channels, so the
    // plugin sees flat channel indexing (main then sidechain). Pro Tools
    // side-chain is always mono; duplicate it across the plugin's declared
    // sidechain width so a stereo-sidechain plugin gets it on both
    // channels. AddSideChainIn's per-block channel index is AAX's only
    // realtime activation signal: 0 means no source is patched. Keep the
    // declared topology stable, but feed silence until the host supplies a
    // positive index. The `!mSilence.empty()` guard keeps a null out of
    // `inputs` if EffectInit never sized the buffer.
    if (g_descriptor.sidechain_in_channels > 0 && !mSilence.empty()) {
        auto* extInfo = reinterpret_cast<TruceAaxExtendedRenderInfo*>(ioRenderInfo);
        const float* scBuf = nullptr;
        if (ioRenderInfo->mAudioInputs && extInfo->mSideChainP && *extInfo->mSideChainP != 0)
            scBuf = ioRenderInfo->mAudioInputs[*extInfo->mSideChainP];
        for (uint32_t c = 0;
             c < g_descriptor.sidechain_in_channels && numIn < kMaxInputs;
             c++) {
            inputs[numIn++] = scBuf ? scBuf : mSilence.data();
        }
    }

    if (ioRenderInfo->mAudioOutputs) {
        for (uint32_t ch = 0; ch < numOut; ch++)
            outputs[ch] = ioRenderInfo->mAudioOutputs[ch];
    }

    // Build one ordered native event transaction. The two vectors were
    // reserved in EffectInit and never grow beyond their fixed bounds here.
    mNativeEvents.clear();
    mNativeSysex.clear();
    uint32_t inputStatus = TRUCE_AAX_EVENT_END;
    bool sysexInProgress = false;
    uint32_t sysexStart = 0;
    uint32_t sysexDeltaFrames = 0;

    auto shortMessageLength = [](uint8_t status) -> uint32_t {
        if (status >= 0x80 && status <= 0xEF) {
            const uint8_t type = status & 0xF0;
            return (type == 0xC0 || type == 0xD0) ? 2u : 3u;
        }
        switch (status) {
            case 0xF1: case 0xF3: return 2;
            case 0xF2: return 3;
            case 0xF6: case 0xF8: case 0xFA: case 0xFB:
            case 0xFC: case 0xFE: case 0xFF: return 1;
            default: return 0;
        }
    };

    auto appendSysex = [&]() -> bool {
        if (mNativeEvents.size() >= TRUCE_AAX_NATIVE_EVENT_CAP) return false;
        TruceAaxNativeEvent event = {};
        event.sample_offset = sysexDeltaFrames;
        event.kind = TRUCE_AAX_NATIVE_EVENT_SYSEX;
        event.data_len = (uint32_t)mNativeSysex.size() - sysexStart;
        event.sysex = event.data_len == 0 ? nullptr : mNativeSysex.data() + sysexStart;
        mNativeEvents.push_back(event);
        return true;
    };

    if (g_descriptor.wants_input_midi && ioRenderInfo->mInputNode) {
        AAX_IMIDINode* midiNode = ioRenderInfo->mInputNode;
        if (midiNode) {
            AAX_CMidiStream* stream = midiNode->GetNodeBuffer();
            if (stream && stream->mBufferSize > 0) {
                uint32_t previousTimestamp = 0;
                bool haveTimestamp = false;
                auto ingestSysexBytes = [&](const AAX_CMidiPacket& pkt, uint32_t start) {
                    for (uint32_t j = start; j < pkt.mLength; j++) {
                        uint8_t b = pkt.mData[j];
                        if (b == 0xF7) {
                            if (j + 1 != pkt.mLength || !appendSysex())
                                inputStatus = j + 1 != pkt.mLength
                                    ? TRUCE_AAX_EVENT_INVALID : TRUCE_AAX_EVENT_QUEUE_FULL;
                            sysexInProgress = false;
                            return inputStatus == TRUCE_AAX_EVENT_END;
                        }
                        if (b & 0x80) {
                            sysexInProgress = false;
                            inputStatus = TRUCE_AAX_EVENT_INVALID;
                            return false;
                        }
                        if (mNativeSysex.size() >= TRUCE_AAX_SYSEX_POOL_CAP) {
                            inputStatus = TRUCE_AAX_EVENT_QUEUE_FULL;
                            sysexInProgress = false;
                            return false;
                        }
                        mNativeSysex.push_back(b);
                    }
                    return true;
                };

                for (uint32_t i = 0; i < stream->mBufferSize; i++) {
                    const AAX_CMidiPacket& pkt = stream->mBuffer[i];
                    if (inputStatus != TRUCE_AAX_EVENT_END) break;
                    if (pkt.mLength < 1 || pkt.mLength > 4 ||
                        pkt.mTimestamp >= (uint32_t)bufferSize ||
                        (haveTimestamp && pkt.mTimestamp < previousTimestamp)) {
                        inputStatus = TRUCE_AAX_EVENT_INVALID;
                        break;
                    }
                    previousTimestamp = pkt.mTimestamp;
                    haveTimestamp = true;
                    const uint8_t status = pkt.mData[0];
                    if (sysexInProgress) {
                        ingestSysexBytes(pkt, 0);
                        continue;
                    }
                    if (status == 0xF0) {
                        sysexInProgress = true;
                        sysexStart = (uint32_t)mNativeSysex.size();
                        sysexDeltaFrames = pkt.mTimestamp;
                        ingestSysexBytes(pkt, 1);
                        continue;
                    }
                    const uint32_t expected = shortMessageLength(status);
                    bool valid = expected == pkt.mLength;
                    for (uint32_t j = 1; valid && j < pkt.mLength; j++)
                        valid = (pkt.mData[j] & 0x80) == 0;
                    if (!valid) {
                        inputStatus = TRUCE_AAX_EVENT_INVALID;
                        break;
                    }
                    if (mNativeEvents.size() >= TRUCE_AAX_NATIVE_EVENT_CAP) {
                        inputStatus = TRUCE_AAX_EVENT_QUEUE_FULL;
                        break;
                    }
                    TruceAaxNativeEvent event = {};
                    event.sample_offset = pkt.mTimestamp;
                    event.kind = TRUCE_AAX_NATIVE_EVENT_MIDI1;
                    event.data_len = pkt.mLength;
                    for (uint32_t j = 0; j < pkt.mLength; j++) event.midi[j] = pkt.mData[j];
                    mNativeEvents.push_back(event);
                }
                if (inputStatus == TRUCE_AAX_EVENT_END && sysexInProgress)
                    inputStatus = TRUCE_AAX_EVENT_INVALID;
            }
        }
    }

    // Event ingestion is transactional: never hand the plugin a valid prefix
    // when the rest of the host block overflowed or was malformed.
    if (inputStatus != TRUCE_AAX_EVENT_END) {
        mNativeEvents.clear();
        mNativeSysex.clear();
    }

    // Query Pro Tools transport. Each getter is independent so the
    // snapshot remains useful even if the host only answers some of
    // them. All coordinates come back in beats / ticks / samples and
    // are forwarded verbatim to Rust.
    TruceAaxTransportSnapshot transport = {};
    AAX_ITransport* trans = Transport();
    if (trans) {
        bool playing = false;
        if (trans->IsTransportPlaying(&playing) == AAX_SUCCESS) {
            transport.playing = playing ? 1 : 0;
            transport.valid = 1;
        }
        double tempo = 0.0;
        if (trans->GetCurrentTempo(&tempo) == AAX_SUCCESS && tempo > 0.0) {
            transport.tempo = tempo;
            transport.valid = 1;
        }
        int32_t num = 0, den = 0;
        if (trans->GetCurrentMeter(&num, &den) == AAX_SUCCESS) {
            transport.time_sig_num = num;
            transport.time_sig_den = den;
            transport.valid = 1;
        }
        int64_t sampleLoc = 0;
        if (trans->GetCurrentNativeSampleLocation(&sampleLoc) == AAX_SUCCESS) {
            transport.position_samples = (double)sampleLoc;
            transport.valid = 1;
        }
        int64_t tickPos = 0;
        if (trans->GetCurrentTickPosition(&tickPos) == AAX_SUCCESS) {
            // AAX ticks are 1/960000 of a quarter note; convert to beats.
            transport.position_beats = (double)tickPos / 960000.0;
            transport.valid = 1;
        }
        // Bar/beat at the current sample location. GetBarBeatPosition
        // returns zero-based bar + beat indices; convert to a beat
        // count by multiplying bars by the reported meter numerator.
        int32_t bars = 0, beats = 0;
        int64_t barDisplayTicks = 0;
        int64_t samplePos = (int64_t)transport.position_samples;
        if (trans->GetBarBeatPosition(&bars, &beats, &barDisplayTicks, samplePos)
                == AAX_SUCCESS
            && transport.time_sig_num > 0)
        {
            transport.bar_start_beats =
                (double)bars * (double)transport.time_sig_num;
            transport.valid = 1;
        }
        bool loop = false;
        int64_t loopStart = 0, loopEnd = 0;
        if (trans->GetCurrentLoopPosition(&loop, &loopStart, &loopEnd) == AAX_SUCCESS) {
            transport.loop_active = loop ? 1 : 0;
            transport.loop_start_beats = (double)loopStart / 960000.0;
            transport.loop_end_beats = (double)loopEnd / 960000.0;
            transport.valid = 1;
        }
    }

    // Call the strict Rust processing function. A non-END status means the
    // MIDI transaction did not round-trip, so no plugin MIDI output follows.
    uint32_t processStatus = g_bridge.process_native(mRustCtx,
        inputs, outputs,
        numIn, numOut,
        (uint32_t)bufferSize,
        mNativeEvents.data(), (uint32_t)mNativeEvents.size(), inputStatus,
        transport.valid ? &transport : nullptr);
    if (processStatus != TRUCE_AAX_EVENT_END) return;

    // Drain plugin-emitted MIDI to the host. The component descriptor
    // built in `TruceAAX_Describe.cpp` registered an extra `LocalOutput`
    // MIDI node past the end of `AAX_SInstrumentRenderInfo`; recover it
    // by casting `ioRenderInfo` back to the extended struct that the
    // runtime actually populates (the cast is sound - same offsets for
    // the inherited fields, plus one extra slot for `mOutputNode`).
    auto* extendedInfo = reinterpret_cast<TruceAaxExtendedRenderInfo*>(ioRenderInfo);
    AAX_IMIDINode* outputNode = extendedInfo->mOutputNode;
    if (outputNode) {
        g_bridge.begin_output_events(mRustCtx, (uint32_t)bufferSize);
        for (;;) {
            TruceAaxNativeEvent event = {};
            uint32_t status = g_bridge.next_output_event(mRustCtx, &event);
            if (status == TRUCE_AAX_EVENT_END) break;
            if (status != TRUCE_AAX_EVENT_EMITTED) return;

            if (event.kind == TRUCE_AAX_NATIVE_EVENT_MIDI1) {
                AAX_CMidiPacket pkt = {};
                pkt.mTimestamp = event.sample_offset;
                pkt.mLength = event.data_len;
                for (uint32_t j = 0; j < event.data_len; j++) pkt.mData[j] = event.midi[j];
                if (outputNode->PostMIDIPacket(&pkt) != AAX_SUCCESS) return;
                continue;
            }

            // AAX represents SysEx as a consecutive run of <=4-byte packets.
            // Frame the exact inner payload and emit the whole logical event
            // before advancing the Rust cursor, preserving event order.
            const uint32_t totalLen = event.data_len + 2;
            uint32_t pos = 0;
            while (pos < totalLen) {
                AAX_CMidiPacket pkt = {};
                pkt.mTimestamp = event.sample_offset;
                pkt.mLength = totalLen - pos < 4 ? totalLen - pos : 4;
                for (uint32_t j = 0; j < pkt.mLength; j++) {
                    const uint32_t p = pos + j;
                    pkt.mData[j] = p == 0 ? 0xF0
                        : (p + 1 == totalLen ? 0xF7 : event.sysex[p - 1]);
                }
                if (outputNode->PostMIDIPacket(&pkt) != AAX_SUCCESS) return;
                pos += pkt.mLength;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// State (chunk) support
// ---------------------------------------------------------------------------

// The AAX standard control chunk ID. `AAX_CEffectParameters`'s
// `GetChunkIDFromIndex(0)` already returns this value from the
// SDK's default implementation, so we just need our chunk
// handlers to honor the same gate. Without the gate, every chunk
// Pro Tools probes for (preset 'pset', mode 'mode', ...) gets
// our `save_state` blob, and the host's chunk table fills with
// wrong-sized entries — `SMgr_PlugInInst::GetLiveSettings` then
// trips a size-mismatch assertion when two plugins share a
// track. Refusing unknown chunks via `AAX_ERROR_INVALID_CHUNK_ID`
// keeps the host's chunk table truthful.
constexpr AAX_CTypeID kTruceControlsChunkID = 'elck';

// True when `chunkID` is one of the legacy chunk fourccs the plugin
// declared in truce.toml's [plugin.legacy_state] - the ids a
// pre-truce build of this plugin saved its sessions under.
static bool is_legacy_chunk(AAX_CTypeID chunkID) {
    for (uint32_t i = 0; i < g_descriptor.num_legacy_chunk_ids; i++) {
        if ((AAX_CTypeID)g_descriptor.legacy_chunk_ids[i] == chunkID) return true;
    }
    return false;
}

AAX_Result TruceAAX_Parameters::GetNumberOfChunks(int32_t* oNumChunks) const {
    // Truce's own chunk plus the declared legacy ids. Declaring the
    // legacy ids is what makes Pro Tools deliver an old session's
    // chunk to SetChunk; an undeclared chunk is never offered.
    *oNumChunks = 1 + (int32_t)g_descriptor.num_legacy_chunk_ids;
    return AAX_SUCCESS;
}

AAX_Result TruceAAX_Parameters::GetChunkIDFromIndex(int32_t index, AAX_CTypeID* oChunkID) const {
    if (index == 0) {
        *oChunkID = kTruceControlsChunkID;
        return AAX_SUCCESS;
    }
    uint32_t legacy = (uint32_t)(index - 1);
    if (legacy < g_descriptor.num_legacy_chunk_ids) {
        *oChunkID = (AAX_CTypeID)g_descriptor.legacy_chunk_ids[legacy];
        return AAX_SUCCESS;
    }
    *oChunkID = 0;
    return AAX_ERROR_INVALID_CHUNK_INDEX;
}

AAX_Result TruceAAX_Parameters::GetChunkSize(AAX_CTypeID chunkID, uint32_t* oSize) const {
    if (!mRustCtx || !g_bridge_loaded) return AAX_ERROR_NULL_OBJECT;
    if (is_legacy_chunk(chunkID)) {
        // Save direction: truce never writes legacy chunks. Zero size
        // keeps Pro Tools' chunk table truthful without refusing an
        // id we declared.
        *oSize = 0;
        return AAX_SUCCESS;
    }
    if (chunkID != kTruceControlsChunkID) {
        *oSize = 0;
        return AAX_ERROR_INVALID_CHUNK_ID;
    }
    // Serialize once into the pending cache; GetChunk drains it.
    uint8_t* data = nullptr;
    uint32_t len = g_bridge.save_state(mRustCtx, &data);
    mPendingChunk.assign(data, data + len);
    if (data) g_bridge.free_state(data, len);
    *oSize = len;
    return AAX_SUCCESS;
}

AAX_Result TruceAAX_Parameters::GetChunk(AAX_CTypeID chunkID, AAX_SPlugInChunk* oChunk) const {
    if (!mRustCtx || !g_bridge_loaded) return AAX_ERROR_NULL_OBJECT;
    if (is_legacy_chunk(chunkID)) {
        // Save direction pair of GetChunkSize's zero-size answer.
        oChunk->fSize = 0;
        return AAX_SUCCESS;
    }
    if (chunkID != kTruceControlsChunkID) {
        return AAX_ERROR_INVALID_CHUNK_ID;
    }
    // Prefer the blob cached by the immediately-preceding GetChunkSize
    // call. Fall back to a fresh serialize only if Pro Tools violates
    // the usual size-then-copy contract (defensive - shouldn't happen).
    if (mPendingChunk.empty()) {
        uint8_t* data = nullptr;
        uint32_t len = g_bridge.save_state(mRustCtx, &data);
        if (data) {
            mPendingChunk.assign(data, data + len);
            g_bridge.free_state(data, len);
        }
    }
    if (!mPendingChunk.empty() && mPendingChunk.size() <= oChunk->fSize) {
        memcpy(oChunk->fData, mPendingChunk.data(), mPendingChunk.size());
        oChunk->fSize = (uint32_t)mPendingChunk.size();
    }
    mPendingChunk.clear();
    mPendingChunk.shrink_to_fit();
    return AAX_SUCCESS;
}

AAX_Result TruceAAX_Parameters::SetChunk(AAX_CTypeID chunkID, const AAX_SPlugInChunk* iChunk) {
    if (!mRustCtx || !g_bridge_loaded) return AAX_ERROR_NULL_OBJECT;
    if (is_legacy_chunk(chunkID)) {
        // Zero-size legacy chunks are Pro Tools echoing our own save
        // answer back: the ids are advertised (so old sessions offer
        // their chunks for migration), GetChunkSize answers 0 in the
        // save direction, and Pro Tools stores that empty chunk in
        // every new session. Nothing to migrate - succeed, don't
        // error on data the host wrote from our own answer.
        if (!iChunk || iChunk->fSize == 0) return AAX_SUCCESS;
        // An old session's chunk from a pre-truce build: offer the
        // bytes to the plugin's migrate_state hook.
        if (g_bridge.load_state_foreign
            && g_bridge.load_state_foreign(mRustCtx, (uint32_t)chunkID,
                                           (const uint8_t*)iChunk->fData,
                                           iChunk->fSize))
            return AAX_SUCCESS;
        return AAX_ERROR_INVALID_CHUNK_ID;
    }
    if (chunkID != kTruceControlsChunkID) {
        return AAX_ERROR_INVALID_CHUNK_ID;
    }
    g_bridge.load_state(mRustCtx, (const uint8_t*)iChunk->fData, iChunk->fSize);
    return AAX_SUCCESS;
}
