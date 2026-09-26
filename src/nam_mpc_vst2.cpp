#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "NAM/get_dsp.h"
#include "NAM/dsp.h"

using intptr = intptr_t;
struct AEffect;
typedef intptr (*audioMasterCallback)(AEffect*, int32_t, int32_t, intptr, void*, float);
struct AEffect { int32_t magic; intptr (*dispatcher)(AEffect*,int32_t,int32_t,intptr,void*,float); void (*process)(AEffect*,float**,float**,int32_t); void (*setParameter)(AEffect*,int32_t,float); float (*getParameter)(AEffect*,int32_t); int32_t numPrograms,numParams,numInputs,numOutputs,flags; intptr resvd1,resvd2; int32_t initialDelay,realQualities,offQualities; float ioRatio; void *object,*user; int32_t uniqueID,version; void (*processReplacing)(AEffect*,float**,float**,int32_t); void (*processDoubleReplacing)(AEffect*,double**,double**,int32_t); char future[56]; };
static constexpr int32_t kMagic=0x56737450, kCanReplacing=1<<4;
enum { effOpen=0,effClose=1,effGetParamLabel=6,effGetParamDisplay=7,effGetParamName=8,effSetSampleRate=10,effSetBlockSize=11,effMainsChanged=12,effGetPlugCategory=35,effGetEffectName=45,effGetVendorString=47,effGetProductString=48,effGetVendorVersion=49,effCanDo=51,effGetVstVersion=58 };
struct State { AEffect fx{}; std::unique_ptr<nam::DSP> model; double sr=44100.0; int block=128; float inputDb=0,outputDb=0,mix=1,bypass=0; std::string error; };
static const char* MODEL="/sdcard/NAM/Models/test.nam";
static float gain(float db){return std::pow(10.0f,db/20.0f);} static void copy(void* p,const char*s,int n){std::strncpy((char*)p,s,n-1);((char*)p)[n-1]=0;}
static void load(State*s){try{s->model=nam::get_dsp(std::filesystem::path(MODEL),nam::DspLoadOptions{false}); if(s->model){s->model->SetPrewarmOnReset(false);s->model->Reset(s->sr,s->block);}}catch(const std::exception&e){s->error=e.what();s->model.reset();}}
static void setp(AEffect*e,int32_t i,float v){auto*s=(State*)e->object;v=std::clamp(v,0.0f,1.0f);if(i==0)s->inputDb=-24+48*v;else if(i==1)s->outputDb=-24+48*v;else if(i==2)s->mix=v;else if(i==3)s->bypass=v;}
static float getp(AEffect*e,int32_t i){auto*s=(State*)e->object;if(i==0)return(s->inputDb+24)/48;if(i==1)return(s->outputDb+24)/48;if(i==2)return s->mix;if(i==3)return s->bypass;return 0;}
static void render(AEffect*e,float**in,float**out,int32_t n){auto*s=(State*)e->object;if(!in||!in[0]||!out||!out[0])return;float ig=gain(s->inputDb),og=gain(s->outputDb);std::vector<NAM_SAMPLE>x(n),y(n);for(int i=0;i<n;i++)x[i]=(NAM_SAMPLE)(in[0][i]*ig);NAM_SAMPLE*ip[1]={x.data()};NAM_SAMPLE*op[1]={y.data()};if(s->model&&s->bypass<0.5f)s->model->process(ip,op,n);else for(int i=0;i<n;i++)y[i]=x[i];for(int i=0;i<n;i++){float wet=(float)y[i]*og,dry=in[0][i],v=s->bypass>=0.5f?dry:(dry*(1-s->mix)+wet*s->mix);out[0][i]=v;if(out[1])out[1][i]=v;}}
static intptr dispatch(AEffect*e,int32_t op,int32_t idx,intptr v,void*p,float o){auto*s=(State*)e->object;switch(op){case effOpen:load(s);return 1;case effClose:delete s;return 1;case effSetSampleRate:s->sr=o;return 1;case effSetBlockSize:s->block=(int)v;return 1;case effMainsChanged:return 1;case effGetPlugCategory:return 1;case effGetEffectName:case effGetProductString:copy(p,"NAM MPC",32);return 1;case effGetVendorString:copy(p,"MPC-NAM",32);return 1;case effGetVendorVersion:return 1;case effGetVstVersion:return 2400;case effGetParamName:{const char*n[]={"Input","Output","Mix","Bypass"};if(idx>=0&&idx<4)copy(p,n[idx],32);return 1;}case effGetParamLabel:copy(p,idx<2?"dB":"",8);return 1;case effGetParamDisplay:if(idx==0)std::snprintf((char*)p,24,"%.1f",s->inputDb);else if(idx==1)std::snprintf((char*)p,24,"%.1f",s->outputDb);else if(idx==2)std::snprintf((char*)p,24,"%.0f%%",s->mix*100);else copy(p,s->bypass>=.5f?"On":"Off",24);return 1;case effCanDo:return 0;}return 0;}
extern "C" __attribute__((visibility("default"))) AEffect* VSTPluginMain(audioMasterCallback){auto*s=new State();auto&e=s->fx;e.magic=kMagic;e.dispatcher=dispatch;e.process=render;e.setParameter=setp;e.getParameter=getp;e.numPrograms=1;e.numParams=4;e.numInputs=2;e.numOutputs=2;e.flags=kCanReplacing;e.object=s;e.uniqueID=0x4E414D31;e.version=1;e.processReplacing=render;return &e;}
extern "C" __attribute__((visibility("default"))) AEffect* main_plugin(audioMasterCallback cb){return VSTPluginMain(cb);}
