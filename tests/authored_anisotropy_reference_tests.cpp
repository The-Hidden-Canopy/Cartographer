#include <carto/d3d12/authored_anisotropy.hpp>
#include <iostream>
#include <limits>
#include <stdexcept>
using carto::core::Vec3d;
using carto::d3d12::evaluate_authored_anisotropy_reference;
void check(bool ok) { if (!ok) throw std::runtime_error("authored anisotropy reference check failed"); }
bool near(double a,double b) { return std::abs(a-b) <= 1e-10*std::max({1.0,std::abs(a),std::abs(b)}); }
auto eval(double r,double s,double rot,Vec3d v,Vec3d l) {
    auto result=evaluate_authored_anisotropy_reference(r,s,rot,v,l);
    check(bool(result)); return result.value();
}
int main() { try {
    const Vec3d n{0,0,1}, v=Vec3d{0.4,0.2,1}.normalized(), l=Vec3d{-0.3,0.5,1}.normalized();
    for(double r : {0.0,0.1,0.5,1.0}) for(double s : {0.0,0.2,1.0}) {
        auto a=eval(r,s,0,n,n);
        check(near(a.alpha_b,std::max(1e-4,r*r)));
        check(near(a.alpha_t,a.alpha_b+(1-a.alpha_b)*s*s));
        check(near(a.distribution,1/(std::numbers::pi*a.alpha_t*a.alpha_b)));
        check(near(a.visibility,0.25));
        for(double z : {1.0,0.01,1e-8,1e-100}) {
            auto x=Vec3d{1,0,z}.normalized(), y=Vec3d{-1,0.1,z}.normalized();
            const auto p=eval(r,s,0.7,x,y), q=eval(r,s,0.7,y,x);
            check(std::isfinite(p.kernel) && p.kernel>=0 && near(p.kernel,q.kernel));
        }
    }
    check(std::isfinite(eval(.5,.8,0,Vec3d{1,0,1e-320},Vec3d{-1,0,1e-320}).kernel));
    // Independent isotropic GGX + clamped Smith result at zero strength.
    const auto h=(v+l).normalized(); const double alpha=0.25;
    const double d=alpha*alpha/(std::numbers::pi*std::pow(h.z*h.z*(alpha*alpha-1)+1,2));
    const double visibility=std::min(1.0,0.5/(l.z*std::sqrt(alpha*alpha*(1-v.z*v.z)+v.z*v.z)+v.z*std::sqrt(alpha*alpha*(1-l.z*l.z)+l.z*l.z)));
    check(near(eval(.5,0,0,v,l).kernel,d*visibility));
    check(near(eval(.5,0,1.7,v,l).kernel,d*visibility));
    check(near(eval(.5,1e-8,0,v,l).kernel,eval(.5,0,0,v,l).kernel));
    const double quarter=std::numbers::pi/2;
    check(near(eval(.5,.8,quarter,v,l).kernel,eval(.5,.8,0,{v.y,-v.x,v.z},{l.y,-l.x,l.z}).kernel));
    check(!near(eval(.5,.8,0,v,l).kernel,eval(.5,.8,quarter,v,l).kernel));
    check(near(eval(.5,.8,.3,v,l).kernel,eval(.5,.8,-.3,{v.x,-v.y,v.z},{l.x,-l.y,l.z}).kernel));
    check(near(eval(.5,.8,.3,v,l).kernel,eval(.5,.8,.3+2*std::numbers::pi,v,l).kernel));
    check(eval(.5,.8,0,{0,0,-1},l).kernel==0 && eval(.5,.8,0,v,{1,0,0}).kernel==0);
    for(double bad : {-0.1,1.1,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        check(!evaluate_authored_anisotropy_reference(bad,.5,0,v,l));
        check(!evaluate_authored_anisotropy_reference(.5,bad,0,v,l));
    }
    check(!evaluate_authored_anisotropy_reference(.5,.5,std::numeric_limits<double>::infinity(),v,l));
    check(!evaluate_authored_anisotropy_reference(.5,.5,0,{0,0,0},l));
    check(!evaluate_authored_anisotropy_reference(.5,.5,0,v,{0,0,2}));
    check(!evaluate_authored_anisotropy_reference(.5,.5,0,v,{0,0,std::numeric_limits<double>::quiet_NaN()}));
    std::cout<<"PASS authored anisotropy CPU reference\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
