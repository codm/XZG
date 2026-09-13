#include <Arduino.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <CCTools.h>
#include "config.h"
#include "web.h"
#include "zb.h"
#include "czc_backhaul.h"
#include <RadioImage.h>

extern CCTools CCTool;
extern SysVarsStruct vars;
static WebServer *uploadServer;
static File imageFile;
static const char *imagePath="/zigbee/local-upload.bin";
static const char *uploadError=nullptr;
static bool received=false,started=false;
static uint32_t expectedBytes=0,uploadedBytes=0,storedBytes=0;
static void uploadFailure(const char *code) {
    if(uploadError) return;
    uploadError=code;
    printLogMsg(String("[ZB upload] ")+code+" received="+String(uploadedBytes)+" expected="+String(expectedBytes)+" stored="+String(storedBytes));
}
static void upload() {
    HTTPUpload &part=uploadServer->upload();
    if(part.status==UPLOAD_FILE_START) {
        if(!backhaulAuthorizeWebMutation()) { uploadFailure("authentication_required"); return; }
        if(started) { uploadFailure("one_file_required"); return; }
        started=true; received=false; uploadError=nullptr;
        expectedBytes=uploadedBytes=storedBytes=0;
        if(!part.filename.endsWith(".bin")) { uploadFailure("radio_bin_required"); return; }
        if(vars.zbFlashing) { uploadFailure("radio_busy"); return; }
        if(!CCTool.chip.flashSize) { uploadFailure("radio_flash_unknown"); return; }
        LittleFS.mkdir("/zigbee"); LittleFS.remove(imagePath);
        imageFile=LittleFS.open(imagePath,"w");
        if(!imageFile) uploadFailure("storage_error");
    } else if(part.status==UPLOAD_FILE_WRITE && !uploadError && started) {
        if(part.currentSize>CCTool.chip.flashSize-uploadedBytes) { uploadFailure("radio_image_too_large"); return; }
        if(!imageFile) { uploadFailure("storage_error"); return; }
        size_t written=imageFile.write(part.buf,part.currentSize);
        uploadedBytes+=written;
        if(written!=part.currentSize) uploadFailure("storage_write_failed");
    } else if(part.status==UPLOAD_FILE_END && started) {
        expectedBytes=part.totalSize;
        if(uploadError) { imageFile.close(); return; }
        // Arduino VFS File.size() uses stat(path), which may lag buffered writes.
        // Closing commits the stream; reopening also discards the cached stat.
        imageFile.close();
        File stored=LittleFS.open(imagePath,"r");
        storedBytes=stored ? stored.size() : 0;
        received=stored && uploadedBytes==expectedBytes && storedBytes==expectedBytes;
        stored.close();
        if(!received) uploadFailure("incomplete_upload");
    } else if(part.status==UPLOAD_FILE_ABORTED) {
        imageFile.close(); LittleFS.remove(imagePath); uploadFailure("upload_aborted"); started=false; received=false;
    }
}
static void finish() {
    if(!backhaulAuthorizeWebMutation()) { imageFile.close(); LittleFS.remove(imagePath); started=received=false; return; }
    imageFile.close();
    bool ok=false;
    ZbFlashReport flash;
    if(!uploadError && received) {
        BackhaulMaintenance maintenance;
        if(!maintenance) uploadFailure("radio_busy");
        else {
            vars.zbFlashing=true;
            ok=eraseWriteZbFile(imagePath,CCTool,&flash);
            const String role=uploadServer->arg("fwMode");
            if(ok && (role=="coordinator" || role=="router" || role=="thread")) changeZbMode(role);
            // Release the broker even after failure; UI remains usable for retry.
            if(!ok && flash.eraseStarted) { CCTool.restart(); backhaulBslHold(false); }
            vars.zbFlashing=false;
        }
    }
    LittleFS.remove(imagePath);
    uploadServer->sendHeader("Cache-Control","no-store");
    StaticJsonDocument<512> response;
    response["result"]=ok ? "radio_updated" : uploadError ? uploadError : flash.error ? flash.error : "missing_file";
    if(!ok) {
        response["stage"]=uploadError || !received ? "upload" : flash.stage;
        response["expected_bytes"]=expectedBytes; response["received_bytes"]=uploadedBytes; response["stored_bytes"]=storedBytes;
        response["written_bytes"]=flash.writtenBytes; response["radio_flash_bytes"]=CCTool.chip.flashSize;
        response["erase_started"]=flash.eraseStarted;
        if(flash.error) { response["bsl_error"]=flash.bslError; response["bsl_status"]=flash.bslStatus; }
    }
    String body; serializeJson(response,body);
    if(!ok) printLogMsg(String("[ZB upload] ")+body);
    uploadServer->send(ok ? 200 : 400,"application/json",body);
    started=received=false; uploadError=nullptr;
    expectedBytes=uploadedBytes=storedBytes=0;
}
void registerRadioUpload(WebServer &server) {
    uploadServer=&server;
    server.on("/updateZB",HTTP_POST,finish,upload);
}
