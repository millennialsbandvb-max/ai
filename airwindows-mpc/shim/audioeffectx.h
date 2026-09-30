/* A small stand-in for the Steinberg VST2 SDK's AudioEffect/AudioEffectX classes (audioeffectx.h), which Airwindows'
 * plugin sources are written against but which isn't redistributable. Hand-written on top of nam-mpc's vst2.h: only
 * what the Airwindows plugins use, with the same names and signatures so their sources build unchanged. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vst2.h"

typedef int32_t VstInt32;
typedef intptr_t VstIntPtr;
typedef int32_t VstPlugCategory;
// kVstMaxParamStrLen is 8 in the SDK; JUCE (the MPC's host) passes 256-byte buffers, so names aren't cut short
enum { kVstMaxProgNameLen = 24, kVstMaxParamStrLen = 15, kVstMaxVendorStrLen = 64, kVstMaxProductStrLen = 64,
       kVstMaxEffectNameLen = 32 };
enum { kPlugCategUnknown = 0, kPlugCategSynth = 2, kPlugCategAnalysis = 3, kPlugCategMastering = 4,
       kPlugCategSpacializer = 5, kPlugCategRoomFx = 6 };   // kPlugCategEffect (1) comes from vst2.h

class AudioEffect {
public:
    AudioEffect(audioMasterCallback audioMaster, VstInt32 numPrograms, VstInt32 numParams);
    virtual ~AudioEffect();

    virtual void setParameter(VstInt32 index, float value) { (void)index; (void)value; }
    virtual float getParameter(VstInt32 index) { (void)index; return 0; }
    virtual void processReplacing(float **inputs, float **outputs, VstInt32 sampleFrames) = 0;
    virtual void processDoubleReplacing(double **inputs, double **outputs, VstInt32 sampleFrames) {
        (void)inputs; (void)outputs; (void)sampleFrames;
    }
    virtual VstInt32 getChunk(void **data, bool isPreset) { (void)data; (void)isPreset; return 0; }
    virtual VstInt32 setChunk(void *data, VstInt32 byteSize, bool isPreset) {
        (void)data; (void)byteSize; (void)isPreset; return 0;
    }
    virtual void getProgramName(char *name) { *name = 0; }
    virtual void setProgramName(char *name) { (void)name; }
    virtual void getParameterLabel(VstInt32 index, char *text) { (void)index; *text = 0; }
    virtual void getParameterDisplay(VstInt32 index, char *text) { (void)index; *text = 0; }
    virtual void getParameterName(VstInt32 index, char *text) { (void)index; *text = 0; }
    virtual void setSampleRate(float rate) { sampleRate = rate; }
    virtual void setBlockSize(VstInt32 size) { blockSize = size; }
    virtual void suspend() {}
    virtual void resume() {}
    virtual void open() {}
    virtual void close() {}

    float getSampleRate() { return sampleRate; }
    VstInt32 getBlockSize() { return blockSize; }
    void setNumInputs(VstInt32 n) { cEffect.numInputs = n; }
    void setNumOutputs(VstInt32 n) { cEffect.numOutputs = n; }
    void setUniqueID(VstInt32 id) { cEffect.uniqueID = id; }
    void canProcessReplacing(bool state = true) { flag(effFlagsCanReplacing, state); }
    void canDoubleReplacing(bool state = true) { (void)state; }   // the MPC only uses 32-bit float processing
    void programsAreChunks(bool state = true) { flag(effFlagsProgramChunks, state); }
    void setInitialDelay(VstInt32 delay) { cEffect.initialDelay = delay; }
    AEffect *getAeffect() { return &cEffect; }

    // value text helpers (the SDK's names)
    static void float2string(float value, char *text, VstInt32 maxLen);
    static void int2string(VstInt32 value, char *text, VstInt32 maxLen);
    static void dB2string(float value, char *text, VstInt32 maxLen);
    static void Hz2string(float samples, char *text, VstInt32 maxLen);
    static void ms2string(float samples, char *text, VstInt32 maxLen);

    float sampleRate = 44100.0f;
    VstInt32 blockSize = 1024;
    audioMasterCallback audioMaster;
    AEffect cEffect;
    void *lastChunk = nullptr;   // the plugins allocate each saved chunk and never free it: we free the last one

private:
    void flag(int32_t f, bool on) { cEffect.flags = on ? (cEffect.flags | f) : (cEffect.flags & ~f); }
};

class AudioEffectX : public AudioEffect {
public:
    AudioEffectX(audioMasterCallback audioMaster, VstInt32 numPrograms, VstInt32 numParams)
        : AudioEffect(audioMaster, numPrograms, numParams) {}
    virtual bool getEffectName(char *name) { (void)name; return false; }
    virtual bool getVendorString(char *text) { (void)text; return false; }
    virtual bool getProductString(char *text) { (void)text; return false; }
    virtual VstInt32 getVendorVersion() { return 0; }
    virtual VstPlugCategory getPlugCategory() { return kPlugCategEffect; }
    virtual VstInt32 canDo(char *text) { (void)text; return 0; }
};

static inline char *vst_strncpy(char *dst, const char *src, VstInt32 maxLen) {
    strncpy(dst, src, (size_t)maxLen);
    dst[maxLen] = 0;
    return dst;
}
static inline char *vst_strncat(char *dst, const char *src, VstInt32 maxLen) {
    strncat(dst, src, (size_t)maxLen - strlen(dst));
    dst[maxLen] = 0;
    return dst;
}

AudioEffect *createEffectInstance(audioMasterCallback audioMaster);   // each plugin's .cpp defines this
