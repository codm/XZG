#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <FS.h>
#include <LittleFS.h>
#include <ETH.h>
#include <HTTPClient.h>

#include <CCTools.h>

#include "config.h"
#include "czc_backhaul.h"
#include <RadioImage.h>
#include "web.h"
#include "log.h"
#include "etc.h"
#include "zb.h"
#include "mqtt.h"
#include "const/keys.h"
#include <string.h>

extern struct SysVarsStruct vars;
extern struct HwConfigStruct hwConfig;
extern HwBrdConfigStruct brdConfigs[BOARD_CFG_CNT];

extern struct SystemConfigStruct systemCfg;
extern struct NetworkConfigStruct networkCfg;
extern struct VpnConfigStruct vpnCfg;
extern struct MqttConfigStruct mqttCfg;

extern LEDControl ledControl;

extern const char *tempFile;

size_t lastSize = 0;

String tag_ZB = "[ZB]";

extern CCTools CCTool;
extern void socketClientsDisconnect();

bool zbFwCheck()
{
    BackhaulMaintenance maintenance;
    if (!maintenance) { printLogMsg("[ZB] UART busy; operation cancelled"); return false; }

    // As long as the ROUTER Version doesn't work with this check this manual override will guarantee a return true in non COORDINATOR mode
    if(systemCfg.zbRole != COORDINATOR) return true;

    const int maxAttempts = 3;
    for (int attempt = 0; attempt < maxAttempts; attempt++)
    {
        if (CCTool.checkFirmwareVersion())
        {
            printLogMsg(tag_ZB + " FW: " + String(CCTool.chip.fwRev));
            return true;
        }
        else
        {
            CCTool.restart();
            int val = attempt + 1;
            LOGD("Try: %d", val);
            delay(500 * (val * val));
        }
    }
    printLogMsg(tag_ZB + " FW: Unknown! Check serial speed!");
    return false;
}

void zbHwCheck()
{
    BackhaulMaintenance maintenance;
    if (!maintenance) { printLogMsg("[ZB] UART busy; operation cancelled"); return; }

    ledControl.modeLED.mode = LED_BLINK_1Hz;

    if (CCTool.detectChipInfo())
    {
        printLogMsg(tag_ZB + " Chip: " + CCTool.chip.hwRev);
        printLogMsg(tag_ZB + " IEEE: " + CCTool.chip.ieee);
        LOGI("modeCfg %s", String((CCTool.chip.modeCfg), HEX));
        LOGI("bslCfg %s", String((CCTool.chip.bslCfg), HEX));
        printLogMsg(tag_ZB + " Flash size: " + String(CCTool.chip.flashSize / 1024) + " KB");

        vars.hwZigbeeIs = true;
        ledControl.modeLED.mode = LED_OFF;
    }
    else
    {
        printLogMsg(tag_ZB + " No Zigbee chip!");
        vars.hwZigbeeIs = false;
        ledControl.modeLED.mode = LED_BLINK_3Hz;
    }
    CCTool.restart();
}

bool zbLedToggle()
{
    BackhaulMaintenance maintenance;
    if (!maintenance) { printLogMsg("[ZB] UART busy; operation cancelled"); return false; }

    if (CCTool.ledToggle())
    {
        if (CCTool.ledState == 1)
        {
            printLogMsg("[ZB] LED toggle ON");
            // vars.zbLedState = 1;
        }
        else
        {
            printLogMsg("[ZB] LED toggle OFF");
            // vars.zbLedState = 0;
        }
        return true;
    }
    else
    {
        printLogMsg("[ZB] LED toggle ERROR");
        return false;
    }
}

bool zigbeeErase()
{
    BackhaulMaintenance maintenance;
    if (!maintenance) { printLogMsg("[ZB] UART busy; operation cancelled"); return false; }

    if (CCTool.eraseFlash())
    {
        LOGD("magic");
        return true;
    }
    return false;
}
void nvPrgs(const String &inputMsg)
{
    String msg = inputMsg;
    if (msg.length() > 25)
    {
        msg = msg.substring(0, 25);
    }
    sendEventSafe(tagZB_NV_prgs, msg);
    LOGD("%s", msg.c_str());
}

void zbEraseNV(void *pvParameters)
{
    {
        // FreeRTOS task deletion does not unwind C++ automatic objects.
        BackhaulMaintenance maintenance;
        if (!maintenance) printLogMsg("[ZB] UART busy; operation cancelled");
        else {
            vars.zbFlashing = true;
            CCTool.nvram_reset(nvPrgs);
            logClear();
            printLogMsg("NVRAM erase finish! Restart CC2652!");
            vars.zbFlashing = false;
        }
    }
    vTaskDelete(NULL);
}

