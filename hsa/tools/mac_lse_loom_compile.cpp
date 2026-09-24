// Compile the pinned LSE Loom path without opening any HSA/HRX device.
#include "lse/backends/hrx/loomc/loomc_compiler.hpp"
#include "code_object.h"
#include "lse/backends/hrx/loomc/loom_emitter.hpp"
#include "lse/backends/hrx/device_info.hpp"
#include "lse/backends/hrx/arch_database.hpp"
#include "lse/graph/graph.hpp"
#include "lse/graph/ops.hpp"
#include <cstdio>
#include <fstream>
#include <string>
std::string loom_matmul_source(int m, int k, int n, unsigned threads) {
  const auto elems = static_cast<unsigned>(m * n);
  const std::string md = std::to_string(m);
  const std::string kd = std::to_string(k);
  const std::string nd = std::to_string(n);
  const std::string xt = "view<" + md + "x" + kd + "xf32, #dense>";
  const std::string yt = "view<" + kd + "x" + nd + "xf32, #dense>";
  const std::string ot = "view<" + md + "x" + nd + "xf32, #dense>";
  std::string s;
  s += "kernel.def export(\"lse_matmul_loom\") @lse_matmul_loom() {\n";
  s += "  %unit = index.constant 1 : index\n";
  s += "  %wg = index.constant " + std::to_string(threads) + " : index\n";
  s += "  %groups = index.constant " +
       std::to_string((elems + threads - 1) / threads) + " : index\n";
  s += "  kernel.launch.config workgroups(%groups, %unit, %unit) "
       "workgroup_size(%wg, %unit, %unit) : index\n";
  s += "} launch(%x: buffer, %y: buffer, %out: buffer, %count: i32) {\n";
  s += "  %base = index.constant 0 : offset\n";
  s += "  %zero = index.constant 0 : index\n";
  s += "  %unit = index.constant 1 : index\n";
  s += "  %wg = index.constant " + std::to_string(threads) + " : index\n";
  s += "  %kdim = index.constant " + kd + " : index\n";
  s += "  %cols = index.constant " + nd + " : index\n";
  s += "  %zero_f32 = scalar.constant 0.0 : f32\n";
  s += "  %group = kernel.workgroup.id<x> : index\n";
  s += "  %lane = kernel.workitem.id<x> : index\n";
  s += "  %i = index.madd %group, %wg, %lane : index\n";
  s += "  %elems = index.constant " + std::to_string(elems) + " : index\n";
  s += "  %limit0 = index.cast %count : i32 to index\n";
  s += "  %limit = index.assume %limit0 [range(%limit0, 0, " +
       std::to_string(elems) + "), le(%limit0, %elems)] : index\n";
  s += "  %live = index.cmp ult, %i, %limit : index\n";
  s += "  %xn, %yn, %on = buffer.assume.noalias %x, %y, %out : buffer, buffer, "
       "buffer\n";
  s += "  %xv = buffer.view %xn[%base] : buffer -> " + xt + "\n";
  s += "  %yv = buffer.view %yn[%base] : buffer -> " + yt + "\n";
  s += "  %ov = buffer.view %on[%base] : buffer -> " + ot + "\n";
  s += "  scf.if %live {\n";
  s += "    %row = index.div %i, %cols : index\n";
  s += "    %col = index.rem %i, %cols : index\n";
  s += "    %acc = scf.for %t = [%zero to %kdim step %unit](%a = %zero_f32 : "
       "f32) -> (f32) {\n";
  s += "      %xval = view.load %xv[%row, %t] : " + xt + " -> f32\n";
  s += "      %yval = view.load %yv[%t, %col] : " + yt + " -> f32\n";
  s += "      %next = scalar.fmaf %xval, %yval, %a : f32\n";
  s += "      scf.yield %next : f32\n";
  s += "    }\n";
  s += "    view.store %acc, %ov[%row, %col] : f32, " + ot + "\n";
  s += "  }\n";
  s += "  kernel.return\n";
  s += "}\n";
  return s;
}


