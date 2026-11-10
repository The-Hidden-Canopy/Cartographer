#include <carto/assets/hdr_environment.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <locale>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>

namespace carto::assets {
namespace {
using V = std::array<double, 3>;
using M = std::array<V, 3>;
V mul(const M& m, V v) {
    V r{};
    for (unsigned y=0;y<3;++y) for(unsigned x=0;x<3;++x) r[y]+=m[y][x]*v[x];
    return r;
}
M inverse(M a) {
    M b{{{1,0,0},{0,1,0},{0,0,1}}};
    for(unsigned k=0;k<3;++k) {
        unsigned pivot=k;
        for(unsigned y=k+1;y<3;++y) if(std::abs(a[y][k])>std::abs(a[pivot][k])) pivot=y;
        if(std::abs(a[pivot][k])<1e-10) throw std::runtime_error("degenerate HDR primaries");
        std::swap(a[k],a[pivot]);std::swap(b[k],b[pivot]);
        const double scale=a[k][k];
        for(unsigned x=0;x<3;++x){a[k][x]/=scale;b[k][x]/=scale;}
        for(unsigned y=0;y<3;++y)if(y!=k){const double s=a[y][k];for(unsigned x=0;x<3;++x){a[y][x]-=s*a[k][x];b[y][x]-=s*b[k][x];}}
    }
    return b;
}
V xyz(double x,double y) {
    if(!std::isfinite(x)||!std::isfinite(y)||x<0||y<=0||x+y>1.000001)
        throw std::runtime_error("invalid HDR chromaticities");
    return {x/y,1,(1-x-y)/y};
}
M rgb_xyz(const std::array<double,8>& p) {
    M m{};
    for(unsigned x=0;x<3;++x){const auto v=xyz(p[2*x],p[2*x+1]);for(unsigned y=0;y<3;++y)m[y][x]=v[y];}
    const auto s=mul(inverse(m),xyz(p[6],p[7]));
    for(unsigned x=0;x<3;++x)for(unsigned y=0;y<3;++y)m[y][x]*=s[x];
    return m;
}
template<std::size_t N> std::array<double,N> numbers(const std::string& text, bool allow_zero = false) {
    std::istringstream in(text);std::array<double,N> a{};
    in.imbue(std::locale::classic());
    for(auto& v:a)if(!(in>>v)||!std::isfinite(v)||v<0||(!allow_zero&&v==0))throw std::runtime_error("invalid HDR numeric metadata");
    std::string extra;if(in>>extra)throw std::runtime_error("trailing HDR numeric metadata");return a;
}
}

core::Result<HdrEnvironmentImage> decode_hdr_environment(std::span<const std::uint8_t> bytes) {
    try {
        if(bytes.empty()||bytes.size()>32U*1024U*1024U)throw std::runtime_error("HDR source exceeds 32 MiB or is empty");
        std::size_t offset=0;
        const auto line=[&]() {
            const auto begin=offset;
            while(offset<bytes.size()&&bytes[offset]!='\n')++offset;
            if(offset==bytes.size()||offset-begin>4096||offset>65536)throw std::runtime_error("invalid or oversized HDR header");
            std::string s(reinterpret_cast<const char*>(bytes.data()+begin),offset++-begin);
            if(!s.empty()&&s.back()=='\r')s.pop_back();return s;
        };
        const auto signature=line();
        if(signature!="#?RADIANCE"&&signature!="#?RGBE")throw std::runtime_error("expected Radiance RGBE source");
        bool format=false,primaries=false;
        double exposure=1;V correction{1,1,1};
        // Radiance's documented default is equal-energy white and green (.29,.60).
        std::array<double,8> source{.640,.330,.290,.600,.150,.060,.3333,.3333};
        for(auto s=line();!s.empty();s=line()) {
            if(s.starts_with("FORMAT=")) {
                if(format||s!="FORMAT=32-bit_rle_rgbe")throw std::runtime_error("unsupported or duplicate HDR FORMAT");
                format=true;
            } else if(s.starts_with("EXPOSURE=")) exposure*=numbers<1>(s.substr(9))[0];
            else if(s.starts_with("COLORCORR=")){auto c=numbers<3>(s.substr(10));for(unsigned i=0;i<3;++i)correction[i]*=c[i];}
            else if(s.starts_with("PRIMARIES=")) {
                if(primaries)throw std::runtime_error("duplicate HDR primaries");source=numbers<8>(s.substr(10),true);primaries=true;
            } else if(s.starts_with("PIXASPECT=")&&std::abs(numbers<1>(s.substr(10))[0]-1)>1e-9)
                throw std::runtime_error("non-square HDR pixels are unsupported");
            else if(s.starts_with("GAMMA=")) throw std::runtime_error("gamma-tagged HDR is unsupported; supply scene-linear RGBE");
        }
        if(!format||!std::isfinite(exposure)||exposure<=0)throw std::runtime_error("missing FORMAT or invalid cumulative HDR exposure");
        for(double c:correction)if(!std::isfinite(c)||c<=0||!std::isfinite(c*exposure)||c*exposure<=0)
            throw std::runtime_error("invalid cumulative HDR color correction");
        std::string ys,xs,extra;std::uint32_t h=0,w=0;std::istringstream resolution(line());
        resolution.imbue(std::locale::classic());
        if(!(resolution>>ys>>h>>xs>>w)||(resolution>>extra)||(ys!="-Y"&&ys!="+Y")||(xs!="+X"&&xs!="-X")||
            h<2||w!=2*h||w>2048||h>1024)throw std::runtime_error("HDR environment requires Y-first 2:1 lat-long, at most 2048x1024");
        const auto source_matrix=rgb_xyz(source);
        constexpr std::array<double,8> rec709{.64,.33,.30,.60,.15,.06,.3127,.3290};
        const auto destination_matrix=inverse(rgb_xyz(rec709));
        const M bradford{{{.8951,.2664,-.1614},{-.7502,1.7135,.0367},{.0389,-.0685,1.0296}}};
        const auto undo_bradford=inverse(bradford);
        const auto from_white=mul(bradford,xyz(source[6],source[7]));
        const auto to_white=mul(bradford,xyz(rec709[6],rec709[7]));
        for(double v:from_white)if(v<=1e-9)throw std::runtime_error("invalid HDR white point");
        std::vector<core::Vec3d> pixels(std::size_t(w)*h);
        std::vector<std::array<std::uint8_t,4>> row(w);
        const auto byte=[&]() {if(offset>=bytes.size())throw std::runtime_error("truncated HDR pixels");return bytes[offset++];};
        for(std::uint32_t y=0;y<h;++y) {
            std::array<std::uint8_t,4> first{};for(auto& c:first)c=byte();
            if(w>=8&&first[0]==2&&first[1]==2&&first[2]<128) {
                if((unsigned(first[2])*256+first[3])!=w)throw std::runtime_error("HDR scanline width mismatch");
                for(unsigned c=0;c<4;++c)for(std::uint32_t x=0;x<w;) {
                    const auto token=byte();const unsigned count=token>128?token-128:token;
                    if(count==0||count>w-x)throw std::runtime_error("invalid HDR RLE packet length");
                    if(token>128){const auto v=byte();for(unsigned i=0;i<count;++i)row[x++][c]=v;}
                    else for(unsigned i=0;i<count;++i)row[x++][c]=byte();
                }
            } else {
                row[0]=first;
                for(std::uint32_t x=1;x<w;++x)for(auto& c:row[x])c=byte();
                for(const auto& p:row)if(p[0]==1&&p[1]==1&&p[2]==1)
                    throw std::runtime_error("legacy HDR repeat encoding is unsupported; resave planar RGBE");
            }
            for(std::uint32_t x=0;x<w;++x) {
                V rgb{};if(row[x][3])for(unsigned c=0;c<3;++c)
                    rgb[c]=std::ldexp(double(row[x][c]),int(row[x][3])-136)/(exposure*correction[c]);
                auto cone=mul(bradford,mul(source_matrix,rgb));
                for(unsigned c=0;c<3;++c)cone[c]*=to_white[c]/from_white[c];
                const auto working=mul(destination_matrix,mul(undo_bradford,cone));
                for(double c:working)if(!std::isfinite(c)||c < -1e-7||c>1e6)
                    throw std::runtime_error("HDR radiance exceeds the supported nonnegative linear Rec.709 gamut/range");
                const auto dx=xs=="+X"?x:w-1-x,dy=ys=="-Y"?y:h-1-y;
                pixels[std::size_t(dy)*w+dx]={std::max(0.,working[0]),std::max(0.,working[1]),std::max(0.,working[2])};
            }
        }
        if(offset!=bytes.size())throw std::runtime_error("trailing data after HDR image");
        HdrEnvironmentImage result;result.source_digest=sha256(bytes);result.source_width=w;result.source_height=h;
        result.height=std::min(h,64U);result.width=2*result.height;result.linear_radiance.resize(std::size_t(result.width)*result.height);
        const double scale=double(h)/result.height;
        for(unsigned y=0;y<result.height;++y)for(unsigned x=0;x<result.width;++x) {
            core::Vec3d sum{};double weight=0;
            const double x0=x*scale,x1=(x+1)*scale,y0=y*scale,y1=(y+1)*scale;
            for(unsigned sy=unsigned(y0);sy<std::min(h,unsigned(std::ceil(y1)));++sy) {
                const double latitude=std::cos(std::max(y0,double(sy))*std::numbers::pi/h)-std::cos(std::min(y1,double(sy+1))*std::numbers::pi/h);
                for(unsigned sx=unsigned(x0);sx<std::min(w,unsigned(std::ceil(x1)));++sx) {
                    const double a=(std::min(x1,double(sx+1))-std::max(x0,double(sx)))*latitude;
                    sum=sum+pixels[std::size_t(sy)*w+sx]*a;weight+=a;
                }
            }
            result.linear_radiance[std::size_t(y)*result.width+x]=sum*(1/weight);
        }
        return core::Result<HdrEnvironmentImage>::success(std::move(result));
    } catch(const std::exception& e) {
        return core::Result<HdrEnvironmentImage>::failure(core::Diagnostic(core::ErrorCode::invalid_argument,e.what()));
    }
}
} // namespace carto::assets
