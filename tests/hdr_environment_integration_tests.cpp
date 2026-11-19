#include "application_access.hpp"
#include "gltf_test_payload.hpp"
#include "../apps/desktop/studio_environment_import.hpp"
#include <carto/assets/hdr_environment.hpp>
#include <carto/production/authoring_viewport.hpp>
#include <locale>
#include <numbers>
namespace {
using namespace carto;
const std::string primaries="PRIMARIES=.64 .33 .30 .60 .15 .06 .3127 .3290\n";
std::vector<std::uint8_t> image(unsigned w,unsigned h,bool rle=false,std::string metadata=primaries,std::string orientation="-Y") {
    const auto header=std::string("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n")+metadata+"\n"+orientation+" "+std::to_string(h)+" +X "+std::to_string(w)+"\n";
    std::vector<std::uint8_t> b(header.begin(),header.end());
    for(unsigned y=0;y<h;++y) {
        const auto v=static_cast<std::uint8_t>(64+y%128);
        if(rle) {
            b.insert(b.end(),{2,2,static_cast<std::uint8_t>(w>>8),static_cast<std::uint8_t>(w)});
            for(unsigned c=0;c<4;++c)for(unsigned x=0;x<w;) {
                const auto n=std::min(w-x,127U);b.push_back(static_cast<std::uint8_t>(128+n));b.push_back(c==3?130:v);x+=n;
            }
        } else for(unsigned x=0;x<w;++x)b.insert(b.end(),{v,v,v,130});
    }
    return b;
}
std::vector<std::uint8_t> striped_image(unsigned w, unsigned h,
                                        std::string_view y_axis, std::string_view x_axis) {
    const auto header=std::string("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n")+primaries+"\n"+
        std::string(y_axis)+" "+std::to_string(h)+" "+std::string(x_axis)+" "+std::to_string(w)+"\n";
    std::vector<std::uint8_t> out(header.begin(),header.end());
    for (unsigned y=0; y<h; ++y) {
        const auto logical_y = y_axis == "-Y" ? y : h-1-y;
        out.insert(out.end(),{2,2,static_cast<std::uint8_t>(w>>8),static_cast<std::uint8_t>(w)});
        for (unsigned c=0; c<4; ++c) {
            const auto value = [&](unsigned x) -> std::uint8_t {
                const auto logical_x = x_axis == "+X" ? x : w-1-x;
                if (c==0) return static_cast<std::uint8_t>(logical_x < w/3 ? 64 : 128);
                if (c==1) return static_cast<std::uint8_t>(64 + logical_y%128);
                return static_cast<std::uint8_t>(c==2 ? 64 : 130);
            };
            for (unsigned x=0; x<w;) {
                const auto v=value(x);
                unsigned count=1;
                while (count<127 && x+count<w && value(x+count)==v) ++count;
                out.push_back(static_cast<std::uint8_t>(128+count));
                out.push_back(v);
                x+=count;
            }
        }
    }
    return out;
}
core::Vec3d stripe_reference(unsigned source_width, unsigned source_height,
                             unsigned target_height, unsigned x, unsigned y) {
    const double scale=double(source_height)/target_height;
    const double x0=x*scale,x1=(x+1)*scale,y0=y*scale,y1=(y+1)*scale;
    core::Vec3d sum{}; double weight=0;
    for (unsigned sy=unsigned(y0); sy<std::min(source_height,unsigned(std::ceil(y1))); ++sy) {
        const double latitude=std::cos(std::max(y0,double(sy))*std::numbers::pi/source_height)-
            std::cos(std::min(y1,double(sy+1))*std::numbers::pi/source_height);
        for (unsigned sx=unsigned(x0); sx<std::min(source_width,unsigned(std::ceil(x1))); ++sx) {
            const double area=(std::min(x1,double(sx+1))-std::max(x0,double(sx)))*latitude;
            sum=sum+core::Vec3d{sx<source_width/3 ? 1. : 2., 1.+double(sy%128)/64., 1.}*area;
            weight+=area;
        }
    }
    return sum*(1/weight);
}
void near(double a,double b,const char* label){check(std::abs(a-b)<1e-8,label);}
void send(application::ApplicationSession& s,const application::ApplicationAction& a){take(application::testing::dispatch(s,a));}
double rendered_radiance(application::ApplicationSession& s) {
    const auto snapshot=s.snapshot();auto document=take(s.authoring_viewport_source(snapshot.project_revision));
    production::AuthoringViewportRequest request;request.width=request.height=32;
    request.view.position={.5,.5,3};request.view.rotation={0,0,0,1};request.view.orthographic_height=1.5;
    const auto frame=take(production::render_authoring_viewport(document,request));
    double sum=0;unsigned covered=0;
    for(unsigned i=0;i<frame.objects.size();++i)if(frame.objects[i]){sum+=frame.linear_hdr[i].x;++covered;}
    check(covered>100,"imported environment lights a visible authored mesh");return sum/covered;
}
}
int main(){try {
    const auto flat=image(8,4),rle=image(8,4,true);
    const auto legacy_decoder = &assets::decode_hdr_environment;
    const auto a=take(legacy_decoder(flat)),b=take(assets::decode_hdr_environment(rle));
    {
        struct CommaDecimal final : std::numpunct<char> {
            char do_decimal_point() const override { return ','; }
        };
        struct RestoreLocale {
            std::locale previous;
            ~RestoreLocale() { std::locale::global(previous); }
        } restore{std::locale()};
        std::locale::global(std::locale(restore.previous, new CommaDecimal));
        const auto foreign = assets::decode_hdr_environment(flat);
        const auto comma_metadata = assets::decode_hdr_environment(
            image(4,2,false,primaries+"EXPOSURE=1,5\n"));
        std::locale::global(restore.previous);
        check(foreign && foreign.value().source_digest == a.source_digest &&
              foreign.value().linear_radiance.size() == a.linear_radiance.size(),
              "HDR metadata decoding is independent of the process decimal locale");
        for (std::size_t i=0; i<a.linear_radiance.size(); ++i)
            near(foreign.value().linear_radiance[i].x,a.linear_radiance[i].x,
                 "HDR linear radiance is unchanged across decimal locales");
        check(!comma_metadata, "HDR metadata rejects locale-specific decimal separators");
    }
    check(a.width==8&&a.height==4&&a.source_digest==assets::sha256(flat),"source identity and dimensions bound");
    for(unsigned i=0;i<a.linear_radiance.size();++i){near(a.linear_radiance[i].x,b.linear_radiance[i].x,"flat/RLE parity");near(a.linear_radiance[i].x,1.+double(i/8)/64,"linear HDR retained");}
    const auto flipped=take(assets::decode_hdr_environment(image(8,4,false,primaries,"+Y")));
    near(a.linear_radiance[0].x,flipped.linear_radiance[24].x,"vertical orientation normalized");
    auto horizontal=image(8,4);const std::string marker="+X 8";
    const auto pixel_start=horizontal.size()-8*4*4;
    for(unsigned x=0;x<8;++x)for(unsigned c=0;c<3;++c)horizontal[pixel_start+x*4+c]=static_cast<std::uint8_t>(64+x);
    const auto forward=take(assets::decode_hdr_environment(horizontal));
    auto it=std::search(horizontal.begin(),horizontal.end(),marker.begin(),marker.end());*it='-';
    const auto backward=take(assets::decode_hdr_environment(horizontal));
    for(unsigned x=0;x<8;++x)near(forward.linear_radiance[x].x,backward.linear_radiance[7-x].x,"horizontal flip normalized");
    auto color=image(4,2);color[color.size()-4*2*4]=128;color[color.size()-4*2*4+1]=64;color[color.size()-4*2*4+2]=32;
    const auto wedge=take(assets::decode_hdr_environment(color));
    near(wedge.linear_radiance[0].x,2,"linear red channel retained above display white");
    near(wedge.linear_radiance[0].y,1,"linear green channel retained");near(wedge.linear_radiance[0].z,.5,"linear blue channel retained");
    auto calibrated=take(assets::decode_hdr_environment(image(4,2,false,primaries+"EXPOSURE=2\nEXPOSURE=2\nCOLORCORR=2 2 2\n")));
    near(calibrated.linear_radiance[0].x,.125,"stored exposure and correction removed exactly once");
    auto defaults=take(assets::decode_hdr_environment(image(4,2,false,"")));
    near(defaults.linear_radiance[0].x,1,"default Radiance white adapted to D65");
    near(defaults.linear_radiance[0].y,1,"default Radiance green converted");
    for(std::size_t cut=0;cut<rle.size();++cut)check(!assets::decode_hdr_environment(std::span(rle).first(cut)),"every truncated prefix rejected");
    auto trailing=rle;trailing.push_back(0);check(!assets::decode_hdr_environment(trailing),"trailing image rejected");
    // Explicit malformed planar RLE packet with zero count.
    auto invalid=image(8,4,true);const auto resolution_end=std::search(invalid.begin(),invalid.end(),marker.begin(),marker.end())+static_cast<std::ptrdiff_t>(marker.size())+1;
    invalid[static_cast<std::size_t>(resolution_end-invalid.begin())+4]=0;
    check(!assets::decode_hdr_environment(invalid),"zero-length RLE fails");
    check(!assets::decode_hdr_environment(image(4,2,false,primaries+"FORMAT=32-bit_rle_rgbe\n")),"duplicate format rejected");
    check(!assets::decode_hdr_environment(image(4,2,false,primaries+"EXPOSURE=0\n")),"invalid exposure rejected");
    check(bool(assets::decode_hdr_environment(image(4,2,false,
        "PRIMARIES=.64 .33 0 .60 .15 .06 .3127 .3290\n"))),
        "valid zero x chromaticity is accepted");
    check(!assets::decode_hdr_environment(image(4,2,false,
        "PRIMARIES=.64 .33 .30 0 .15 .06 .3127 .3290\n")),
        "zero y chromaticity is rejected");
    check(!assets::decode_hdr_environment(image(4,2,false,primaries+"PIXASPECT=2\n")),"pixel aspect fails visibly");
    check(!assets::decode_hdr_environment(image(6,2)),"non lat-long extent rejected");
    const auto large=take(assets::decode_hdr_environment(image(512,256,true)));
    check(large.width==256&&large.height==128&&large.source_width==512,"higher-detail bounded lighting grid");
    {
        const auto four_k_source = image(4096,2048,true);
        const auto began = std::chrono::steady_clock::now();
        const auto four_k = take(assets::decode_hdr_environment_with_quality(
            four_k_source, assets::HdrLightingQuality::high));
        const auto decode_ms = std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-began).count();
        check(four_k.source_width == 4096 && four_k.source_height == 2048 &&
                  four_k.width == 512 && four_k.height == 256,
              "4K HDR sources produce a bounded high-detail lighting grid");
        double source_energy=0, output_energy=0;
        for (unsigned y=0; y<2048; ++y)
            source_energy+=(1+double(y%128)/64)*
                (std::cos(y*std::numbers::pi/2048)-std::cos((y+1)*std::numbers::pi/2048));
        for (unsigned y=0; y<256; ++y)
            output_energy+=four_k.linear_radiance[y*512].x*
                (std::cos(y*std::numbers::pi/256)-std::cos((y+1)*std::numbers::pi/256));
        near(source_energy,output_energy,"4K streaming preserves solid-angle weighted energy");
        const auto standard = take(assets::decode_hdr_environment(four_k_source));
        check(standard.width==256 && standard.height==128,
              "4K HDR sources retain the standard lower-memory grid option");
        const auto calibrated_4k = take(assets::decode_hdr_environment_with_quality(
            image(4096,2048,true,primaries+"EXPOSURE=2\nCOLORCORR=2 1 0.5\n"),
            assets::HdrLightingQuality::high));
        for (unsigned y : {0U,63U,128U,255U}) {
            const auto index=std::size_t(y)*four_k.width+256U;
            near(calibrated_4k.linear_radiance[index].x, four_k.linear_radiance[index].x/4,
                 "4K streaming removes red exposure and color correction once");
            near(calibrated_4k.linear_radiance[index].y, four_k.linear_radiance[index].y/2,
                 "4K streaming removes green exposure and color correction once");
            near(calibrated_4k.linear_radiance[index].z, four_k.linear_radiance[index].z,
                 "4K streaming removes blue exposure and color correction once");
        }
        const auto uncompressed_source = image(4096,2048,false);
        check(uncompressed_source.size()>32U*1024U*1024U &&
                  uncompressed_source.size()<assets::kMaxHdrEnvironmentSourceBytes,
              "uncompressed 4K RGBE fits the explicit encoded-input budget");
        const auto uncompressed = take(assets::decode_hdr_environment_with_quality(
            uncompressed_source, assets::HdrLightingQuality::high));
        for (unsigned y : {0U, 1U, 63U, 128U, 254U, 255U})
            for (unsigned x : {0U, 127U, 256U, 511U}) {
                const auto index=std::size_t(y)*four_k.width+x;
                check((four_k.linear_radiance[index]-uncompressed.linear_radiance[index]).length()<1e-8,
                      "4K flat and RLE sources decode to the same lighting grid");
            }
        check(!assets::decode_hdr_environment_with_quality(
                  std::span(four_k_source).first(four_k_source.size()-1), assets::HdrLightingQuality::high),
              "truncated 4K sources fail without producing a partial grid");
        project::AuthoredEnvironment imported;
        imported.identity="hdr."+four_k.source_digest.hex();
        imported.width=four_k.width; imported.height=four_k.height;
        imported.linear_radiance=four_k.linear_radiance;
        project::PresentationCatalog catalog;
        okay(catalog.set_environment(imported));
        const auto serialized=catalog.serialize();
        check(serialized.starts_with("CARTOGRAPHER_PRESENTATION_CATALOG 7\n"),
              "a high-detail 4K import uses the versioned catalog");
        application::ApplicationSession session;
        const auto snapshot=session.snapshot();
        send(session,application::SetPresentationCatalogAction{
            snapshot.project_generation,snapshot.project_revision,catalog});
        const auto path=std::filesystem::temp_directory_path()/
            ("cartographer-hdr-4k-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".carto");
        send(session,application::SaveProjectAction{path,std::nullopt,{},{},false});
        check(take(project::ProjectDocument::load(path)).presentation_catalog().serialize()==serialized,
              "a decoded 4K HDR source survives project save and reload");
        std::cout << "4K HDR streaming: encoded_bytes=" << four_k_source.size()
                  << "; grid_bytes=" << four_k.linear_radiance.size()*sizeof(core::Vec3d)
                  << "; decode_ms=" << decode_ms << '\n';
    }
    {
        constexpr unsigned w=2050,h=1025;
        const auto forward_stripes = take(assets::decode_hdr_environment_with_quality(
            striped_image(w,h,"-Y","+X"), assets::HdrLightingQuality::high));
        const auto reversed = take(assets::decode_hdr_environment_with_quality(
            striped_image(w,h,"+Y","-X"), assets::HdrLightingQuality::high));
        const unsigned split=(w/3)*forward_stripes.width/w;
        for (unsigned y : {0U,1U,63U,128U,254U,255U})
            for (unsigned x : {0U,split-1,split,split+1,511U}) {
                const auto index=std::size_t(y)*forward_stripes.width+x;
                const auto expected=stripe_reference(w,h,forward_stripes.height,x,y);
                check((forward_stripes.linear_radiance[index]-expected).length()<1e-8,
                      "fractional 4K-path bins agree with independent solid-angle integration");
                check((forward_stripes.linear_radiance[index]-reversed.linear_radiance[index]).length()<1e-8,
                      "streamed HDR axis flips preserve the same normalized directions");
            }
        const std::string beyond="#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2049 +X 4098\n";
        check(!assets::decode_hdr_environment(std::span(
                  reinterpret_cast<const std::uint8_t*>(beyond.data()),beyond.size())),
              "HDR dimensions beyond the 4K source bound fail explicitly");
        const std::vector<std::uint8_t> oversized(assets::kMaxHdrEnvironmentSourceBytes+1U);
        check(!assets::decode_hdr_environment(oversized),
              "HDR input beyond the encoded byte budget is rejected before decoding");
    }
    double input_energy=0,output_energy=0;
    for(unsigned y=0;y<256;++y)input_energy+=(1+double(y%128)/64)*(std::cos(y*std::numbers::pi/256)-std::cos((y+1)*std::numbers::pi/256));
    for(unsigned y=0;y<128;++y)output_energy+=large.linear_radiance[y*256].x*(std::cos(y*std::numbers::pi/128)-std::cos((y+1)*std::numbers::pi/128));
    near(input_energy,output_energy,"solid-angle filtering conserves environment energy");
    {
        project::AuthoredEnvironment detailed;
        detailed.identity="hdr.detail";
        detailed.width=large.width; detailed.height=large.height;
        detailed.linear_radiance=large.linear_radiance;
        project::PresentationCatalog detailed_catalog;
        okay(detailed_catalog.set_environment(detailed));
        const auto serialized=detailed_catalog.serialize();
        check(serialized.starts_with("CARTOGRAPHER_PRESENTATION_CATALOG 6\n"),
              "higher-detail environment uses a new catalog version");
        const auto reopened=take(project::PresentationCatalog::deserialize(serialized));
        check(reopened.environment()->width==256 && reopened.serialize()==serialized,
              "higher-detail environment survives exact catalog reload");
        const auto high_product=take(production::compile_environment_lighting(detailed));
        check(high_product->atlas_width()==256 && high_product->atlas_height()==335,
              "higher-detail source selects the bounded higher-quality atlas");
        const auto standard_product=take(production::compile_environment_lighting(
            detailed,production::EnvironmentLightingQuality::standard));
        check(standard_product->atlas_width()==128 &&
              standard_product->source_digest()==high_product->source_digest(),
              "standard tier remains available for the same source");
        application::ApplicationSession high_session;
        const auto high_snapshot=high_session.snapshot();
        send(high_session,application::SetPresentationCatalogAction{
            high_snapshot.project_generation,high_snapshot.project_revision,detailed_catalog});
        const auto high_path=std::filesystem::temp_directory_path() /
            ("cartographer-hdr-high-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".carto");
        send(high_session,application::SaveProjectAction{high_path, std::nullopt, {}, {}, false});
        const auto high_reopened=take(project::ProjectDocument::load(high_path));
        check(high_reopened.presentation_catalog().serialize()==serialized,
              "higher-detail source survives project save and reload");
        auto forged=serialized;
        forged.replace(forged.find("CATALOG 6"),9,"CATALOG 5");
        check(!project::PresentationCatalog::deserialize(forged),
              "old catalog versions cannot smuggle a newer oversized environment");
    }
    {
        auto sharp_source = image(512,256);
        const auto sharp_pixel_start = sharp_source.size() - 512U*256U*4U;
        const auto bright = sharp_pixel_start + (128U*512U + 256U)*4U;
        sharp_source[bright] = sharp_source[bright+1] = sharp_source[bright+2] = 255;
        const auto standard = take(assets::decode_hdr_environment(sharp_source));
        const auto high = take(assets::decode_hdr_environment_with_quality(
            sharp_source, assets::HdrLightingQuality::high));
        check(standard.width == 256 && standard.height == 128 &&
                  high.width == 512 && high.height == 256 &&
                  high.source_digest == standard.source_digest,
              "explicit high-detail HDR import retains the native 512x256 source grid");
        check(high.linear_radiance[128U*512U+256U].x >
                  standard.linear_radiance[64U*256U+128U].x * 1.5,
              "high-detail HDR import preserves a narrow bright feature lost by downsampling");
        check(!assets::decode_hdr_environment_with_quality(sharp_source,
                  static_cast<assets::HdrLightingQuality>(99)),
              "unknown HDR import quality fails explicitly");
        const auto filtered = take(assets::decode_hdr_environment_with_quality(
            image(1024,512,true), assets::HdrLightingQuality::high));
        check(filtered.width == 512 && filtered.height == 256,
              "high-detail HDR imports larger sources within their bounded grid");
        double source_energy = 0, filtered_energy = 0;
        for (unsigned y=0; y<512; ++y)
            source_energy += (1+double(y%128)/64) *
                (std::cos(y*std::numbers::pi/512)-std::cos((y+1)*std::numbers::pi/512));
        for (unsigned y=0; y<256; ++y)
            filtered_energy += filtered.linear_radiance[y*512].x *
                (std::cos(y*std::numbers::pi/256)-std::cos((y+1)*std::numbers::pi/256));
        near(source_energy, filtered_energy,
             "high-detail HDR downsampling conserves solid-angle weighted energy");
        project::AuthoredEnvironment source;
        source.identity = "hdr.high-detail";
        source.width = high.width;
        source.height = high.height;
        source.linear_radiance = high.linear_radiance;
        project::PresentationCatalog catalog;
        okay(catalog.set_environment(source));
        const auto serialized = catalog.serialize();
        check(serialized.starts_with("CARTOGRAPHER_PRESENTATION_CATALOG 7\n") &&
                  take(project::PresentationCatalog::deserialize(serialized)).serialize() == serialized,
              "512x256 HDR grids use v7 and survive exact catalog reload");
        auto forged = serialized;
        forged.replace(forged.find("CATALOG 7"),9,"CATALOG 6");
        check(!project::PresentationCatalog::deserialize(forged),
              "v6 cannot smuggle a v7-sized environment grid");
        application::ApplicationSession session;
        const auto snapshot = session.snapshot();
        send(session, application::SetPresentationCatalogAction{
            snapshot.project_generation, snapshot.project_revision, catalog});
        send(session, application::UndoAction{});
        check(!session.snapshot().presentation_catalog.environment(),
              "undo removes the high-detail HDR source");
        send(session, application::RedoAction{});
        check(session.snapshot().presentation_catalog.serialize() == serialized,
              "redo restores the exact high-detail HDR source");
        const auto path = std::filesystem::temp_directory_path() /
            ("cartographer-hdr-v7-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".carto");
        send(session, application::SaveProjectAction{path, std::nullopt, {}, {}, false});
        check(take(project::ProjectDocument::load(path)).presentation_catalog().serialize() == serialized,
              "high-detail HDR survives the real project save and reload path");
    }

    // Exercise the same preparation used by the desktop import button.
    {
        application::ApplicationSession session;
        project::AuthoredEnvironment imported;
        imported.identity="hdr."+a.source_digest.hex();
        imported.width=a.width;imported.height=a.height;imported.linear_radiance=a.linear_radiance;
        auto authored=imported;
        authored.enabled=false;authored.rotation_radians=.7;authored.intensity=2;
        auto before=session.snapshot();auto catalog=before.presentation_catalog;
        okay(catalog.set_environment(authored));
        send(session,application::SetPresentationCatalogAction{before.project_generation,before.project_revision,catalog});
        before=session.snapshot();
        const auto same=take(studio::prepare_environment_import(before.presentation_catalog,imported));
        check(same.serialize()==before.presentation_catalog.serialize(),"same HDR reimport is a no-op with authored controls and revision intact");
        check(!same.environment()->enabled && same.environment()->rotation_radians==.7 &&
              same.environment()->intensity==2 && same.environment()->revision==authored.revision,
              "same source preserves enabled, rotation, intensity and revision");
        auto changed=imported;changed.linear_radiance[0].x+=.25;
        auto refreshed=take(studio::prepare_environment_import(before.presentation_catalog,changed));
        check(!refreshed.environment()->enabled && refreshed.environment()->rotation_radians==.7 &&
              refreshed.environment()->intensity==2 && refreshed.environment()->revision==authored.revision.next(),
              "changed grid preserves controls and advances the source revision");
        send(session,application::SetPresentationCatalogAction{before.project_generation,before.project_revision,refreshed});
        check(session.snapshot().presentation_catalog.serialize()==refreshed.serialize(),
              "changed grid passes the normal presentation action");
        auto replacement=imported;replacement.identity="hdr.replacement";
        const auto replaced=take(studio::prepare_environment_import(refreshed,replacement));
        check(replaced.environment()->enabled && replaced.environment()->rotation_radians==0 &&
              replaced.environment()->intensity==1 && replaced.environment()->revision==core::Revision{1},
              "different HDR identity uses replacement defaults");
        auto invalid_environment=changed;invalid_environment.linear_radiance[0].x=-1;
        check(!studio::prepare_environment_import(refreshed,invalid_environment) &&
              session.snapshot().presentation_catalog.serialize()==refreshed.serialize(),
              "invalid reimport leaves existing lighting unchanged");
        auto exhausted=*refreshed.environment();exhausted.revision=core::Revision{core::Revision::max_value()};
        auto full=refreshed;okay(full.set_environment(exhausted));
        check(!studio::prepare_environment_import(full,imported) &&
              full.environment()->revision==exhausted.revision,
              "changed grid rejects an exhausted revision without modifying the catalog");
    }

    const auto root=std::filesystem::temp_directory_path()/("environment-edit-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(root);
    application::ApplicationSession s;send(s,application::SaveProjectAction{root/"scene.carto", {}, {}, {}, false});
    geometry::EditableMesh mesh;okay(mesh.insert_bulk({{{1},{0,0,0}},{{2},{1,0,0}},{{3},{1,1,0}},{{4},{0,1,0}}},{{{1},{{1},{2},{3}}},{{2},{{1},{3},{4}}}}));
    send(s,application::CreateMeshObjectAction{"HDR-lit authored quad",std::move(mesh)});
    project::AuthoredEnvironment env;env.identity="hdr."+a.source_digest.hex();env.width=a.width;env.height=a.height;env.linear_radiance=a.linear_radiance;
    auto snap=s.snapshot();auto catalog=snap.presentation_catalog;okay(catalog.set_environment(env));
    send(s,application::SetPresentationCatalogAction{snap.project_generation,snap.project_revision,catalog});
    const auto initial_radiance=rendered_radiance(s);check(initial_radiance>.01,"decoded source produces scene-linear illumination");
    snap=s.snapshot();catalog=snap.presentation_catalog;env.intensity=2;
    okay(catalog.set_environment(env));
    check(!application::testing::dispatch(s,application::SetPresentationCatalogAction{snap.project_generation,snap.project_revision,catalog}),"changed environment requires source revision increment");
    env.revision=env.revision.next();okay(catalog.set_environment(env));send(s,application::SetPresentationCatalogAction{snap.project_generation,snap.project_revision,catalog});
    near(rendered_radiance(s),initial_radiance*2,"native environment action changes reference HDR lighting without double exposure");
    send(s,application::UndoAction{});near(s.snapshot().presentation_catalog.environment()->intensity,1,"undo environment edit");
    send(s,application::RedoAction{});near(s.snapshot().presentation_catalog.environment()->intensity,2,"redo environment edit");
    snap=s.snapshot();catalog=snap.presentation_catalog;env.rotation_radians=.7;env.revision=env.revision.next();
    okay(catalog.set_environment(env));send(s,application::SetPresentationCatalogAction{snap.project_generation,snap.project_revision,catalog});
    send(s,application::SaveProjectAction{});const auto loaded=take(project::ProjectDocument::load(root/"scene.carto"));
    check(loaded.presentation_catalog().serialize()==s.snapshot().presentation_catalog.serialize(),"HDR source, controls and identity survive reopen");
    const auto exported=take(io::export_glb(loaded,root/"lit.glb"));
    check(std::any_of(exported.warnings.begin(),exported.warnings.end(),[](const auto& warning){return warning.find("HDR environment lighting")!=std::string::npos;}),"export explicitly reports consumer lighting boundary");
    snap=s.snapshot();catalog=snap.presentation_catalog;env.identity="replacement";okay(catalog.set_environment(env));
    check(!application::testing::dispatch(s,application::SetPresentationCatalogAction{snap.project_generation,snap.project_revision,catalog}),"replacement identity starts at revision one");
    env.revision=core::Revision{1};env.linear_radiance[0].x=-1;check(!catalog.set_environment(env),"invalid replacement rejected before publication");
    check(s.snapshot().presentation_catalog.serialize()==loaded.presentation_catalog().serialize(),"failed replacement preserves good source");
    std::filesystem::remove_all(root);return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
