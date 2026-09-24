// Host-only regression for the public HRX/IREE status conversion boundary.
// Links the production status.c with the real IREE status implementation.
#include "hrx_runtime.h"
#include "iree/base/api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern hrx_status_t hrx_status_from_iree(iree_status_t);
extern iree_status_t hrx_status_to_iree(hrx_status_t);
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);abort();}}while(0)
static const struct {hrx_status_code_t hrx;iree_status_code_t iree;} codes[]={
 {HRX_STATUS_OK,IREE_STATUS_OK},
 {HRX_STATUS_CANCELLED,IREE_STATUS_CANCELLED},
 {HRX_STATUS_UNKNOWN,IREE_STATUS_UNKNOWN},
 {HRX_STATUS_INVALID_ARGUMENT,IREE_STATUS_INVALID_ARGUMENT},
 {HRX_STATUS_DEADLINE_EXCEEDED,IREE_STATUS_DEADLINE_EXCEEDED},
 {HRX_STATUS_NOT_FOUND,IREE_STATUS_NOT_FOUND},
 {HRX_STATUS_ALREADY_EXISTS,IREE_STATUS_ALREADY_EXISTS},
 {HRX_STATUS_PERMISSION_DENIED,IREE_STATUS_PERMISSION_DENIED},
 {HRX_STATUS_OUT_OF_MEMORY,IREE_STATUS_RESOURCE_EXHAUSTED},
 {HRX_STATUS_FAILED_PRECONDITION,IREE_STATUS_FAILED_PRECONDITION},
 {HRX_STATUS_ABORTED,IREE_STATUS_ABORTED},
 {HRX_STATUS_OUT_OF_RANGE,IREE_STATUS_OUT_OF_RANGE},
 {HRX_STATUS_UNIMPLEMENTED,IREE_STATUS_UNIMPLEMENTED},
 {HRX_STATUS_INTERNAL,IREE_STATUS_INTERNAL},
 {HRX_STATUS_UNAVAILABLE,IREE_STATUS_UNAVAILABLE},
 {HRX_STATUS_DATA_LOSS,IREE_STATUS_DATA_LOSS},
};
static void check_message(hrx_status_t status,hrx_status_code_t code){
 CHECK(hrx_status_code(status)==code);
 char* text=NULL;size_t length=0;
 hrx_status_t formatted=hrx_status_to_string(status,&text,&length);
 CHECK(hrx_status_is_ok(formatted)&&text&&length==strlen(text));
 CHECK(strstr(text,code==HRX_STATUS_OK?"OK":"roundtrip sentinel")!=NULL);
 hrx_status_free_message(text);hrx_status_ignore(formatted);
}
int main(void){
 for(size_t i=0;i<sizeof(codes)/sizeof(codes[0]);++i){
  // HRX -> IREE -> HRX, consuming each owning status exactly once.
  iree_status_t inner=hrx_status_to_iree(hrx_make_status(codes[i].hrx,"roundtrip sentinel"));
  CHECK(iree_status_code(inner)==codes[i].iree);
  hrx_status_t outer=hrx_status_from_iree(inner);
  check_message(outer,codes[i].hrx);hrx_status_ignore(outer);
  // IREE -> HRX -> IREE, independently exercising the other entry point.
  inner=codes[i].iree==IREE_STATUS_OK?iree_ok_status():iree_make_status(codes[i].iree,"roundtrip sentinel");
  outer=hrx_status_from_iree(inner);check_message(outer,codes[i].hrx);
  inner=hrx_status_to_iree(outer);CHECK(iree_status_code(inner)==codes[i].iree);iree_status_free(inner);
 }
 const iree_status_code_t unsupported[]={IREE_STATUS_UNAUTHENTICATED,IREE_STATUS_DEFERRED,IREE_STATUS_INCOMPATIBLE};
 for(size_t i=0;i<sizeof(unsupported)/sizeof(unsupported[0]);++i){
  hrx_status_t status=hrx_status_from_iree(iree_make_status(unsupported[i],"roundtrip sentinel"));
  check_message(status,HRX_STATUS_INTERNAL);hrx_status_ignore(status);
 }
 puts("PASS all 16 public HRX status codes round-trip in both directions with messages; 3 IREE-only codes retain INTERNAL fallback");
 return 0;
}
