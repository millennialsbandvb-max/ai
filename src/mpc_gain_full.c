/* Full-contract MPC VST2 stereo gain diagnostic effect. MIT. */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
typedef struct AEffect AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect*,int32_t,int32_t,intptr_t,void*,float);
struct AEffect{
 int32_t magic; intptr_t(*dispatcher)(AEffect*,int32_t,int32_t,intptr_t,void*,float);
 void(*process)(AEffect*,float**,float**,int32_t);
 void(*setParameter)(AEffect*,int32_t,float); float(*getParameter)(AEffect*,int32_t);
 int32_t numPrograms,numParams,numInputs,numOutputs,flags; intptr_t resvd1,resvd2;
 int32_t initialDelay,realQualities,offQualities; float ioRatio; void*object,*user;
 int32_t uniqueID,version; void(*processReplacing)(AEffect*,float**,float**,int32_t);
 void(*processDoubleReplacing)(AEffect*,double**,double**,int32_t); char future[56];
};
enum{effOpen=0,effClose=1,effGetParamLabel=6,effGetParamDisplay=7,effGetParamName=8,
 effSetSampleRate=10,effSetBlockSize=11,effMainsChanged=12,effCanBeAutomated=26,
 effGetPlugCategory=35,effGetEffectName=45,effGetVendorString=47,effGetProductString=48,
 effGetVendorVersion=49,effCanDo=51,effGetVstVersion=58};
static float g=1.0f;
static intptr_t disp(AEffect*e,int32_t op,int32_t idx,intptr_t v,void*p,float o){
 (void)e;(void)v;(void)o;
 switch(op){
 case effOpen:case effClose:case effSetSampleRate:case effSetBlockSize:case effMainsChanged:return 1;
 case effGetPlugCategory:return 1;
 case effGetEffectName:case effGetProductString:strcpy((char*)p,"MPC Gain Full");return 1;
 case effGetVendorString:strcpy((char*)p,"sd88me-test");return 1;
 case effGetVendorVersion:return 1001;
 case effGetVstVersion:return 2400;
 case effCanBeAutomated:return idx==0;
 case effGetParamName:if(idx==0){strcpy((char*)p,"Gain");return 1;}return 0;
 case effGetParamLabel:if(idx==0){strcpy((char*)p,"dB");return 1;}return 0;
 case effGetParamDisplay:if(idx) return 0; if(g<=0.0001f)strcpy((char*)p,"-inf");else snprintf((char*)p,8,"%.1f",20.0*log10f(g));return 1;
 case effCanDo: if(!p)return -1; return (!strcmp((char*)p,"receiveVstTimeInfo"))?1:-1;
 default:return 0;
 }}
static void setp(AEffect*e,int32_t i,float v){(void)e;if(i==0)g=v<0?0:v>1?1:v;}
static float getp(AEffect*e,int32_t i){(void)e;return i?0:g;}
static void repl(AEffect*e,float**in,float**out,int32_t n){(void)e;for(int32_t i=0;i<n;i++){out[0][i]=in[0][i]*g;out[1][i]=in[1][i]*g;}}
static void accum(AEffect*e,float**in,float**out,int32_t n){(void)e;for(int32_t i=0;i<n;i++){out[0][i]+=in[0][i]*g;out[1][i]+=in[1][i]*g;}}
static AEffect fx;
__attribute__((visibility("default"))) AEffect*VSTPluginMain(audioMasterCallback m){
 memset(&fx,0,sizeof fx); fx.magic=0x56737450; fx.dispatcher=disp; fx.process=accum;
 fx.setParameter=setp;fx.getParameter=getp;fx.processReplacing=repl;fx.numParams=1;fx.numInputs=2;fx.numOutputs=2;
 fx.flags=1<<4;fx.uniqueID=0x46476e46;fx.version=1001;fx.user=(void*)m;return &fx;
}
