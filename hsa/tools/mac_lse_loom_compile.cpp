// Compile the pinned LSE Loom path without opening any HSA/HRX device.
#include "lse/backends/hrx/loomc/loomc_compiler.hpp"
#include "code_object.h"
#include "lse/backends/hrx/loomc/loom_emitter.hpp"
#include "lse/backends/hrx/device_info.hpp"
#include "lse/backends/hrx/arch_database.hpp"
#include "lse/graph/graph.hpp"
#include "lse/graph/ops.hpp"
#include "lse/kv/block.hpp"
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

bool compileRepeat(lse::backend::LoomcCompiler &compiler,const std::string &path,
                   lse::Shape shape,int count,int axis,lse::DType dtype) {
  using namespace lse;using namespace lse::graph;
  auto node=std::make_shared<Node>();node->shape=shape;node->dtype=dtype;node->materialized=true;
  auto output=repeat(Array(node),count,axis);
  backend::DeviceInfo device;device.arch="gfx1201";device.compute_units=64;
  device.max_threads_per_workgroup=1024;device.wavefront_size=32;device.lds_bytes_per_workgroup=65536;
  backend::AmdDeviceInfo amd;backend::apply_arch_defaults(device,amd);
  device.extension_id=backend::AmdDeviceInfo::kExtensionId;device.extension=&amd;
  backend::LoomEmitter emitter;
  const NodePtr roots[]={output.node()};
  for(const auto &group:Partitioner::partition(roots)) {
    if(group.nodes.empty() || group.nodes.back()!=output.node()) continue;
    auto emitted=emitter.emit(group,device);
    if(!emitted.ok()) {std::fprintf(stderr,"repeat emitter: %s\n",emitted.status().to_string().c_str());return false;}
    std::ofstream(path+".loom")<<emitted->source;
    auto object=compiler.compile(emitted->source,"gfx1201");
    if(!object.ok()) {std::fprintf(stderr,"repeat compiler: %s\n",object.status().to_string().c_str());return false;}
    mac_hsa::CodeObject parsed;
    if(!mac_hsa::parseCodeObject({reinterpret_cast<const uint8_t *>(object->code.data()),object->code.size()},parsed) || parsed.kernels.empty()) return false;
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char *>(object->code.data()),object->code.size());
    std::printf("LSE repeat compile: %zu -> %zu elements axis%d count%d, %zu bytes\n",node->element_count(),output.node()->element_count(),axis,count,object->code.size());
    return bool(file);
  }
  return false;
}

