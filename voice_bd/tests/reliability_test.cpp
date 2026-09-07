#include "../voicebd.h"
#include <array>
#include <thread>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>
static uint32_t le32(unsigned char const* p) {
 return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
static uint16_t le16(unsigned char const* p) {return uint16_t(p[0]) | uint16_t(p[1])<<8;}
int main(int argc, char** argv) {
 try {
  std::ifstream f(argc > 1 ? argv[1] : "sample.wav",std::ios::binary);
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),{});
  if(bytes.size()<12)throw std::runtime_error("cannot read WAV");
  unsigned channels=0,rate=0,bits=0,format=0;size_t data=0,size=0;
  for(size_t at=12;at+8<=bytes.size();) {
   size_t n=le32(bytes.data()+at+4);size_t begin=at+8;
   if(n>bytes.size()-begin)throw std::runtime_error("truncated WAV");
   if(std::equal(bytes.begin()+at,bytes.begin()+at+4,"fmt ")&&n>=16){
    format=le16(bytes.data()+begin);channels=le16(bytes.data()+begin+2);
    rate=le32(bytes.data()+begin+4);bits=le16(bytes.data()+begin+14);
   }
   if(std::equal(bytes.begin()+at,bytes.begin()+at+4,"data")){data=begin;size=n;}
   at=begin+n+(n&1);
  }
  if(format!=1||bits!=16||rate!=48000||(channels!=1&&channels!=2)||!data)
   throw std::runtime_error("expected 48kHz 16-bit PCM mono/stereo");
  std::vector<int16_t> pcm(size/(2*channels));
  for(size_t i=0;i<pcm.size();++i){
   int value=int16_t(le16(bytes.data()+data+i*2*channels));
   if(channels==2)value=(value+int16_t(le16(bytes.data()+data+i*4+2)))/2;
   pcm[i]=int16_t(value);
  }
  std::string const mode = argc > 3 ? argv[3] : "burst";
  if(mode != "burst" && mode != "steady" && mode != "startups")
   throw std::runtime_error("mode must be burst, steady, or startups");
  unsigned const key = argc > 4 ? std::stoul(argv[4]) : 60u;
  if(key > 127u) throw std::runtime_error("key must be 0..127");
  voice_board_t board;
  voice_board_config_t config;
  if(argc > 2) config.bec_file = argv[2];
  config.initial_attenuation_db = 40;
  auto check = [](voice_board_result_t const& result) {
   if(!result) throw std::runtime_error(result.message);
  };
  check(board.open(config));
  for(uint8_t i=0;i<8;++i) check(board.load_sample(i,pcm));
  using clock = std::chrono::steady_clock;
  std::vector<double> on_ms,off_ms;
  on_ms.reserve(4096);off_ms.reserve(4096);
  auto on = [&](uint8_t v,uint8_t key) {
   auto start=clock::now();auto result=board.note_on(v,v,key,100);
   on_ms.push_back(std::chrono::duration<double,std::milli>(clock::now()-start).count());
   if(!result)std::cerr<<"Failed note-on "<<on_ms.size()<<" voice "<<unsigned(v)<<" key "<<unsigned(key)<<"\n";
   check(result);
  };
  auto off = [&](uint8_t v) {
   auto start=clock::now();auto result=board.note_off(v);
   off_ms.push_back(std::chrono::duration<double,std::milli>(clock::now()-start).count());
   if(!result)std::cerr<<"Failed note-off "<<off_ms.size()<<" after "<<on_ms.size()<<" note-ons\n";
   check(result);
  };
  std::cout<<"Program "<<config.bec_file<<", mode "<<mode<<", eight sample slots, no MIDI\n"<<std::flush;
  auto const begin=clock::now();
  if(mode=="steady") {
   for(uint8_t v=0;v<8;++v)on(v,static_cast<uint8_t>(key));
   std::this_thread::sleep_for(std::chrono::seconds(30));
  } else if(mode=="startups") {
   for(unsigned round=0;round<64;++round) {
    for(uint8_t v=0;v<8;++v)on(v,static_cast<uint8_t>(key));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for(uint8_t v=0;v<8;++v)off(v);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
   }
  } else {
   for(unsigned i=0;i<4096;++i) {
    uint8_t v=i%8;
    if(i>=8 && i%3==0)off(v);
    on(v,48+i%13);
   }
   std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  check(board.all_notes_off());
  check(board.set_attenuation(0));
  check(board.close());
  std::cout<<"PASS "<<on_ms.size()<<" note-ons, "<<off_ms.size()<<" note-offs, "
   <<std::chrono::duration<double>(clock::now()-begin).count()<<" seconds\n";
  auto report=[](std::vector<double> times,char const* name) {
   if(times.empty())return;
   double first=times.front();std::sort(times.begin(),times.end());
   std::cout<<name<<" ms: first="<<first<<" median="<<times[times.size()/2]
    <<" p99="<<times[(times.size()-1)*99/100]<<" max="<<times.back()<<"\n";
  };
  report(on_ms,"note-on");report(off_ms,"note-off");
 } catch(std::exception const& e){std::cerr<<e.what()<<'\n';return 1;}
}
