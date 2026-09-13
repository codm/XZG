#pragma once
#include <stdint.h>

struct ZbFlashReport {
    const char *error=nullptr;
    const char *stage="validate";
    const char *bslError="none";
    uint8_t bslStatus=0;
    uint32_t imageBytes=0, writtenBytes=0;
    bool eraseStarted=false;
};

bool zbFwCheck();
void zbHwCheck();
bool zbLedToggle();
bool zigbeeErase();
void nvPrgs(const String &inputMsg);
void zbEraseNV(void *pvParameters);

bool flashZigbeefromURL(const char *url, const char *filePath, CCTools &CCTool);
const char* downloadFirmwareFromGithub(const char *url);
bool eraseWriteZbFile(const char *filePath, CCTools &CCTool, ZbFlashReport *report=nullptr);
float sendPercentageToFrontend(float percent, float previousPercent, const char* eventType);
bool hasEnoughLittleFsSpaceLeft(size_t firmwareSize);
bool removeFileFromFS(const char *filePath);
