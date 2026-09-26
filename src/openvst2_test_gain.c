/*
MIT License

Copyright (c) 2026 MPC VST2 diagnostic build

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
*/
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

typedef struct AEffect AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
struct AEffect {
 int32_t magic;
 intptr_t (*dispatcher)(AEffect*,int32_t,int32_t,intptr_t,void*,float);
 void (*process)(AEffect*,float**,float**,int32_t);
 void (*setParameter)(AEffect*,int32_t,float);
 float (*getParameter)(AEffect*,int32_t);
 int32_t numPrograms,numParams,numInputs,numOutputs,flags;
 intptr_t resvd1,resvd2;
 int32_t initialDelay,realQualities,offQualities;
 float ioRatio;
 void *object,*user;
 int32_t uniqueID,version;
 void (*processReplacing)(AEffect*,float**,float**,int32_t);
 void (*processDoubleReplacing)(AEffect*,double**,double**,int32_t);
 char future[56];
};
enum { effOpen=0,effClose=1,effGetParamLabel=6,effGetParamDisplay=7,effGetParamName=8,
 effCanBeAutomated=26,effGetPlugCategory=35,effGetEffectName=45,effGetVendorString=47,
 effGetProductString=48,effGetVendorVersion=49,effGetVstVersion=58 };
static float gain=1.0f;
static intptr_t dispatch(AEffect*e,int32_t op,int32_t idx,intptr_t v,void*p,float o){
 (void)e;(void)idx;(void)v;(void)o;
 switch(op){
  case effGetPlugCategory:return 1;
  case effGetEffectName:case effGetProductString:strcpy((char*)p,"OpenVST2 Test Gain");return 1;
  case effGetVendorString:strcpy((char*)p,"MPC-NAM-Test");return 1;
  case effGetVendorVersion:return 1000;
  case effGetVstVersion:return 2400;
  case effCanBeAutomated:return 1;
  case effGetParamName:strcpy((char*)p,"Gain");return 1;
  case effGetParamLabel:strcpy((char*)p,"dB");return 1;
  case effGetParamDisplay: if(gain<=0.0001f)strcpy((char*)p,"-inf");else snprintf((char*)p,8,"%.1f",20.0*log10f(gain));return 1;
  case effOpen:case effClose:return 1;
 }
 return 0;
}
static void setp(AEffect*e,int32_t i,float v){(void)e;if(!i)gain=v<0?0:v>1?1:v;}
static float getp(AEffect*e,int32_t i){(void)e;return i?0:gain;}
static void process(AEffect*e,float**in,float**out,int32_t n){
 (void)e;for(int32_t i=0;i<n;i++){out[0][i]=in[0][i]*gain;out[1][i]=in[1][i]*gain;}
}
static AEffect fx;
__attribute__((visibility("default"))) AEffect* VSTPluginMain(audioMasterCallback m){
 memset(&fx,0,sizeof fx);fx.magic=0x56737450;fx.dispatcher=dispatch;fx.setParameter=setp;fx.getParameter=getp;
 fx.processReplacing=process;fx.numParams=1;fx.numInputs=2;fx.numOutputs=2;fx.flags=1<<4;
 fx.uniqueID=0x4f544731; /* OTG1 */ fx.version=1000;fx.user=(void*)m;return &fx;
}
