#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <utility>
#include <algorithm>
#include <cctype>
using byte=uint8_t;
#define HEX 16
#define OUTPUT 1
#define HIGH 1
#define LOW 0
struct String:std::string {
 String(const char *s):std::string(s){}
 using std::string::operator=;
 String()=default;
 String(const std::string &s):std::string(s){}
 String(unsigned long n,unsigned base=10){char b[32];snprintf(b,sizeof(b),base==16?"%lx":"%lu",n);assign(b);}
 int indexOf(const char *s)const {auto p=find(s);return p==npos?-1:int(p);}
 void toUpperCase(){for(auto &c:*this)c=std::toupper(c);}
};
unsigned long millis();
void delay(unsigned long);
inline void pinMode(int,int){}
inline void digitalWrite(int,int){}
class Stream {
public:
 virtual ~Stream()=default;
 virtual int available()=0;
 virtual int read()=0;
 virtual size_t write(uint8_t)=0;
 virtual void flush(){}
 size_t write(const uint8_t *p,size_t n){for(size_t i=0;i<n;i++)if(write(p[i])!=1)return i;return n;}
 size_t readBytes(uint8_t *p,size_t n){unsigned long start=millis();size_t i=0;while(i<n && millis()-start<1000){if(available()){int b=read();if(b>=0)p[i++]=b;}else delay(1);}return i;}
};