bool compileQwenOperators(lse::backend::LoomcCompiler &compiler,const std::string &path) {
  using namespace lse;using namespace lse::graph;
  auto leaf=[](Shape shape,DType dtype=DType::kF32) {auto n=std::make_shared<Node>();n->shape=shape;n->dtype=dtype;n->materialized=true;return Array(n);};
  std::vector<std::pair<std::string,Array>> cases;
  for(int seq:{1,6}) {
    const auto suffix=std::to_string(seq);
    auto x=leaf(Shape{1,seq,5120});
    cases.emplace_back("rms"+suffix,rms_norm(x,leaf(Shape{5120},DType::kBF16),1e-6f,true));
    auto q=leaf(Shape{1,seq,48,128}),k=leaf(q.shape()),v=leaf(q.shape());
    auto state=leaf(Shape{1,48,128,128});Array sout;
    auto gdn=gated_delta_step(q,k,v,leaf(Shape{1,seq,48}),leaf(Shape{1,seq,48}),state,&sout);
    cases.emplace_back("gdn-out"+suffix,gdn);cases.emplace_back("gdn-state"+suffix,sout);
    cases.emplace_back("gdn-rms"+suffix,rms_norm(q,leaf(Shape{128},DType::kBF16),1e-6f));
    cases.emplace_back("conv-tail"+suffix,conv_tail(leaf(Shape{1,3,10240}),leaf(Shape{1,seq,10240})));
    cases.emplace_back("slice"+suffix,slice(leaf(Shape{1,seq,10240}),-1,2048,4096));
    cases.emplace_back("l2"+suffix,l2_normalize(leaf(Shape{1,seq,16,128}),1e-6f));
    cases.emplace_back("transpose"+suffix,transpose(leaf(Shape{1,seq,24,256}),{0,2,1,3}));
    auto aq=leaf(Shape{1,24,seq,256}),ak=leaf(Shape{1,4,128,256}),av=leaf(ak.shape());
    cases.emplace_back("rope"+suffix,rope(aq,leaf(Shape{128,256}),leaf(Shape{128,256}),leaf(Shape{1})));
    cases.emplace_back("attention"+suffix,sdpa(aq,ak,av,0.0625f,MaskKind::kCausal,0,leaf(Shape{1})));
    auto pool=leaf(Shape{8,4,16,256}),meta=leaf(Shape{kv::step_meta_elems(1)}),table=leaf(Shape{1,8});
    cases.emplace_back("paged-attention"+suffix,sdpa_paged(aq,pool,pool,0.0625f,MaskKind::kCausal,0,meta,table,16));
    cases.emplace_back("kv-page"+suffix,kv_page_write(pool,leaf(Shape{1,4,seq,256}),meta,table,16));
    cases.emplace_back("argmax"+suffix,argmax(leaf(Shape{1,seq,248320})));
    cases.emplace_back("embedding"+suffix,quant_embedding(leaf(Shape{248320,960},DType::kU32),leaf(Shape{248320,80},DType::kBF16),leaf(Shape{248320,80},DType::kBF16),leaf(Shape{1,seq}),6,64));
    cases.emplace_back("decay"+suffix,exp(neg(exp(leaf(Shape{48}))*softplus(leaf(Shape{1,seq,48})+leaf(Shape{48},DType::kBF16)))));
    cases.emplace_back("beta"+suffix,clamp(sigmoid(leaf(Shape{1,seq,48})),1e-4f,1-1e-4f));
    cases.emplace_back("gate"+suffix,silu(leaf(Shape{1,seq,17408}))*leaf(Shape{1,seq,17408}));
  }
  backend::DeviceInfo device;device.arch="gfx1201";device.compute_units=64;
  device.max_threads_per_workgroup=1024;device.wavefront_size=32;device.lds_bytes_per_workgroup=65536;
  backend::AmdDeviceInfo amd;backend::apply_arch_defaults(device,amd);
  device.extension_id=backend::AmdDeviceInfo::kExtensionId;device.extension=&amd;
  backend::LoomEmitter emitter;bool all=true;size_t compiled=0;
  for(auto &[name,output]:cases) {
    const NodePtr roots[]={output.node()};
    auto groups=Partitioner::partition(roots);
    for(size_t i=0;i<groups.size();++i) {
      auto &group=groups[i];
      auto emitted=emitter.emit(group,device);
      if(!emitted.ok()) {std::fprintf(stderr,"Qwen coverage %s group%zu emitter: %s\n",name.c_str(),i,emitted.status().to_string().c_str());all=false;continue;}
      const auto stem=path+"."+name+"."+std::to_string(i);
      std::ofstream(stem+".loom")<<emitted->source;
      auto object=compiler.compile(emitted->source,"gfx1201");
      if(!object.ok()) {std::fprintf(stderr,"Qwen coverage %s group%zu compiler: %s\n",name.c_str(),i,object.status().to_string().c_str());all=false;continue;}
      mac_hsa::CodeObject parsed;
      if(!mac_hsa::parseCodeObject({reinterpret_cast<const uint8_t *>(object->code.data()),object->code.size()},parsed) || parsed.kernels.empty()) {all=false;continue;}
      std::ofstream file(stem+".hsaco",std::ios::binary);file.write(reinterpret_cast<const char *>(object->code.data()),object->code.size());
      std::printf("Qwen operator compile PASS %s group%zu (%zu bytes)\n",name.c_str(),i,object->code.size());++compiled;
    }
  }
  std::printf("Qwen operator coverage: %zu cases, %zu compiled groups, all=%s (compile-only)\n",cases.size(),compiled,all?"yes":"no");
  return all;
}

