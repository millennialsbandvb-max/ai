/* The VST2 entry point and dispatcher behind audioeffectx.h: turns the host's AEffect calls into the AudioEffectX
 * virtual methods the Airwindows plugins implement. */
#include "audioeffectx.h"

#include <math.h>

static AudioEffectX *self(AEffect *e) { return static_cast<AudioEffectX *>(static_cast<AudioEffect *>(e->object)); }

static intptr_t dispatch(AEffect *e, int32_t op, int32_t index, intptr_t value, void *ptr, float opt) {
    AudioEffectX *fx = self(e);
    const bool param_ok = index >= 0 && index < e->numParams;
    switch (op) {
    case effOpen: fx->open(); return 0;
    case effClose:
        fx->close();
        free(fx->lastChunk);
        delete fx;
        return 1;
    case effGetParamLabel: if (param_ok) fx->getParameterLabel(index, (char *)ptr); else *(char *)ptr = 0; return 0;
    case effGetParamDisplay: if (param_ok) fx->getParameterDisplay(index, (char *)ptr); else *(char *)ptr = 0; return 0;
    case effGetParamName: if (param_ok) fx->getParameterName(index, (char *)ptr); else *(char *)ptr = 0; return 0;
    case effSetSampleRate: fx->setSampleRate(opt); return 0;
    case effSetBlockSize: fx->setBlockSize((VstInt32)value); return 0;
    case effMainsChanged: if (value) fx->resume(); else fx->suspend(); return 0;
    case effGetChunk: {
        void *data = nullptr;
        const VstInt32 n = fx->getChunk(&data, index != 0);
        if (data != fx->lastChunk) { free(fx->lastChunk); fx->lastChunk = data; }
        *(void **)ptr = data;
        return n;
    }
    case effSetChunk: {
        // The plugins read their fixed number of values whatever the size: hand them a zero-padded copy
        if (!ptr || value <= 0) return 0;
        const size_t n = (size_t)value, room = n + 64 * sizeof(double);
        char *copy = (char *)calloc(1, room);
        if (!copy) return 0;
        memcpy(copy, ptr, n);
        const VstInt32 r = fx->setChunk(copy, (VstInt32)n, index != 0);
        free(copy);
        return r;
    }
    case effCanBeAutomated: return param_ok ? 1 : 0;
    case effGetPlugCategory: return fx->getPlugCategory();
    case effGetEffectName: return fx->getEffectName((char *)ptr) ? 1 : 0;
    case effGetVendorString: return fx->getVendorString((char *)ptr) ? 1 : 0;
    case effGetProductString: return fx->getProductString((char *)ptr) ? 1 : 0;
    case effGetVendorVersion: return fx->getVendorVersion();
    case effCanDo: return ptr ? fx->canDo((char *)ptr) : 0;
    case effGetVstVersion: return 2400;
    default: return 0;
    }
}

static void process(AEffect *e, float **in, float **out, int32_t n) { self(e)->processReplacing(in, out, n); }
static void process_double(AEffect *e, double **in, double **out, int32_t n) {
    self(e)->processDoubleReplacing(in, out, n);
}
static void set_parameter(AEffect *e, int32_t i, float v) {
    if (i < 0 || i >= e->numParams) return;   // the plugins throw on an unknown index
    self(e)->setParameter(i, v < 0 ? 0 : v > 1 ? 1 : v);
}
static float get_parameter(AEffect *e, int32_t i) {
    return i >= 0 && i < e->numParams ? self(e)->getParameter(i) : 0.0f;
}

AudioEffect::AudioEffect(audioMasterCallback master, VstInt32 numPrograms, VstInt32 numParams) : audioMaster(master) {
    memset(&cEffect, 0, sizeof cEffect);
    cEffect.magic = VST_MAGIC;
    cEffect.dispatcher = dispatch;
    cEffect.process = process;
    cEffect.setParameter = set_parameter;
    cEffect.getParameter = get_parameter;
    cEffect.processReplacing = process;
    cEffect.processDoubleReplacing = process_double;
    cEffect.numPrograms = numPrograms;
    cEffect.numParams = numParams;
    cEffect.numInputs = cEffect.numOutputs = 2;
    cEffect.ioRatio = 1.0f;
    cEffect.object = static_cast<AudioEffect *>(this);
    cEffect.version = 1;
}

AudioEffect::~AudioEffect() {}

// value text: at most 2 decimals, so it fits the MPC's value labels
void AudioEffect::float2string(float value, char *text, VstInt32 maxLen) {
    const float a = fabsf(value);
    snprintf(text, (size_t)maxLen + 1, a >= 100.f ? "%.0f" : a >= 10.f ? "%.1f" : "%.2f", value);
}
void AudioEffect::int2string(VstInt32 value, char *text, VstInt32 maxLen) {
    snprintf(text, (size_t)maxLen + 1, "%d", (int)value);
}
void AudioEffect::dB2string(float value, char *text, VstInt32 maxLen) {
    if (value <= 0) vst_strncpy(text, "-oo", maxLen);
    else float2string(20.f * log10f(value), text, maxLen);
}
void AudioEffect::Hz2string(float samples, char *text, VstInt32 maxLen) {
    if (samples == 0) vst_strncpy(text, "0", maxLen);
    else float2string(44100.f / samples, text, maxLen);
}
void AudioEffect::ms2string(float samples, char *text, VstInt32 maxLen) {
    float2string(samples * 1000.f / 44100.f, text, maxLen);
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    if (!master) return nullptr;
    AudioEffect *fx = createEffectInstance(master);
    if (!fx) return nullptr;
    fx->cEffect.version = static_cast<AudioEffectX *>(fx)->getVendorVersion();   // e.g. 1000 = 1.0.0
    return fx->getAeffect();
}
