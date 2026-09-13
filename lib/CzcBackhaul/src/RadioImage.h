#pragma once
#include <stdint.h>
#include <stddef.h>
namespace czc {
// IEEE CRC-32 accumulator, initial 0xFFFFFFFF and final XOR 0xFFFFFFFF.
inline uint32_t radioCrcUpdate(uint32_t crc,const uint8_t *data,size_t size) {
    for(size_t i=0;i<size;i++) {
        crc^=data[i];
        for(unsigned bit=0;bit<8;bit++) crc=(crc>>1)^(0xEDB88320u & (0u-(crc&1u)));
    }
    return crc;
}

inline uint32_t imageWord(const uint8_t *p) { return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24); }
inline bool radioImageHeader(const uint8_t *p,size_t bytes,size_t size,uint32_t flashSize) {
    if(bytes<8 || size<4096 || !flashSize || size>flashSize || (size&3)) return false;
    uint32_t stack=imageWord(p),entry=imageWord(p+4);
    return stack>=0x20000000 && stack<=0x20080000 && !(stack&3) && (entry&1) && (entry&~1U)<size;
}
}
