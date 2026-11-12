#include <carto/production/authoring_material_bake.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace carto::production {
core::Result<AuthoringMaterialBake> bake_authoring_material(
    const AuthoringMaterialProgram& program, std::uint32_t resolution) {
    using Result=core::Result<AuthoringMaterialBake>;
    try {
        if(!authoring_material_bake_fits_budget(resolution))
            throw std::runtime_error("material bake resolution must be a power of two from "+
                std::to_string(kMinAuthoringMaterialBakeResolution)+" through "+
                std::to_string(kMaxAuthoringMaterialBakeResolution));
        const auto packet=program.gpu_packet();
        if(!packet)return Result::failure(packet.error());
        const double step=1./resolution;
        const auto sample=[&](unsigned x,unsigned y){return program.evaluate({{(x+.5)*step,(y+.5)*step},{step,0},{0,step}});};
        double emission_scale=1;
        for(unsigned y=0;y<resolution;++y)for(unsigned x=0;x<resolution;++x) {
            const auto value=sample(x,y);
            if(!value)return Result::failure(value.error());
            const auto& m=value.value().material;
            if(m.base_color_factor.x>1||m.base_color_factor.y>1||m.base_color_factor.z>1)
                throw std::runtime_error("HDR base color cannot be represented by the bounded RGBA8 material bake");
            for(double v:{m.emissive_factor.x,m.emissive_factor.y,m.emissive_factor.z}) {
                const auto radiance=v*m.emissive_strength;
                if(!std::isfinite(radiance))throw std::runtime_error("material bake emission overflows");
                emission_scale=std::max(emission_scale,radiance);
            }
        }
        auto center=sample(resolution/2,resolution/2);
        if(!center)return Result::failure(center.error());
        AuthoringMaterialBake result;
        result.program_digest=packet.value().digest;
        result.uv=program.uv_set().value_or(1);
        result.normal_mapping=program.normal_mapping();
        result.factors=center.value().material;
        result.factors.base_color_factor={1,1,1,1};
        result.factors.roughness=result.factors.metallic=1;
        result.factors.normal_scale=result.factors.occlusion_strength=1;
        result.factors.emissive_factor={1,1,1};result.factors.emissive_strength=emission_scale;
        result.factors.clearcoat=result.factors.clearcoat_roughness=1;
        for(auto& map:result.maps){map.width=map.height=resolution;map.pixels.resize(std::size_t(resolution)*resolution*4);}
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
            const std::array<std::array<double,4>,6> channels{{
                {m.base_color_factor.x,m.base_color_factor.y,m.base_color_factor.z,m.base_color_factor.w},
                {1,m.roughness,m.metallic,1}, {(n.x+1)*.5,(n.y+1)*.5,(n.z+1)*.5,1},
                {s.ambient_occlusion,1,1,1},
                {m.emissive_factor.x*m.emissive_strength/emission_scale,m.emissive_factor.y*m.emissive_strength/emission_scale,m.emissive_factor.z*m.emissive_strength/emission_scale,1},
                {m.clearcoat,m.clearcoat_roughness,0,1}}};
            const auto p=(std::size_t(y)*resolution+x)*4;
            for(unsigned map=0;map<6;++map)for(unsigned c=0;c<4;++c)
                result.maps[map].pixels[p+c]=quantize(channels[map][c],(map==0||map==4)&&c<3);
        }
        return Result::success(std::move(result));
    } catch(const std::exception& e) {
        return Result::failure({core::ErrorCode::validation_failed,std::string("material bake: ")+e.what()});
    }
}
} // namespace carto::production
