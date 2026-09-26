#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct AEffect AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect*,int32_t,int32_t,intptr_t,void*,float);
struct AEffect{int32_t magic;intptr_t(*dispatcher)(AEffect*,int32_t,int32_t,intptr_t,void*,float);void(*process)(AEffect*,float**,float**,int32_t);void(*setParameter)(AEffect*,int32_t,float);float(*getParameter)(AEffect*,int32_t);int32_t numPrograms,numParams,numInputs,numOutputs,flags;intptr_t r1,r2;int32_t initialDelay,realQualities,offQualities;float ioRatio;void*object,*user;int32_t uniqueID,version;void(*processReplacing)(AEffect*,float**,float**,int32_t);void*processDoubleReplacing;char future[56];};
static intptr_t host(AEffect*e,int32_t op,int32_t i,intptr_t v,void*p,float o){(void)e;(void)op;(void)i;(void)v;(void)p;(void)o;return 0;}
int main(int argc,char**argv){
 const char*so=argc>1?argv[1]:"/sdcard/vst/MPC-Gain-Full.so";
 const char*log=argc>2?argv[2]:"/sdcard/vst/MPC-Gain-Full.dlopen.log";
 FILE*f=fopen(log,"w"); if(!f)return 2;
 fprintf(f,"probe so=%s\n",so);
 void*h=dlopen(so,RTLD_NOW|RTLD_LOCAL);
 if(!h){fprintf(f,"dlopen FAIL: %s\n",dlerror());fclose(f);return 3;}
 fprintf(f,"dlopen OK\n");
 dlerror();
 AEffect*(*mainfn)(audioMasterCallback)=(AEffect*(*)(audioMasterCallback))dlsym(h,"VSTPluginMain");
 const char*e=dlerror(); if(e||!mainfn){fprintf(f,"dlsym FAIL: %s\n",e?e:"null");fclose(f);return 4;}
 fprintf(f,"dlsym OK\n");
 AEffect*a=mainfn(host);
 if(!a){fprintf(f,"VSTPluginMain returned NULL\n");fclose(f);return 5;}
 fprintf(f,"AEffect magic=%08x uid=%08x ver=%d in=%d out=%d params=%d flags=%08x process=%p replacing=%p dispatcher=%p\n",(unsigned)a->magic,(unsigned)a->uniqueID,a->version,a->numInputs,a->numOutputs,a->numParams,(unsigned)a->flags,(void*)a->process,(void*)a->processReplacing,(void*)a->dispatcher);
 if(a->dispatcher){
   fprintf(f,"effOpen=%ld\n",(long)a->dispatcher(a,0,0,0,0,0));
   fprintf(f,"setSR=%ld\n",(long)a->dispatcher(a,10,0,0,0,44100.0f));
   fprintf(f,"setBlock=%ld\n",(long)a->dispatcher(a,11,0,128,0,0));
   fprintf(f,"mainsOn=%ld\n",(long)a->dispatcher(a,12,0,1,0,0));
 }
 fflush(f); fclose(f); return 0;
}
