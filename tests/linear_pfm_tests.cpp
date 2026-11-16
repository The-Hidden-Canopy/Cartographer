#include "apps/cli/linear_pfm.hpp"
#include "libs/assets/include/carto/assets/linear_float_image.hpp"
#include <stdexcept>
void require(bool ok) { if (!ok) throw std::runtime_error("PFM test failed"); }
#include <limits>
#include <bit>
#include <string>
struct Image { std::uint32_t width=1,height=2; std::vector<float> linear_hdr_rgba{4.0f,0.5f,0.25f,1, 2.0f,3.0f,8.0f,1}; };
int main() {
 Image a; auto bytes=encode_linear_pfm(a); auto decoded=carto::assets::decode_linear_pfm(bytes);
 auto chain=carto::assets::generate_linear_float_mips(decoded, carto::assets::sha256(bytes));
 require(carto::assets::validate_linear_float_mips(chain) && chain.levels.size()==2 && chain.levels.back().rgba[0]==3.0f && chain.levels.back().rgba[2]==4.125f);
 auto persisted=carto::assets::serialize_linear_float_mips(chain); auto restored=carto::assets::deserialize_linear_float_mips(persisted);
 require(restored.artifact_digest==chain.artifact_digest && restored.levels[0].rgba==chain.levels[0].rgba);
 persisted.back()^=1; bool corrupt=false; try{(void)carto::assets::deserialize_linear_float_mips(persisted);}catch(const std::runtime_error&){corrupt=true;} require(corrupt);
 chain.levels[0].rgba[0]=5.0f; require(!carto::assets::validate_linear_float_mips(chain));
 require(decoded.width==1 && decoded.height==2 && decoded.rgba==a.linear_hdr_rgba);
 auto broken=bytes; broken.pop_back(); bool rejected=false; try {(void)carto::assets::decode_linear_pfm(broken);}catch(const std::runtime_error&){rejected=true;} require(rejected);
 // A positive scale declares big-endian samples; its magnitude scales them.
 const auto pfm=[](const char* scale, bool big_endian, float value) {
  const std::string header=std::string("PF\n1 1\n")+scale+"\n"; std::vector<std::uint8_t> out(header.begin(),header.end());
  const auto bits=std::bit_cast<std::uint32_t>(value);
  for(unsigned c=0;c<3;++c) for(unsigned b=0;b<4;++b) out.push_back(static_cast<std::uint8_t>(bits>>(big_endian ? (3U-b)*8U : b*8U)));
  return out; };
 const auto big=carto::assets::decode_linear_pfm(pfm("2.0",true,1.5f));
 require(big.rgba[0]==3.0f && big.rgba[1]==3.0f && big.rgba[2]==3.0f && big.rgba[3]==1.0f);
 const auto little=carto::assets::decode_linear_pfm(pfm("-0.5",false,6.0f));
 require(little.rgba[0]==3.0f && little.rgba[2]==3.0f);
 for (const char* bad : {"0", "-0.0", "inf", "nan", "1e40", "x"}) {
  rejected=false; try {(void)carto::assets::decode_linear_pfm(pfm(bad,false,1.0f));}catch(const std::runtime_error&){rejected=true;} require(rejected); }
 rejected=false; try {(void)carto::assets::decode_linear_pfm(pfm("4.0",true,std::numeric_limits<float>::max()));}catch(const std::runtime_error&){rejected=true;} require(rejected);
 a.linear_hdr_rgba[0]=std::numeric_limits<float>::infinity(); rejected=false; try {(void)encode_linear_pfm(a);}catch(const std::runtime_error&){rejected=true;} require(rejected);
}
