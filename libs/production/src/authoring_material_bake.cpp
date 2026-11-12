#include <carto/production/authoring_material_bake.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>
namespace carto::production {
core::Result<AuthoringMaterialBake> bake_authoring_material(
    const AuthoringMaterialProgram& program, std::uint32_t resolution,
    AuthoringMaterialBakeDomain domain) {
    using Result=core::Result<AuthoringMaterialBake>;
    try {
        if(!authoring_material_bake_fits_budget(resolution))
            throw std::runtime_error("material bake resolution must be a power of two from "+
                std::to_string(kMinAuthoringMaterialBakeResolution)+" through "+
                std::to_string(kMaxAuthoringMaterialBakeResolution));
        if(!program.executable())
            return Result::failure({core::ErrorCode::invalid_argument,
                "material bake requires a compiled material program"});
        const auto coordinates=[&](double origin,double span) {
            const double end=origin+span;
            if(!std::isfinite(origin)||!std::isfinite(span)||span<=0.0||
                !std::isfinite(end)||std::abs(origin)>1e6||std::abs(end)>1e6||end<=origin)
                throw std::runtime_error("material bake domain requires bounded increasing finite UV endpoints");
            const double step=span/resolution;
            if(!std::isfinite(step)||step<=0.0)
                throw std::runtime_error("material bake domain has an unrepresentable UV derivative");
            std::vector<double> centers;
            centers.reserve(resolution);
            double previous=origin;
            for(unsigned i=0;i<resolution;++i) {
                const double center=origin+(i+.5)*step;
                if(!std::isfinite(center)||center<=previous||center>=end)
                    throw std::runtime_error("material bake domain collapses distinct UV pixel centers");
                centers.push_back(center);
                previous=center;
            }
            return centers;
        };
        const auto u=coordinates(domain.origin.x,domain.span.x);
        const auto v=coordinates(domain.origin.y,domain.span.y);
        const auto packet=program.gpu_packet();
        if(!packet)return Result::failure(packet.error());
        const double step_u=domain.span.x/resolution;
        const double step_v=domain.span.y/resolution;
        const auto sample=[&](unsigned x,unsigned y){return program.evaluate({{u[x],v[y]},{step_u,0},{0,step_v}});};
        AuthoringMaterialBake result;
        result.program_digest=packet.value().digest;
        result.domain=domain;
        result.uv=program.uv_set().value_or(1);
        result.normal_mapping=program.normal_mapping();
        const auto texels=std::size_t(resolution)*resolution;
        for(auto& map:result.maps){map.width=map.height=resolution;map.pixels.resize(texels*4);}
        // Only emission depends on a global scale. Retain its exact linear
        // values instead of reevaluating the entire material program. The
        // scratch cost is 3 doubles per texel (24 MiB at 1024 squared), not
        // an unbounded cache of complete surface samples or graph registers.
        std::vector<std::array<double,3>> emission(texels);
        double emission_scale=1;
        const auto quantize=[](double v,bool color) {
            if(!std::isfinite(v)||v < -1e-7||v > 1.0000001)throw std::runtime_error("material bake channel outside supported range");
            v=std::clamp(v,0.,1.);
            if(color)v=v<=.0031308?12.92*v:1.055*std::pow(v,1./2.4)-.055;
            return static_cast<std::uint8_t>(std::lround(v*255));
        };
        for(unsigned y=0;y<resolution;++y)for(unsigned x=0;x<resolution;++x) {
            const auto value=sample(x,y);if(!value)return Result::failure(value.error());
            const auto& s=value.value();const auto& m=s.material;
            const auto& n=s.tangent_normal;
            if(m.base_color_factor.x>1||m.base_color_factor.y>1||m.base_color_factor.z>1)
                throw std::runtime_error("HDR base color cannot be represented by the bounded RGBA8 material bake");
            const auto texel=std::size_t(y)*resolution+x;
            emission[texel]={m.emissive_factor.x*m.emissive_strength,
                m.emissive_factor.y*m.emissive_strength,m.emissive_factor.z*m.emissive_strength};
            for(const double radiance:emission[texel]) {
                if(!std::isfinite(radiance))throw std::runtime_error("material bake emission overflows");
                emission_scale=std::max(emission_scale,radiance);
            }
            if(x==resolution/2&&y==resolution/2)result.factors=m;
            const std::array<std::array<double,4>,6> channels{{
                {m.base_color_factor.x,m.base_color_factor.y,m.base_color_factor.z,m.base_color_factor.w},
                {1,m.roughness,m.metallic,1}, {(n.x+1)*.5,(n.y+1)*.5,(n.z+1)*.5,1},
                {s.ambient_occlusion,1,1,1},
                {0,0,0,1},
                {m.clearcoat,m.clearcoat_roughness,0,1}}};
            const auto p=(std::size_t(y)*resolution+x)*4;
            for(unsigned map=0;map<6;++map) {
                if(map==4)continue;
                for(unsigned c=0;c<4;++c)
                    result.maps[map].pixels[p+c]=quantize(channels[map][c],map==0&&c<3);
            }
        }
        for(std::size_t texel=0;texel<texels;++texel) {
            for(unsigned c=0;c<3;++c)
                result.maps[4].pixels[texel*4+c]=quantize(emission[texel][c]/emission_scale,true);
            result.maps[4].pixels[texel*4+3]=255;
        }
        result.factors.base_color_factor={1,1,1,1};
        result.factors.roughness=result.factors.metallic=1;
        result.factors.normal_scale=result.factors.occlusion_strength=1;
        result.factors.emissive_factor={1,1,1};result.factors.emissive_strength=emission_scale;
        result.factors.clearcoat=result.factors.clearcoat_roughness=1;
        return Result::success(std::move(result));
    } catch(const std::exception& e) {
        return Result::failure({core::ErrorCode::validation_failed,std::string("material bake: ")+e.what()});
    }
}
core::Result<AuthoringMaterialBake> bake_authoring_material(
    const AuthoringMaterialProgram& program, std::uint32_t resolution) {
    return bake_authoring_material(program,resolution,{});
}
} // namespace carto::production
