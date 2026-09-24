// Explicit hardware opt-in: simultaneous pooled buffers, reuse and large offsets.
#include "hrx_runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check(hrx_status_t status, const char *where) {
  if(hrx_status_is_ok(status)) return 1;
  char *message=NULL;size_t length=0;
  hrx_status_ignore(hrx_status_to_string(status,&message,&length));
  fprintf(stderr,"FAIL: %s: %.*s\n",where,(int)length,message?message:"");
  if(message) hrx_status_free_message(message);
  hrx_status_ignore(status);return 0;
}
static int pattern(hrx_device_t device,hrx_buffer_t buffer,size_t size,
                   unsigned index,unsigned generation,int write) {
  uint32_t expected[1024],actual[1024];
  const size_t offsets[]={0,(size/2)&~(size_t)4095,size-sizeof(expected)};
  for(unsigned phase=0;phase<3;++phase) {
    for(unsigned i=0;i<1024;++i)
      expected[i]=0x891a34cdu^(index*0x00101001u)^(generation*0x31579bdu)^(phase*0x10017u)^i;
    if(write) {
      if(!check(hrx_synchronous_h2d(device,expected,buffer,offsets[phase],sizeof(expected)),"write allocation canary")) return 0;
    } else {
      if(!check(hrx_synchronous_d2h(device,buffer,offsets[phase],actual,sizeof(actual)),"read allocation canary")) return 0;
      if(memcmp(actual,expected,sizeof(actual))) {
        fprintf(stderr,"FAIL: allocation=%u generation=%u offset=%zu canary mismatch\n",index,generation,offsets[phase]);return 0;
      }
    }
  }
  return 1;
}
int main(int argc,char **argv) {
  if(argc!=2 || strcmp(argv[1],"--run")) {
    fprintf(stderr,"Usage: %s --run (96 live pooled buffers, reuse, 2GiB VRAM offsets)\n",argv[0]);return 2;
  }
  setvbuf(stdout,NULL,_IONBF,0);
  enum {kCount=96};
  hrx_device_t device=NULL;hrx_stream_t stream=NULL;
  hrx_buffer_t buffers[kCount]={0},large=NULL;
  size_t sizes[kCount]={0};
  int initialized=0,success=0,count=0;
  if(!check(hrx_gpu_initialize(0),"initialize")) goto cleanup;
  initialized=1;
  if(!check(hrx_gpu_device_count(&count),"device count") || count!=1 ||
     !check(hrx_gpu_device_get(0,&device),"device") ||
     !check(hrx_stream_create(device,0,&stream),"stream")) goto cleanup;
  for(unsigned generation=0;generation<2;++generation) {
    for(unsigned i=0;i<kCount;++i) {
      if(generation && (i&1)) continue;
      if(buffers[i]) {hrx_buffer_release(buffers[i]);buffers[i]=NULL;}
      sizes[i]=(generation?1u:3u)<<20;
      if(!check(hrx_buffer_allocate(stream,sizes[i],HRX_MEMORY_TYPE_DEVICE_LOCAL,
                    HRX_BUFFER_USAGE_DEFAULT,&buffers[i]),"allocate simultaneous buffer")) goto cleanup;
    }
    for(unsigned i=0;i<kCount;++i) {
      if(generation && (i&1)) continue;
      if(!pattern(device,buffers[i],sizes[i],i,generation,1)) goto cleanup;
    }
    for(unsigned i=0;i<kCount;++i)
      if(!pattern(device,buffers[i],sizes[i],i,generation && !(i&1),0)) goto cleanup;
    printf("PASS: generation%u, 96 simultaneous buffers, all head/middle/tail canaries; retained odd buffers unchanged\n",generation);
  }
  if(!check(hrx_buffer_allocate(stream,(size_t)2<<30,HRX_MEMORY_TYPE_DEVICE_LOCAL,
                HRX_BUFFER_USAGE_DEFAULT,&large),"allocate 2GiB device slab") ||
     !pattern(device,large,(size_t)2<<30,100,0,1) ||
     !pattern(device,large,(size_t)2<<30,100,0,0)) goto cleanup;
  for(unsigned i=0;i<kCount;++i)
    if(!pattern(device,buffers[i],sizes[i],i,!(i&1),0)) goto cleanup;
  puts("PASS: 2GiB device slab with exact canaries at0,1GiB and2GiB-4KiB;96 small buffers still live");
  success=1;
cleanup:
  if(stream && !check(hrx_stream_synchronize(stream),"retire before cleanup")) {
    fputs("FAIL: retirement unconfirmed; retaining buffers for driver connection teardown\n",stderr);return 1;
  }
  if(large) hrx_buffer_release(large);
  for(unsigned i=0;i<kCount;++i) if(buffers[i]) hrx_buffer_release(buffers[i]);
  if(stream) hrx_stream_release(stream);
  if(initialized && !check(hrx_gpu_shutdown(),"shutdown")) success=0;
  if(success) puts("PASS: allocation stress and clean shutdown");
  return success?0:1;
}
