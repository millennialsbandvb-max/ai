#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include "NAM/get_dsp.h"
#include "NAM/dsp.h"

typedef struct AEffect AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect*,int32_t,int32_t,intptr_t,void*,float);
struct AEffect { int32_t magic; intptr_t(*dispatcher)(AEffect*,int32_t,int32_t,intptr_t,void*,float); void(*process)(AEffect*,float**,float**,int32_t); void(*setParameter)(AEffect*,int32_t,float); float(*getParameter)(AEffect*,int32_t); int32_t numPrograms,numParams,numInputs,numOutputs,flags; intptr_t resvd1,resvd2; int32_t initialDelay,realQualities,offQualities; float ioRatio; void*object,*user; int32_t uniqueID,version; void(*processReplacing)(AEffect*,float**,float**,int32_t); void(*processDoubleReplacing)(AEffect*,double**,double**,int32_t); char future[56]; };
enum { effOpen=0,effClose=1,effGetParamLabel=6,effGetParamDisplay=7,effGetParamName=8,effSetSampleRate=10,effSetBlockSize=11,effMainsChanged=12,effGetPlugCategory=35,effGetEffectName=45,effGetVendorString=47,effGetProductString=48,effGetVendorVersion=49,effCanDo=51,effGetVstVersion=58 };
enum { effFlagsCanReplacing=1<<4 };
struct State { AEffect fx{}; std::unique_ptr<nam::DSP> dsp; float inGain=1.f,outGain=1.f,mix=1.f; double sr=44100.0; int block=128; std::string status; };
static float dbgain(float n){ float db=-18.f+36.f*n; return std::pow(10.f,db/20.f); }
static void load(State*s){
  const char* paths[]={"/sdcard/NAM/default.nam","/sdcard/vst/default.nam"};
  for(auto p:paths){ try{ if(std::filesystem::exists(p)){ nam::DspLoadOptions opt; opt.prewarm=false; s->dsp=nam::get_dsp(std::filesystem::path(p),opt); s->dsp->Reset(s->sr,s->block); s->status=p; return; }}catch(const std::exception&e){s->status=e.what();}}
  s->status="default.nam not found";
}
static void proc(AEffect*e,float**in,float**out,int32_t n){ State*s=(State*)e->object; if(!out||!out[0]||!out[1])return; if(!s->dsp){ for(int i=0;i<n;i++){float x=(in&&in[0]?in[0][i]:0);out[0][i]=x;out[1][i]=x;} return;} std::vector<float> ib(n),ob(n); for(int i=0;i<n;i++)ib[i]=(in&&in[0]?in[0][i]:0)*s->inGain; NAM_SAMPLE* ip[1]={ib.data()}; NAM_SAMPLE* op[1]={ob.data()}; s->dsp->process(ip,op,n); for(int i=0;i<n;i++){float dry=(in&&in[0]?in[0][i]:0);float wet=ob[i]*s->outGain;float y=dry*(1-s->mix)+wet*s->mix;out[0][i]=y;out[1][i]=y;}}
static void setp(AEffect*e,int32_t i,float v){State*s=(State*)e->object;if(v<0)v=0;if(v>1)v=1;if(i==0)s->inGain=dbgain(v);else if(i==1)s->outGain=dbgain(v);else if(i==2)s->mix=v;}
static float getp(AEffect*e,int32_t i){State*s=(State*)e->object;if(i==2)return s->mix;return .5f;}
static void cp(void*p,const char*s,size_t n){std::strncpy((char*)p,s,n-1);((char*)p)[n-1]=0;}
static intptr_t disp(AEffect*e,int32_t op,int32_t idx,intptr_t v,void*p,float o){State*s=(State*)e->object;switch(op){case effOpen:return 1;case effClose:delete s;return 1;case effGetPlugCategory:return 1;case effGetEffectName:case effGetProductString:cp(p,"NAM MPC",32);return 1;case effGetVendorString:cp(p,"MPC-NAM",32);return 1;case effGetVendorVersion:return 1000;case effGetVstVersion:return 2400;case effGetParamName:if(idx==0)cp(p,"Input",32);else if(idx==1)cp(p,"Output",32);else cp(p,"Mix",32);return 1;case effGetParamLabel:if(idx<2)cp(p,"dB",8);else cp(p,"%",8);return 1;case effGetParamDisplay:if(idx==2)std::snprintf((char*)p,16,"%.0f",s->mix*100);else std::snprintf((char*)p,16,"%.1f",20*std::log10(idx?s->outGain:s->inGain));return 1;case effSetSampleRate:s->sr=o;if(s->dsp)s->dsp->Reset(s->sr,s->block);return 1;case effSetBlockSize:s->block=(int)v;if(s->dsp)s->dsp->Reset(s->sr,s->block);return 1;case effMainsChanged:if(v&&!s->dsp)load(s);return 1;case effCanDo:return -1;default:return 0;}}
extern "C" __attribute__((visibility("default"))) AEffect* VSTPluginMain(audioMasterCallback){State*s=new State();AEffect*e=&s->fx;e->magic=0x56737450;e->dispatcher=disp;e->process=proc;e->processReplacing=proc;e->setParameter=setp;e->getParameter=getp;e->numParams=3;e->numInputs=2;e->numOutputs=2;e->flags=effFlagsCanReplacing;e->uniqueID=0x4e414d50;e->version=1000;e->object=s;load(s);return e;}
