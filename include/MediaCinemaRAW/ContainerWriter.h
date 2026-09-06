#pragma once
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
namespace mediacinemaraw {
struct GyroSample { int64_t timestampNs; float x,y,z; };
class ContainerWriter {
public:
 ContainerWriter(const std::string& path,const std::string& metadata); ~ContainerWriter();
 ContainerWriter(const ContainerWriter&)=delete; ContainerWriter& operator=(const ContainerWriter&)=delete;
 void writeFrame(const std::vector<uint8_t>&,int64_t,const std::string&);
 void writeAudio(const int16_t*,size_t,int64_t); void writeGyro(const GyroSample*,size_t);
 void close(); size_t frameCount() const noexcept{return frames_.size();}
private:
 struct Offset{int64_t offset,timestamp;}; std::ofstream out_;
 std::vector<Offset> frames_,audio_,gyro_; bool closed_=false;
 void item(uint32_t,uint32_t); void bytes(const void*,size_t); int64_t position();
};
}