bool flashZigbeefromURL(const char *url, const char *zigbee_firmware_path, CCTools &CCTool) {
    BackhaulMaintenance maintenance;
    if (!maintenance) return false;
    bool update_successful = false;

    ledControl.modeLED.mode = LED_BLINK_3Hz;
    vars.zbFlashing = true;

    // Remove first if the previous firmware download had an error and the file was not deleted
    removeFileFromFS(zigbee_firmware_path);
    zigbee_firmware_path = downloadFirmwareFromGithub(url);
    if(zigbee_firmware_path != nullptr) {
        update_successful = eraseWriteZbFile(zigbee_firmware_path,CCTool);
        if (!removeFileFromFS(zigbee_firmware_path)) {
            printLogMsg("[ZB] Cannot remove downloaded firmware file");
        }
        if(!update_successful) {
            DEBUG_PRINTLN("[FLASH] Error while flashing -> function returned false");
        }
    }
    else {
        sendEventSafe(tagZB_FW_err, String("Failed!"));
        DEBUG_PRINTLN("[HTTP] download returned file nullptr");
    }

    ledControl.modeLED.mode = LED_OFF;
    vars.zbFlashing = false;

    DEBUG_PRINTLN(url);

    // Example URL:
    // https://raw.githubusercontent.com/xyzroe/XZG/zb_fws/ti/router/zr_genericapp_LP_CC1352P7_4_tirtos7_ticlang_20231201.bin?b=115200
    if (update_successful) {
        String version=extractVersionFromURL(String(url));
        if (version.length()) systemCfg.zigBeeFwVersion=version.toInt();
        saveSystemConfig(systemCfg);
    }

    return update_successful;
}

const char* downloadFirmwareFromGithub(const char *url) {
    sendEventSafe(tagZB_FW_info, String("startDownload"));

    HTTPClient http;
    WiFiClient plain_client;
    WiFiClientSecure secure_client;
    secure_client.setInsecure();
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    
    const char* zigbee_firmware_path = "/zigbee/firmware.bin";
    DEBUG_PRINTLN("[HTTP] begin...");
    if(String(url).startsWith("http://")) http.begin(plain_client,url);
    else http.begin(secure_client, url);

    http.addHeader("Content-Type", "application/octet-stream");
    http.addHeader("Connection", "close");    

    int http_response_code = http.GET();
    DEBUG_PRINT("[HTTP] GET code: ");
    DEBUG_PRINTLN(http_response_code);
    if(http_response_code == HTTP_CODE_OK) {
        int http_remaining_file_length = http.getSize();
        int http_total_file_length = http_remaining_file_length;
        DEBUG_PRINT("[HTTP] GET file length: ");
        DEBUG_PRINTLN(http_remaining_file_length);

        uint8_t http_read_buffer[128] = {0};

        WiFiClient* http_read_stream = http.getStreamPtr();

        DEBUG_PRINTLN("[LITTLEFS] check filesystem, create and open Firmware file");
        if(http_total_file_length<=0 || http_total_file_length>static_cast<int>(CCTool.chip.flashSize) || !hasEnoughLittleFsSpaceLeft(http_total_file_length)) {
            DEBUG_PRINTLN("[LITTLEFS] ERROR: not enough space in FS, returning nullptr");
            return nullptr;
        }

        LittleFS.mkdir("/zigbee");
        LittleFS.open(zigbee_firmware_path, "w");
        DEBUG_PRINTLN("[LITTLEFS] file creation and checkout completed successfully");

        File zigbee_firmware_file = LittleFS.open(zigbee_firmware_path, FILE_APPEND);
        if(!zigbee_firmware_file) {
            DEBUG_PRINTLN("[LITTLEFS] ERROR opening firmware file");
            return nullptr;
        }

        DEBUG_PRINTLN("[HTTP] Start download...");
        float previousPercent = 0;
        uint32_t lastRead=millis();
        // download remaining file length OR continue chunked download (len = -1)
        while(http.connected() && (http_remaining_file_length > 0 || http_remaining_file_length == -1)) {
            size_t download_available_size = http_read_stream->available();

            if(download_available_size > 0) {
                int http_payload_size = http_read_stream->readBytes(
                    http_read_buffer, 
                    ((download_available_size > sizeof(http_read_buffer)) ? sizeof(http_read_buffer) : download_available_size)
                );

                if(http_payload_size<=0) { zigbee_firmware_file.close(); http.end(); return nullptr; }
                lastRead=millis();
                //write payload to littleFS
                if(zigbee_firmware_file.write(http_read_buffer, http_payload_size)!=static_cast<size_t>(http_payload_size)){
                    DEBUG_PRINTLN("[LITTLEFS] ERROR appending data to firmware file");
                    return nullptr;
                }

                if(http_remaining_file_length > 0)
                    http_remaining_file_length -= http_payload_size;
            }

            float percent = ((float)http_total_file_length - http_remaining_file_length) / http_total_file_length * 100.0f;  
            previousPercent = sendPercentageToFrontend(percent, previousPercent, tagZB_FW_DW_prgs);

            if(millis()-lastRead>10000) { zigbee_firmware_file.close(); http.end(); return nullptr; }
            delay(1); // yield to other applications
        }
        zigbee_firmware_file.close();
        File stored=LittleFS.open(zigbee_firmware_path,"r");
        bool complete=http_remaining_file_length==0 && stored && stored.size()==static_cast<size_t>(http_total_file_length);
        stored.close();
        if(!complete) { http.end(); return nullptr; }
        DEBUG_PRINTLN("[HTTP] Finished download");
    }
    else {
        DEBUG_PRINT("[HTTP] GET failed, error: ");
        DEBUG_PRINTLN(http.errorToString(http_response_code).c_str());
        http.end(); return nullptr;
    }
    http.end();
    return zigbee_firmware_path;
}

