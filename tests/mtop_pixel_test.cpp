#include "../amdgpu_mtop/render.h"
#include <cassert>
#include <cstring>
int main() {
    using namespace mtop;
    assert(graphicsFor("auto","Apple_Terminal","xterm-256color",false)==Graphics::Text);
    assert(graphicsFor("auto","kitty","",false)==Graphics::Kitty);
    assert(graphicsFor("auto","iTerm.app","",false)==Graphics::ITerm);
    assert(graphicsFor("auto","kitty","",true)==Graphics::Text);
    assert(clipColumns("A⡀B",2)=="A⡀");
    assert(clipColumns("\033[31mA⡀B",2)=="\033[31mA⡀");
    Frame f;
    assert(!f.update({"abc","def"},10,2).empty());
    assert(f.update({"abc","def"},10,2).empty());
    const auto changed=f.update({"ab","def"},10,2);
    assert(changed.find("\033[1;1H")!=std::string::npos && changed.find("def")==std::string::npos);
    auto png=chartPNG({0.0,50.0,{},100.0},32,16,100);
    assert(png.size()>50 && png[0]==137 && std::memcmp(png.data()+1,"PNG",3)==0);
    auto u32=[&](size_t i){return (uint32_t(png[i])<<24)|(uint32_t(png[i+1])<<16)|(uint32_t(png[i+2])<<8)|png[i+3];};
    std::vector<uint8_t> compressed;
    for(size_t i=8;i<png.size();) {
        auto n=u32(i);assert(i+n+12<=png.size());
        assert(u32(i+8+n)==crc32(0,png.data()+i+4,n+4));
        if(std::memcmp(png.data()+i+4,"IDAT",4)==0)compressed.insert(compressed.end(),png.begin()+i+8,png.begin()+i+8+n);
        i+=n+12;
    }
    std::vector<uint8_t> pixels((32*3+1)*16);uLongf size=pixels.size();
    assert(uncompress(pixels.data(),&size,compressed.data(),compressed.size())==Z_OK && size==pixels.size());
    const auto kitty=pixelImage(Graphics::Kitty,png,5001,3,20,4);
    assert(kitty.find("a=T,f=100,t=d,i=5001,q=2,C=1,c=20,r=4")!=std::string::npos);
    assert(kitty.find("a=d,d=I,i=5001")!=std::string::npos);
    const auto iterm=pixelImage(Graphics::ITerm,png,5001,3,20,4);
    assert(iterm.find("1337;File=inline=1;")!=std::string::npos);
    assert(pixelImage(Graphics::Text,png,5001,3,20,4).empty());
    assert(base64({0,0,0})=="AAAA" && base64({255})=="/w==");
    // Large transmission is chunked at a multiple-of-four boundary, <=4096.
    std::vector<uint8_t> large(10000,1);
    const auto chunks=pixelImage(Graphics::Kitty,large,5001,3,20,4);
    size_t pos=0,count=0;
    while((pos=chunks.find(";",pos))!=std::string::npos) {
        ++pos;const auto end=chunks.find("\033\\",pos);if(end==std::string::npos)break;
        // First semicolon belongs to cursor position; image chunks start after APC G.
        if(chunks.substr(pos,end-pos).find("\033")==std::string::npos){assert(end-pos<=4096);++count;}
        pos=end+2;
    }
    assert(count>=3);
}
