#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <zlib.h>

namespace mtop {
enum class Graphics { Text, Kitty, ITerm };
inline Graphics graphicsFor(std::string_view requested,std::string_view program,std::string_view term,bool multiplexed) {
    if(requested=="kitty") return Graphics::Kitty;
    if(requested=="iterm") return Graphics::ITerm;
    if(requested=="text" || multiplexed) return Graphics::Text;
    if(program=="iTerm.app") return Graphics::ITerm;
    if(program=="kitty" || program=="ghostty" || term=="xterm-kitty") return Graphics::Kitty;
    return Graphics::Text;
}
inline std::string clipColumns(std::string_view text,size_t columns) {
    std::string out;size_t at=0,n=0;
    while(at<text.size() && n<columns) {
        if(text[at]=='\033' && at+1<text.size() && text[at+1]=='[') {
            const auto begin=at;at+=2;
            while(at<text.size() && !(text[at]>='@' && text[at]<='~')) ++at;
            if(at<text.size()) ++at;
            out+=text.substr(begin,at-begin);continue;
        }
        const unsigned char c=text[at];
        const size_t bytes=c<128?1:(c&0xe0)==0xc0?2:(c&0xf0)==0xe0?3:4;
        if(at+bytes>text.size()) break;
        out+=text.substr(at,bytes);at+=bytes;++n;
    }
    return out;
}
struct Frame {
    unsigned width=0,height=0;
    std::vector<std::string> previous;
    std::string update(const std::vector<std::string>&lines,unsigned columns,unsigned rows) {
        std::string out;
        if(width!=columns || height!=rows) {out="\033[2J";previous.clear();}
        width=columns;height=rows;
        for(size_t i=0;i<rows;++i) {
            const auto line=i<lines.size()?clipColumns(lines[i],columns>1?columns-1:1):std::string{};
            if(i>=previous.size() || line!=previous[i])
                out+="\033["+std::to_string(i+1)+";1H"+line+"\033[0m\033[K";
            if(i>=previous.size()) previous.push_back(line);else previous[i]=line;
        }
        previous.resize(rows);
        return out;
    }
};
inline std::string base64(const std::vector<uint8_t>&bytes) {
    constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;out.reserve((bytes.size()+2)/3*4);
    for(size_t i=0;i<bytes.size();i+=3) {
        const unsigned n=(unsigned(bytes[i])<<16)|(i+1<bytes.size()?unsigned(bytes[i+1])<<8:0)|(i+2<bytes.size()?bytes[i+2]:0);
        out+=alphabet[n>>18];out+=alphabet[(n>>12)&63];
        out+=i+1<bytes.size()?alphabet[(n>>6)&63]:'=';out+=i+2<bytes.size()?alphabet[n&63]:'=';
    }
    return out;
}
inline void bigEndian(std::vector<uint8_t>&out,uint32_t v) {
    for(int shift=24;shift>=0;shift-=8) out.push_back(uint8_t(v>>shift));
}
inline void pngChunk(std::vector<uint8_t>&out,const char*type,const std::vector<uint8_t>&payload) {
    bigEndian(out,uint32_t(payload.size()));const size_t start=out.size();
    out.insert(out.end(),type,type+4);out.insert(out.end(),payload.begin(),payload.end());
    bigEndian(out,uint32_t(crc32(0,out.data()+start,uInt(out.size()-start))));
}
inline std::vector<uint8_t> chartPNG(const std::vector<std::optional<double>>&values,unsigned width,unsigned height,double ceiling,
                                    std::array<uint8_t,3> color={64,203,230}) {
    width=std::clamp(width,2u,1600u);height=std::clamp(height,2u,320u);
    constexpr std::array<uint8_t,3> bg{15,23,35},grid{37,49,65};
    std::vector<uint8_t> pixels(size_t(width)*height*3);
    auto set=[&](unsigned x,unsigned y,std::array<uint8_t,3> c){const size_t i=(size_t(y)*width+x)*3;std::copy(c.begin(),c.end(),pixels.begin()+i);};
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)set(x,y,y%(std::max(1u,(height-1)/4))==0?grid:bg);
    std::optional<unsigned> prior;
    for(unsigned x=0;x<width;++x) {
        const size_t i=values.empty()?0:std::min(values.size()-1,size_t(x)*values.size()/width);
        if(values.empty() || !values[i] || !std::isfinite(*values[i]) || !(ceiling>0)) {prior.reset();continue;}
        const unsigned y=height-1-unsigned(std::lround(std::clamp(*values[i]/ceiling,0.0,1.0)*(height-1)));
        for(unsigned fill=y+1;fill<height;++fill)set(x,fill,{uint8_t((color[0]+bg[0]*4)/5),uint8_t((color[1]+bg[1]*4)/5),uint8_t((color[2]+bg[2]*4)/5)});
        if(prior)for(unsigned yy=std::min(y,*prior);yy<=std::max(y,*prior);++yy)set(x,yy,color);
        set(x,y,color);if(y+1<height)set(x,y+1,color);prior=y;
    }
    std::vector<uint8_t> raw;raw.reserve((size_t(width)*3+1)*height);
    for(unsigned y=0;y<height;++y){raw.push_back(0);raw.insert(raw.end(),pixels.begin()+size_t(y)*width*3,pixels.begin()+size_t(y+1)*width*3);}
    uLongf size=compressBound(raw.size());std::vector<uint8_t> compressed(size);
    if(compress2(compressed.data(),&size,raw.data(),raw.size(),1)!=Z_OK)return {};
    compressed.resize(size);
    std::vector<uint8_t> png{137,80,78,71,13,10,26,10},header;
    bigEndian(header,width);bigEndian(header,height);header.insert(header.end(),{8,2,0,0,0});
    pngChunk(png,"IHDR",header);pngChunk(png,"IDAT",compressed);pngChunk(png,"IEND",{});return png;
}
inline std::string pixelImage(Graphics graphics,const std::vector<uint8_t>&png,unsigned id,unsigned row,unsigned columns,unsigned rows) {
    if(graphics==Graphics::Text || png.empty())return {};
    const auto data=base64(png);
    std::string out="\033["+std::to_string(row)+";2H";
    if(graphics==Graphics::ITerm)
        return out+"\033]1337;File=inline=1;size="+std::to_string(png.size())+";width="+std::to_string(columns)+";height="+std::to_string(rows)+";preserveAspectRatio=0:"+data+"\a";
    out+="\033_Ga=d,d=I,i="+std::to_string(id)+",q=2\033\\";
    for(size_t at=0;at<data.size();at+=4096) {
        const bool more=at+4096<data.size();
        out+="\033_G";
        if(at==0)out+="a=T,f=100,t=d,i="+std::to_string(id)+",q=2,C=1,c="+std::to_string(columns)+",r="+std::to_string(rows)+",";
        out+="m="+std::to_string(more?1:0)+";"+data.substr(at,4096)+"\033\\";
    }
    return out;
}
} // namespace mtop
