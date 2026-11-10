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
    const auto a=take(assets::decode_hdr_environment(flat)),b=take(assets::decode_hdr_environment(rle));
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
    check(large.width==128&&large.height==64&&large.source_width==512,"explicit bounded lighting grid");
    double input_energy=0,output_energy=0;
    for(unsigned y=0;y<256;++y)input_energy+=(1+double(y%128)/64)*(std::cos(y*std::numbers::pi/256)-std::cos((y+1)*std::numbers::pi/256));
    for(unsigned y=0;y<64;++y)output_energy+=large.linear_radiance[y*128].x*(std::cos(y*std::numbers::pi/64)-std::cos((y+1)*std::numbers::pi/64));
    near(input_energy,output_energy,"solid-angle filtering conserves environment energy");

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
        auto invalid=changed;invalid.linear_radiance[0].x=-1;
        check(!studio::prepare_environment_import(refreshed,invalid) &&
              session.snapshot().presentation_catalog.serialize()==refreshed.serialize(),
              "invalid reimport leaves existing lighting unchanged");
        auto exhausted=*refreshed.environment();exhausted.revision=core::Revision{core::Revision::max_value()};
        auto full=refreshed;okay(full.set_environment(exhausted));
        check(!studio::prepare_environment_import(full,imported) &&
              full.environment()->revision==exhausted.revision,
              "changed grid rejects an exhausted revision without modifying the catalog");
    }

    const auto root=std::filesystem::temp_directory_path()/("environment-edit-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(root);
    application::ApplicationSession s;send(s,application::SaveProjectAction{root/"scene.carto"});
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
    send(s,application::SaveProjectAction{std::nullopt});const auto loaded=take(project::ProjectDocument::load(root/"scene.carto"));
    check(loaded.presentation_catalog().serialize()==s.snapshot().presentation_catalog.serialize(),"HDR source, controls and identity survive reopen");
    const auto exported=take(io::export_glb(loaded,root/"lit.glb"));
    check(std::any_of(exported.warnings.begin(),exported.warnings.end(),[](const auto& warning){return warning.find("HDR environment lighting")!=std::string::npos;}),"export explicitly reports consumer lighting boundary");
    snap=s.snapshot();catalog=snap.presentation_catalog;env.identity="replacement";okay(catalog.set_environment(env));
    check(!application::testing::dispatch(s,application::SetPresentationCatalogAction{snap.project_generation,snap.project_revision,catalog}),"replacement identity starts at revision one");
    env.revision=core::Revision{1};env.linear_radiance[0].x=-1;check(!catalog.set_environment(env),"invalid replacement rejected before publication");
    check(s.snapshot().presentation_catalog.serialize()==loaded.presentation_catalog().serialize(),"failed replacement preserves good source");
    std::filesystem::remove_all(root);return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