bool compileQwenCompositions(lse::backend::LoomcCompiler &compiler,const std::string &path) {
  using namespace lse;using namespace lse::graph;
  auto leaf=[](Shape shape,DType dtype=DType::kF32) {auto n=std::make_shared<Node>();n->shape=shape;n->dtype=dtype;n->materialized=true;return Array(n);};
  std::vector<std::pair<std::string,Array>> cases;
  std::vector<Array> carryRoots;
  auto qlinear=[&](Array x,int n) {const auto k=x.shape().dim(x.shape().rank()-1);return quant_linear(x,leaf(Shape{n,k*6/32},DType::kU32),leaf(Shape{n,k/64},DType::kBF16),leaf(Shape{n,k/64},DType::kBF16),6,64);};
  for(int seq:{1,6}) {
    const auto suffix=std::to_string(seq);
    auto q=leaf(Shape{1,24,seq,256});
    auto norm=rms_norm(q,leaf(Shape{256},DType::kBF16),1e-6f,false);
    auto rotated=rope(slice(norm,-1,0,64),leaf(Shape{128,64}),leaf(Shape{128,64}),leaf(Shape{kv::step_meta_elems(1)}));
    cases.emplace_back("partial-rope64-q"+suffix,concat({rotated,slice(norm,-1,64,256)},-1));
    auto knorm=rms_norm(leaf(Shape{1,4,seq,256}),leaf(Shape{256},DType::kBF16),1e-6f,false);
    cases.emplace_back("partial-rope64-k"+suffix,concat({rope(slice(knorm,-1,0,64),leaf(Shape{128,64}),leaf(Shape{128,64}),leaf(Shape{kv::step_meta_elems(1)})),slice(knorm,-1,64,256)},-1));
    auto x=leaf(Shape{1,seq,5120});
    auto qproj=qlinear(x,12288);
    auto qheads=transpose(reshape(slice(qproj,-1,0,6144),Shape{1,seq,24,256}),{0,2,1,3});
    cases.emplace_back("qprojection-slice-transpose-norm"+suffix,rms_norm(qheads,leaf(Shape{256},DType::kBF16),1e-6f,false));
    cases.emplace_back("attention-gate"+suffix,reshape(transpose(leaf(Shape{1,24,seq,256}),{0,2,1,3}),Shape{1,seq,6144})*sigmoid(slice(qproj,-1,6144,12288)));
    auto raw=silu(slice(leaf(Shape{1,seq,10240}),-1,0,2048));
    cases.emplace_back("gdn-q-scale-repeat"+suffix,repeat(l2_normalize(reshape(raw,Shape{1,seq,16,128}),1e-6f)*Array::full(Shape{1},DType::kF32,0.08838835f),3,2));
    auto rate=exp(cast(leaf(Shape{48},DType::kBF16),DType::kF32));
    cases.emplace_back("q6-decay-fusion"+suffix,exp(neg(rate*softplus(qlinear(x,48)+leaf(Shape{48},DType::kBF16)))));
    cases.emplace_back("q6-beta-fusion"+suffix,clamp(sigmoid(qlinear(x,48)),1e-4f,1-1e-4f));
    cases.emplace_back("q6-mlp-fusion"+suffix,silu(qlinear(x,17408))*qlinear(x,17408));
    cases.emplace_back("q6-down-residual-norm"+suffix,rms_norm(qlinear(leaf(Shape{1,seq,17408}),5120)+x,leaf(Shape{5120},DType::kBF16),1e-6f,false));
    cases.emplace_back("q6-attention-out"+suffix,qlinear(leaf(Shape{1,seq,6144}),5120));
    cases.emplace_back("final-logits"+suffix,qlinear(rms_norm(x,leaf(Shape{5120},DType::kBF16),1e-6f,false),248320));
    auto convInput=qlinear(x,10240),hist=leaf(Shape{1,3,10240});
    auto conv=causal_conv1d(convInput,leaf(Shape{10240,4},DType::kBF16),leaf(Shape{10240},DType::kBF16),hist);
    auto gq=repeat(l2_normalize(reshape(silu(slice(conv,-1,0,2048)),Shape{1,seq,16,128}),1e-6f)*Array::full(Shape{1},DType::kF32,0.08838835f),3,2);
    auto gk=repeat(l2_normalize(reshape(silu(slice(conv,-1,2048,4096)),Shape{1,seq,16,128}),1e-6f),3,2);
    auto gv=reshape(silu(slice(conv,-1,4096,10240)),Shape{1,seq,48,128});
    auto alpha=exp(neg(rate*softplus(qlinear(x,48)+leaf(Shape{48},DType::kBF16))));
    auto beta=clamp(sigmoid(qlinear(x,48)),1e-4f,1-1e-4f);
    Array newState; auto go=gated_delta_step(gq,gk,gv,alpha,beta,leaf(Shape{1,48,128,128}),&newState);
    auto gated=reshape(rms_norm(go,leaf(Shape{128},DType::kBF16),1e-6f,false),Shape{1,seq,6144})*silu(qlinear(x,6144));
    cases.emplace_back("whole-gdn-carry"+suffix,rms_norm(qlinear(gated,5120)+x,leaf(Shape{5120},DType::kBF16),1e-6f,false));
    carryRoots.push_back(newState);carryRoots.push_back(conv_tail(hist,convInput));
    auto partial=[&](Array heads){auto rn=rms_norm(heads,leaf(Shape{256},DType::kBF16),1e-6f,false);return concat({rope(slice(rn,-1,0,64),leaf(Shape{128,64}),leaf(Shape{128,64}),leaf(Shape{kv::step_meta_elems(1)})),slice(rn,-1,64,256)},-1);};
    auto aq=partial(qheads),ak=partial(transpose(reshape(qlinear(x,1024),Shape{1,seq,4,256}),{0,2,1,3}));
    auto av=transpose(reshape(qlinear(x,1024),Shape{1,seq,4,256}),{0,2,1,3});
    auto meta=leaf(Shape{kv::step_meta_elems(1)}),table=leaf(Shape{1,8});
    auto kp=kv_page_write(leaf(Shape{8,4,16,256}),ak,meta,table,16);
    auto vp=kv_page_write(leaf(Shape{8,4,16,256}),av,meta,table,16);
    auto att=sdpa_paged(aq,kp,vp,0.0625f,MaskKind::kCausal,0,meta,table,16);
    auto attgate=reshape(transpose(att,{0,2,1,3}),Shape{1,seq,6144})*sigmoid(slice(qproj,-1,6144,12288));
    cases.emplace_back("whole-attention-cache"+suffix,rms_norm(qlinear(attgate,5120)+x,leaf(Shape{5120},DType::kBF16),1e-6f,false));

  }
  backend::DeviceInfo device;device.arch="gfx1201";device.compute_units=64;
  device.max_threads_per_workgroup=1024;device.wavefront_size=32;device.lds_bytes_per_workgroup=65536;
  backend::AmdDeviceInfo amd;backend::apply_arch_defaults(device,amd);
  device.extension_id=backend::AmdDeviceInfo::kExtensionId;device.extension=&amd;
  backend::LoomEmitter emitter;bool all=true;size_t compiled=0;
  for(auto &[name,output]:cases) {
    std::vector<NodePtr> roots{output.node()};
    if(name.rfind("whole-gdn-carry",0)==0) {const size_t at=name.back()=='1'?0:2;roots.push_back(carryRoots[at].node());roots.push_back(carryRoots[at+1].node());}
    auto groups=Partitioner::partition(roots);
    for(size_t i=0;i<groups.size();++i) {
      auto &group=groups[i];
      auto emitted=emitter.emit(group,device);
      if(!emitted.ok()) {std::fprintf(stderr,"Qwen coverage %s group%zu emitter: %s\n",name.c_str(),i,emitted.status().to_string().c_str());all=false;continue;}
      const auto stem=path+"."+name+"."+std::to_string(i);
      std::ofstream(stem+".loom")<<emitted->source;
      auto object=compiler.compile(emitted->source,"gfx1201");
      if(!object.ok()) {std::fprintf(stderr,"Qwen coverage %s group%zu compiler: %s\n",name.c_str(),i,object.status().to_string().c_str());all=false;continue;}
      mac_hsa::CodeObject parsed;
      if(!mac_hsa::parseCodeObject({reinterpret_cast<const uint8_t *>(object->code.data()),object->code.size()},parsed) || parsed.kernels.empty()) {all=false;continue;}
      std::ofstream file(stem+".hsaco",std::ios::binary);file.write(reinterpret_cast<const char *>(object->code.data()),object->code.size());
      std::printf("Qwen operator compile PASS %s group%zu (%zu bytes)\n",name.c_str(),i,object->code.size());++compiled;
    }
  }
  std::printf("Qwen composition coverage: %zu cases, %zu compiled groups, all=%s (compile-only)\n",cases.size(),compiled,all?"yes":"no");
  return all;
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
      compileConvolution(compiler,std::string(argv[1])+".conv-prefill.hsaco",7,17,true) &&
      compileRepeat(compiler,std::string(argv[1])+".repeat-gdn.hsaco",lse::Shape{1,6,16,128},3,2,lse::DType::kF32) &&
      compileRepeat(compiler,std::string(argv[1])+".repeat-attn.hsaco",lse::Shape{1,4,128,256},6,1,lse::DType::kF32) &&
      compileRepeat(compiler,std::string(argv[1])+".repeat-u32.hsaco",lse::Shape{2,3,5},3,-1,lse::DType::kU32) &&
      compileQwenOperators(compiler,std::string(argv[1])+".qwen") &&
      compileQwenCompositions(compiler,std::string(argv[1])+".composed") ? 0 : 1;
}
