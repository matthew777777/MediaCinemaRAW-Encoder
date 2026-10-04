#include <MediaCinemaRAW/ContainerWriter.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
namespace mediacinemaraw { namespace {
// Buffered little-endian assembly: sample loops append to a vector and flush
// with one write() instead of a put() per byte. Same bytes, same order.
template<class T> void putLe(std::vector<uint8_t>& b,T v){for(size_t i=0;i<sizeof(T);i++)b.push_back(uint8_t(uint64_t(v)>>(8*i)));}
void putLeF(std::vector<uint8_t>& b,float v){uint32_t n;std::memcpy(&n,&v,4);putLe(b,n);}
uint32_t ck(size_t n){if(n>UINT32_MAX)throw std::length_error("item too large");return uint32_t(n);}
// Checked payload sizes: validate before any narrowing or multiplication.
uint32_t audioPayload(size_t n){
  if(n>UINT32_MAX/2)throw std::length_error("audio item too large");
  return uint32_t(n*2);
}
uint32_t gyroPayload(size_t n){
  if(n>(UINT32_MAX-8)/24)throw std::length_error("gyro item too large");
  return uint32_t(8+n*24);
}
uint32_t accelPayload(size_t n){
  if(n>(UINT32_MAX-8)/24)throw std::length_error("accelerometer item too large");
  return uint32_t(8+n*24);
}
uint32_t audioIndexPayload(size_t n){
  if(n>(UINT32_MAX-16)/16)throw std::length_error("audio index too large");
  return uint32_t(16+n*16);
}
uint32_t motionIndexPayload(size_t n){
  if(n>(UINT32_MAX-8)/16)throw std::length_error("motion index too large");
  return uint32_t(8+n*16);
}
uint32_t frameIndexPayload(size_t n){
  if(n>UINT32_MAX/16)throw std::length_error("frame index too large");
  return uint32_t(n*16);
}
// Memory backstop for coalesced motion: ~33 min of 500 Hz samples between
// two frames. Frame-cadence callers hold ~dozens, so this only trips for a
// caller streaming motion with no frames — one extra chunk beats unbounded
// growth. The payload helpers still length-check every emit.
constexpr size_t kMaxPendingMotionSamples=1024*1024;
}
ContainerWriter::ContainerWriter(const std::string&p,const std::string&m):out_(p,std::ios::binary|std::ios::trunc){
 if(!out_)throw std::runtime_error("cannot open output");const char h[8]={'M','O','T','I','O','N',' ',3};
 bytes(h,8);item(3,ck(m.size()));bytes(m.data(),m.size());
}
ContainerWriter::~ContainerWriter(){try{close();}catch(...){}}
int64_t ContainerWriter::position(){
  const auto p=out_.tellp();
  if(p==std::ofstream::pos_type(-1))throw std::runtime_error("tell failed");
  return int64_t(p);
}
void ContainerWriter::bytes(const void*p,size_t n){if(!n)return;out_.write((const char*)p,n);if(!out_)throw std::runtime_error("write failed");}
void ContainerWriter::item(uint32_t t,uint32_t n){
  uint8_t h[8];
  for(int i=0;i<4;i++){h[i]=uint8_t(t>>(8*i));h[4+i]=uint8_t(n>>(8*i));}
  bytes(h,8);
}
void ContainerWriter::writeFrame(const std::vector<uint8_t>&d,int64_t ts,const std::string&m){
 if(closed_)throw std::logic_error("closed");if(!frames_.empty()&&ts<=frames_.back().timestamp)throw std::invalid_argument("timestamp");
 // Motion coalescing (see header): at most one pending chunk per sensor
 // lands ahead of this frame, so motion chunks <= frames + 1 always.
 // After the regression check so a rejected frame emits nothing.
 flushGyro();flushAccel();
 auto p=position();item(2,ck(d.size()));bytes(d.data(),d.size());item(3,ck(m.size()));bytes(m.data(),m.size());frames_.push_back({p,ts});
}
void ContainerWriter::writeAudio(const int16_t*s,size_t n,int64_t ts){
  if(closed_)throw std::logic_error("closed");
  if(n>0&&s==nullptr)throw std::invalid_argument("null audio samples");
  const uint32_t payload=audioPayload(n);
  auto p=position();item(5,payload);
  // Chunked: n is bounded only by u32, so never buffer the whole payload.
  std::vector<uint8_t> b;b.reserve(16384);
  for(size_t i=0;i<n;){
    b.clear();
    const size_t m=std::min(n-i,size_t(8192));
    for(size_t j=0;j<m;j++){const uint16_t v=uint16_t(s[i+j]);b.push_back(uint8_t(v));b.push_back(uint8_t(v>>8));}
    bytes(b.data(),b.size());i+=m;
  }
  item(6,8);b.clear();putLe(b,ts);bytes(b.data(),b.size());audio_.push_back({p,ts});
}
void ContainerWriter::writeGyro(const GyroSample*s,size_t n){
  if(closed_)throw std::logic_error("closed");
  if(!n)return;
  if(s==nullptr)throw std::invalid_argument("null gyro samples");
  gyroPayload(n); // single-call limit, before buffering a single sample
  pendingGyro_.insert(pendingGyro_.end(),s,s+n);
  if(pendingGyro_.size()>kMaxPendingMotionSamples)flushGyro();
}
void ContainerWriter::writeAccelerometer(const AccelerometerSample*s,size_t n){
  if(closed_)throw std::logic_error("closed");
  if(!n)return;
  if(s==nullptr)throw std::invalid_argument("null accelerometer samples");
  accelPayload(n); // single-call limit, before buffering a single sample
  pendingAccel_.insert(pendingAccel_.end(),s,s+n);
  if(pendingAccel_.size()>kMaxPendingMotionSamples)flushAccel();
}
void ContainerWriter::flushGyro(){
  if(pendingGyro_.empty())return;
  const uint32_t payload=gyroPayload(pendingGyro_.size());
  auto p=position();item(9,payload);
  std::vector<uint8_t> b;b.reserve(8);putLe(b,uint32_t(1));putLe(b,ck(pendingGyro_.size()));bytes(b.data(),b.size());
  // Chunked to ~96 KiB so the backstop case (1M samples, no frames) cannot
  // spike transient memory the way one 24 MB buffer would.
  b.reserve(4096*24);
  for(size_t i=0;i<pendingGyro_.size();){
    b.clear();
    const size_t m=std::min(pendingGyro_.size()-i,size_t(4096));
    for(size_t j=0;j<m;j++){const auto&s=pendingGyro_[i+j];putLe(b,s.timestampNs);putLeF(b,s.x);putLeF(b,s.y);putLeF(b,s.z);putLe(b,uint32_t(0));}
    bytes(b.data(),b.size());i+=m;
  }
  gyro_.push_back({p,pendingGyro_[0].timestampNs});pendingGyro_.clear();
}
void ContainerWriter::flushAccel(){
  if(pendingAccel_.empty())return;
  const uint32_t payload=accelPayload(pendingAccel_.size());
  auto p=position();item(13,payload);
  std::vector<uint8_t> b;b.reserve(8);putLe(b,uint32_t(1));putLe(b,ck(pendingAccel_.size()));bytes(b.data(),b.size());
  b.reserve(4096*24);
  for(size_t i=0;i<pendingAccel_.size();){
    b.clear();
    const size_t m=std::min(pendingAccel_.size()-i,size_t(4096));
    for(size_t j=0;j<m;j++){const auto&s=pendingAccel_[i+j];putLe(b,s.timestampNs);putLeF(b,s.x);putLeF(b,s.y);putLeF(b,s.z);putLe(b,uint32_t(0));}
    bytes(b.data(),b.size());i+=m;
  }
  accel_.push_back({p,pendingAccel_[0].timestampNs});pendingAccel_.clear();
}
void ContainerWriter::close(){if(closed_)return;closed_=true;
 // Tail order is a compatibility contract: pre-gyro readers (MotionCam
 // Tools v1.0, decoder era Jan 2026) stop their tail scan at the first
 // motion item, so the audio index MUST precede any trailing motion data
 // or they report no audio. Gyro-era parsers break on accelerometer
 // items — same cure. Order-free readers don't care.
 // Index sections are buffered whole: their entry counts already live in
 // memory (frames_/audio_/gyro_/accel_), so this adds no order of magnitude.
 std::vector<uint8_t> b;
 if(!audio_.empty()){item(4,audioIndexPayload(audio_.size()));b.clear();b.reserve(16+audio_.size()*16);putLe(b,int64_t(audio_.size()));putLe(b,audio_[0].timestamp/1000000);for(auto x:audio_){putLe(b,x.offset);putLe(b,x.timestamp);}bytes(b.data(),b.size());}
 flushGyro();flushAccel(); // trailing samples AFTER the audio index (see above)
 if(!gyro_.empty()){item(8,motionIndexPayload(gyro_.size()));b.clear();b.reserve(8+gyro_.size()*16);putLe(b,uint32_t(1));putLe(b,ck(gyro_.size()));for(auto x:gyro_){putLe(b,x.offset);putLe(b,x.timestamp);}bytes(b.data(),b.size());}
 if(!accel_.empty()){item(12,motionIndexPayload(accel_.size()));b.clear();b.reserve(8+accel_.size()*16);putLe(b,uint32_t(1));putLe(b,ck(accel_.size()));for(auto x:accel_){putLe(b,x.offset);putLe(b,x.timestamp);}bytes(b.data(),b.size());}
 item(1,frameIndexPayload(frames_.size()));auto index=position();b.clear();b.reserve(frames_.size()*16);for(auto x:frames_){putLe(b,x.offset);putLe(b,x.timestamp);}bytes(b.data(),b.size());
 item(0,16);b.clear();b.reserve(16);putLe(b,uint32_t(0x8A905612));putLe(b,ck(frames_.size()));putLe(b,index);bytes(b.data(),b.size());out_.flush();if(!out_)throw std::runtime_error("finalize failed");out_.close();
}}