bool compileProjection(lse::backend::LoomcCompiler &compiler,const std::string &path,int m,int k,int n,int bits) {
  using namespace lse;using namespace lse::graph;
  auto leaf=[](Shape shape,DType dtype) {auto node=std::make_shared<Node>();node->shape=shape;node->dtype=dtype;return Array(node);};
  auto x=leaf(Shape{m,k},DType::kF32);
  auto packed=leaf(Shape{n,k*bits/32},DType::kU32);
  auto scales=leaf(Shape{n,k/64},DType::kBF16),biases=leaf(Shape{n,k/64},DType::kBF16);
  auto output=quant_linear(x,packed,scales,biases,bits,64);
  backend::DeviceInfo device;device.arch="gfx1201";device.compute_units=64;
  device.max_threads_per_workgroup=1024;device.wavefront_size=32;device.lds_bytes_per_workgroup=65536;
  backend::AmdDeviceInfo amd;backend::apply_arch_defaults(device,amd);
  device.extension_id=backend::AmdDeviceInfo::kExtensionId;device.extension=&amd;
  backend::LoomEmitter emitter;
  const NodePtr roots[]={output.node()};
  for(const auto &group:Partitioner::partition(roots)) {
    if(group.anchor!=OpKind::kQuantMatMul) continue;
    auto emitted=emitter.emit(group,device);
    if(!emitted.ok()) {std::fprintf(stderr,"Q6 emitter: %s\n",emitted.status().to_string().c_str());return false;}
    std::ofstream(path+".loom")<<emitted->source;
    auto object=compiler.compile(emitted->source,"gfx1201");
    if(!object.ok()) {std::fprintf(stderr,"Q6 compiler: %s\n",object.status().to_string().c_str());return false;}
    mac_hsa::CodeObject parsed;
    if(!mac_hsa::parseCodeObject({reinterpret_cast<const uint8_t *>(object->code.data()),object->code.size()},parsed) || parsed.kernels.empty()) {
      std::fputs("HSA parser rejected Q6 model projection\n",stderr);return false;
    }
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char *>(object->code.data()),object->code.size());
    std::printf("LSE Qwen Q%d projection compile: M%d K%d N%d, %zu bytes, %zu bindings, WG%u LDS%u\n",
        bits,m,k,n,object->code.size(),emitted->binding_order.size(),emitted->dims.workgroup_size[0],emitted->lds_bytes);
    return bool(file);
  }
  std::fputs("No Q6 projection group emitted\n",stderr);return false;
}

bool compileConvolution(lse::backend::LoomcCompiler &compiler,const std::string &path,int seq,int channels,bool tail) {
  using namespace lse;using namespace lse::graph;
  auto leaf=[](Shape shape,DType dtype) {auto node=std::make_shared<Node>();node->shape=shape;node->dtype=dtype;return Array(node);};
  auto x=leaf(Shape{2,seq,channels},DType::kF32);
  auto weight=leaf(Shape{channels,4},DType::kBF16),bias=leaf(Shape{channels},DType::kBF16);
  auto history=leaf(Shape{2,3,channels},DType::kF32);
  auto output=tail?causal_conv1d(x,weight,bias,history):causal_conv1d(x,weight,bias);
  backend::DeviceInfo device;device.arch="gfx1201";device.compute_units=64;
  device.max_threads_per_workgroup=1024;device.wavefront_size=32;device.lds_bytes_per_workgroup=65536;
  backend::AmdDeviceInfo amd;backend::apply_arch_defaults(device,amd);
  device.extension_id=backend::AmdDeviceInfo::kExtensionId;device.extension=&amd;
  backend::LoomEmitter emitter;
  const NodePtr roots[]={output.node()};
  for(const auto &group:Partitioner::partition(roots)) {
    if(group.nodes.empty() || group.nodes.back()!=output.node()) continue;
    auto emitted=emitter.emit(group,device);
    if(!emitted.ok()) {std::fprintf(stderr,"conv emitter: %s\n",emitted.status().to_string().c_str());return false;}
    std::ofstream(path+".loom")<<emitted->source;
    auto object=compiler.compile(emitted->source,"gfx1201");
    if(!object.ok()) {std::fprintf(stderr,"conv compiler: %s\n",object.status().to_string().c_str());return false;}
    mac_hsa::CodeObject parsed;
    if(!mac_hsa::parseCodeObject({reinterpret_cast<const uint8_t *>(object->code.data()),object->code.size()},parsed) || parsed.kernels.empty()) return false;
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char *>(object->code.data()),object->code.size());
    std::printf("LSE causal convolution compile: B2 T%d C%d K4 tail=%d, %zu bytes, WG%u\n",seq,channels,tail,object->code.size(),emitted->dims.workgroup_size[0]);
    return bool(file);
  }
  return false;
}

