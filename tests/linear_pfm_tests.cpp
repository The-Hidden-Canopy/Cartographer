#include "apps/cli/linear_pfm.hpp"
#include "libs/assets/include/carto/assets/linear_float_image.hpp"
#include <stdexcept>
void require(bool ok) { if (!ok) throw std::runtime_error("PFM test failed"); }
#include <limits>
struct Image { std::uint32_t width=1,height=2; std::vector<float> linear_hdr_rgba{4.0f,0.5f,0.25f,1, 2.0f,3.0f,8.0f,1}; };
int main() {
 Image a; auto bytes=encode_linear_pfm(a); auto decoded=carto::assets::decode_linear_pfm(bytes);
 require(decoded.width==1 && decoded.height==2 && decoded.rgba==a.linear_hdr_rgba);
 auto broken=bytes; broken.pop_back(); bool rejected=false; try {(void)carto::assets::decode_linear_pfm(broken);}catch(const std::runtime_error&){rejected=true;} require(rejected);
 a.linear_hdr_rgba[0]=std::numeric_limits<float>::infinity(); rejected=false; try {(void)encode_linear_pfm(a);}catch(const std::runtime_error&){rejected=true;} require(rejected);
}
