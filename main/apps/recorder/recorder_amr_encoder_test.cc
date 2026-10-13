#include "recorder_amr_encoder.h"
#include <algorithm>
#include <cassert>

static std::vector<uint8_t> Encode(size_t chunk) {
    RecorderAmrEncoder encoder([](const int16_t* pcm, std::vector<uint8_t>* out) {
        uint32_t sum=0;for(int i=0;i<160;++i)sum+=pcm[i];
        out->push_back(0x3c);out->insert(out->end(),31,static_cast<uint8_t>(sum));return true;
    });
    assert(encoder.Reset());std::vector<int16_t> input(16013);
    for(size_t i=0;i<input.size();++i)input[i]=i%997;
    std::vector<uint8_t> output;
    for(size_t i=0;i<input.size();i+=chunk){
        auto end=std::min(input.size(),i+chunk);
        assert(encoder.Process({input.begin()+i,input.begin()+end},&output));
    }
    assert(encoder.Flush(&output));
    assert(output.size()==51*32);
    assert(!encoder.Process({},&output));
    assert(encoder.Reset());std::vector<uint8_t> next;
    assert(encoder.Process(std::vector<int16_t>(320),&next));assert(encoder.Flush(&next));
    assert(next.size()==32);
    return output;
}
int main(){assert(Encode(85)==Encode(16013));assert(Encode(1024)==Encode(16013));
    RecorderAmrEncoder failing([](const int16_t*,std::vector<uint8_t>*){return false;});
    std::vector<uint8_t> out;assert(failing.Reset());assert(!failing.Process(std::vector<int16_t>(320),&out));
}