bool eraseWriteZbFile(const char *filePath, CCTools &CCTool, ZbFlashReport *report)
{
    ZbFlashReport local;
    ZbFlashReport &state=report ? *report : local;
    state=ZbFlashReport();
    auto fail=[&](const char *code) {
        state.error=code; state.bslError=CCTool.lastBslError(); state.bslStatus=CCTool.lastBslStatus();
        String message=String("[ZB] ")+code+" stage="+state.stage+" written="+String(state.writtenBytes)+"/"+String(state.imageBytes)+" bsl="+state.bslError+" status="+String(state.bslStatus);
        printLogMsg(message);
        sendEventSafe(tagZB_FW_err,message);
        return false;
    };
    BackhaulMaintenance maintenance;
    if (!maintenance) return fail("radio_busy");

    sendEventSafe(tagZB_FW_info, String("startFlash"));
    
    File file = LittleFS.open(filePath, "r");
    if (!file)
    {
        return fail("image_open_failed");
    }

    uint8_t header[8];
    state.imageBytes=file.size();
    size_t headerBytes=file.read(header,sizeof(header));
    if(!CCTool.chip.flashSize) { file.close(); return fail("radio_flash_unknown"); }
    if(!czc::radioImageHeader(header,headerBytes,file.size(),CCTool.chip.flashSize) || !file.seek(0)) {
        file.close(); return fail("radio_image_invalid");
    }
    // Pair sessions were closed by maintenance; also close ordinary TCP clients.
    socketClientsDisconnect();
    state.stage="erase"; state.eraseStarted=true;
    if (!CCTool.eraseFlash()) { file.close(); return fail("radio_erase_failed"); }
    printLogMsg("Erase completed!");
    DEBUG_PRINTLN("[FLASH] Erase completed!");

    int totalSize = file.size();
    state.stage="begin_write";
    if (!CCTool.beginFlash(BEGIN_ZB_ADDR, totalSize))
    {
        file.close();
        return fail("radio_begin_failed");
    }

    byte buffer[CCTool.TRANSFER_SIZE];
    int loadedSize = 0;
    uint32_t crc=0xffffffffu;

    DEBUG_PRINTLN("[FLASH] Begin flash process...");
    state.stage="write";
    float previousPercent = 0;
    while (file.available() && loadedSize < totalSize)
    {
        size_t size = file.available();
        int c = file.readBytes(reinterpret_cast<char *>(buffer), std::min(size, sizeof(buffer)));
        if (c <= 0) { file.close(); return fail("image_read_failed"); }
        if (!CCTool.processFlash(buffer, c)) { file.close(); return fail("radio_write_failed"); }
        crc=czc::radioCrcUpdate(crc,buffer,c);
        loadedSize += c;
        state.writtenBytes=loadedSize;
        float percent = static_cast<float>(loadedSize) / totalSize * 100.0f;
        previousPercent = sendPercentageToFrontend(percent, previousPercent, tagZB_FW_prgs);
        delay(1); // Yield to allow other processes
    }
    
    DEBUG_PRINTLN("[FLASH] Completed!");
    file.close();
    if(loadedSize!=totalSize) return fail("image_truncated");

    state.stage="verify";
    sendEventSafe(tagZB_FW_info, String("verifyFlash"));
    if(!CCTool.verifyFlash(BEGIN_ZB_ADDR,totalSize,crc^0xffffffffu)) return fail("radio_verify_failed");
    sendEventSafe(tagZB_FW_info, String("finishFlash"));

    CCTool.restart();
    backhaulBslHold(false);
    state.stage="complete";
    return true;
}

float sendPercentageToFrontend(float percent, float previousPercent, const char* eventType) {
    if ((percent - previousPercent) > 1 || percent < 0.1 || percent == 100) {
        sendEventSafe(eventType, String(percent));  
        DEBUG_PRINT("[DOWNLOAD/FLASH] in progress: ");
        DEBUG_PRINTLN(percent);  
        return percent;
    }
    return previousPercent;
}

bool hasEnoughLittleFsSpaceLeft(size_t firmwareSize) {
    size_t remainingBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    return remainingBytes > firmwareSize;
}

bool removeFileFromFS(const char *filePath) {
    DEBUG_PRINTLN("[LITTLEFS] removing file");
    return LittleFS.remove(filePath);
}
