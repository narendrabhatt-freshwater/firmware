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
  voice_board_t board;
  voice_board_config_t config;
  if (argc > 2) config.bec_file = argv[2];
  config.initial_attenuation_db = 40;
  auto check = [](voice_board_result_t const& result) {
   if (!result) throw std::runtime_error(result.message);
  };
  check(board.open(config));
  std::cout << "Program: " << config.bec_file << '\n';
  using clock = std::chrono::steady_clock;
  std::array<double, 8> load_ms{}, note_ms{};
  for (uint8_t i = 0; i < 8; ++i) {
   auto const start = clock::now();
   auto const result = board.load_sample(i, pcm);
   load_ms[i] = std::chrono::duration<double, std::milli>(clock::now() - start).count();
   check(result);
  }
  auto const burst_start = clock::now();
  for (uint8_t i = 0; i < 8; ++i) {
   auto const start = clock::now();
   auto const result = board.note_on(i, i, 60, 100);
   note_ms[i] = std::chrono::duration<double, std::milli>(clock::now() - start).count();
   check(result);
  }
  double const burst_ms = std::chrono::duration<double, std::milli>(clock::now() - burst_start).count();
  std::cout << "PCM samples per slot: " << pcm.size() << '\n';
  for (unsigned i = 0; i < 8; ++i)
   std::cout << "load_sample " << i << ": " << load_ms[i] << " ms\n";
  for (unsigned i = 0; i < 8; ++i)
   std::cout << "n" << i << " on: " << note_ms[i] << " ms\n";
  std::cout << "Eight note-on calls: " << burst_ms << " ms\n";
  std::this_thread::sleep_for(std::chrono::seconds(1));
  check(board.all_notes_off());
  check(board.set_attenuation(0));
  check(board.close());
 } catch(std::exception const& e){std::cerr<<e.what()<<'\n';return 1;}
}