int main(int argc,char **argv) {
  if(argc!=2) return 2;
  lse::backend::LoomcCompiler compiler;
  if(!compiler.available()) {std::fputs("Loom unavailable\n",stderr);return 1;}
  const auto source=loom_matmul_source(32,16,32,64);
  auto object=compiler.compile(source,"gfx1201");
  if(!object.ok()) {std::fprintf(stderr,"%s\n",object.status().to_string().c_str());return 1;}
  if(object->code.size()<64) return 1;
  const auto *bytes=reinterpret_cast<const unsigned char *>(object->code.data());
  const unsigned machine=bytes[18]|(bytes[19]<<8),flags=bytes[48]|(bytes[49]<<8)|(bytes[50]<<16)|(bytes[51]<<24);
  if(bytes[0]!=0x7f || bytes[1]!='E' || bytes[2]!='L' || bytes[3]!='F' || machine!=224 || (flags&0xff)!=0x4e) {std::fprintf(stderr,"Unexpected target machine=%u flags=%#x\n",machine,flags);return 1;}
  mac_hsa::CodeObject parsed;
  if(!mac_hsa::parseCodeObject({reinterpret_cast<const uint8_t *>(object->code.data()),object->code.size()},parsed) ||
     parsed.kernels.size()!=1 || parsed.kernels[0].name!="lse_matmul_loom" || parsed.kernels[0].kernargSize!=32 ||
     !mac_hsa::relocateCodeObject(parsed,0x8002000000ull)) {
    std::fputs("HSA parser/relocator rejected Loom matmul fixture\n",stderr);return 1;
  }
  std::ofstream file(argv[1],std::ios::binary);
  file.write(reinterpret_cast<const char *>(object->code.data()),object->code.size());
  if(!file) return 1;
  std::printf("LSE Loom gfx1201 compile passed: %zu bytes flags=%#x; %s\n%s",object->code.size(),flags,
      compiler.identity().c_str(),object->resources.empty() ? "LSE resource reader returned no kernel entries\n" : object->resources[0].describe().c_str());
  return compileProjection(compiler,std::string(argv[1])+".q6-model.hsaco",1,5120,17408,6) &&
      compileProjection(compiler,std::string(argv[1])+".q6-smoke.hsaco",3,128,17,6) &&
      compileProjection(compiler,std::string(argv[1])+".q8-smoke.hsaco",3,128,17,8) &&
      compileConvolution(compiler,std::string(argv[1])+".conv-model.hsaco",6,10240,true) &&
      compileConvolution(compiler,std::string(argv[1])+".conv-zero.hsaco",1,17,false) &&
      compileConvolution(compiler,std::string(argv[1])+".conv-tail.hsaco",1,17,true) &&
      compileConvolution(compiler,std::string(argv[1])+".conv-prefill.hsaco",7,17,true) ? 0 : 1;
}
