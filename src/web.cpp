#include <Arduino.h>
#include <ArduinoJson.h>
#include <ETH.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <Update.h>
#include <WebServer.h>
#include <FS.h>
#include <WiFi.h>
#include <Ticker.h>
#include <CCTools.h>
#include <lwip/sockets.h>
#include <atomic>

#include "config.h"
#include "czc_backhaul.h"
#include "network_status.h"
#include "memory_status.h"
#include "web.h"
#include "web_refresh.h"
#include "log.h"
#include "etc.h"
#include "zb.h"
#include "const/zones.h"
#include "const/keys.h"
// #include "const/hw.h"

#include "webh/html/PAGE_MQTT.html.gz.h"
#include "webh/html/PAGE_GENERAL.html.gz.h"
#include "webh/html/PAGE_LOADER.html.gz.h"
#include "webh/html/PAGE_ROOT.html.gz.h"
#include "webh/html/PAGE_ZIGBEE.html.gz.h"
#include "webh/html/PAGE_TOOLS.html.gz.h"
#include "webh/html/PAGE_NETWORK.html.gz.h"
#include "webh/html/PAGE_LOGIN.html.gz.h"

#include "webh/js/i18next.min.js.gz.h"
#include "webh/js/i18nextHttpBackend.min.js.gz.h"
#include "webh/js/functions.js.gz.h"
#include "webh/js/bootstrap.bundle.min.js.gz.h"
#include "webh/js/jquery-min.js.gz.h"
#include "webh/js/masonry.js.gz.h"

#include "webh/css/style.css.gz.h"
#include "webh/img/icons.svg.gz.h"
#include "webh/img/logo.svg.gz.h"
#include "webh/img/logo-dark.svg.gz.h"

#include "webh/json/en.json.gz.h"
#include "webh/json/uk.json.gz.h"
#include "webh/json/zh.json.gz.h"
#include "webh/json/es.json.gz.h"
#include "webh/json/pt.json.gz.h"
#include "webh/json/ru.json.gz.h"
#include "webh/json/fr.json.gz.h"
#include "webh/json/de.json.gz.h"
#include "webh/json/ja.json.gz.h"
#include "webh/json/tr.json.gz.h"
#include "webh/json/it.json.gz.h"
#include "webh/json/pl.json.gz.h"
#include "webh/json/cz.json.gz.h"

// #define HTTP_DOWNLOAD_UNIT_SIZE 3000

// #define HTTP_UPLOAD_BUFLEN 3000

// #define HTTP_MAX_DATA_WAIT 10000 // ms to wait for the client to send the request
// #define HTTP_MAX_POST_WAIT 10000 // ms to wait for POST data to arrive
// #define HTTP_MAX_SEND_WAIT 10000 // ms to wait for data chunk to be ACKed
// #define HTTP_MAX_CLOSE_WAIT 4000 // ms to wait for the client to close the connection

extern CCTools CCTool;

extern struct SysVarsStruct       vars;
extern struct HwConfigStruct    hwConfig;
extern        HwBrdConfigStruct     brdConfigs[BOARD_CFG_CNT];
extern struct SystemConfigStruct  systemCfg;
extern struct NetworkConfigStruct networkCfg;
extern struct VpnConfigStruct     vpnCfg;
extern struct MqttConfigStruct    mqttCfg;

extern LEDControl ledControl;

extern IPAddress apIP;

bool wifiWebSetupInProgress = false;

bool eventOK = false;

SemaphoreHandle_t sendEventMutex;
static std::atomic<uint32_t> eventDrops{0};
static const char *espUpdateState="idle",*espUpdateError="";
static uint32_t espUpdateRestartAt=0;
static bool espUploadHeld=false,espUploadSeen=false,espUploadReady=false,espUploadStarted=false;
static size_t espUploadExpected=0,espUploadReceived=0;
static void failEspUpdate(const char *reason) {
    Update.abort(); espUpdateState="failed"; espUpdateError=reason;
    sendEventSafe("esp.fi","failed");
}
static void completeEspUpdate() {
    espUpdateState="restarting"; espUpdateError="";
    sendEventSafe(tagESP_FW_prgs,"100"); sendEventSafe("esp.fi","restarting");
    espUpdateRestartAt=millis()+2000;
}
void espUpdateLoop() {
    // Give the HTTP result time to reach the browser; never reboot inside the
    // multipart callback, before WebServer can send its completion response.
    if(espUpdateRestartAt && static_cast<int32_t>(millis()-espUpdateRestartAt)>=0) {
        espUpdateRestartAt=0; ESP.restart();
    }
}

// API strings
const char *argAction          = "action";
const char *argPage            = "page";
const char *argParam           = "param";
const char *argFilename        = "filename";
const char *argCmd             = "cmd";
const char *argUrl             = "url";
const char *argConf            = "conf";
const char *argLed             = "led";
const char *argAct             = "act";
const char *apiWrongArgs       = "wrong args";
const char *apiOk              = "ok";
const char *errLink            = "Error getting link";
const char *argFwMode          = "fwMode";

// MIME types
const char *contTypeTextHtml   = "text/html";
const char *contTypeTextJs     = "text/javascript";
const char *contTypeTextCss    = "text/css";
const char *contTypeTextSvg    = "image/svg+xml";
const char *contTypeJson       = "application/json";
const char *contTypeText       = "text/plain";

// Misc. strings
const char *checked            = "true";
const char *respHeaderName     = "respValuesArr";
const char *respTimeZonesName  = "respTimeZones";
const char *tempFile           = "/fw.hex";

bool opened = false;
File fwFile;

extern bool loadFileConfigMqtt();
extern bool loadFileConfigWg();

WebServer serverWeb(80);

WiFiClient eventsClient;

// apiHandler() handler functions,
// listed in index order
static void apiDefault          ();
static void apiGetPage          ();
static void apiGetParam         ();
static void apiStartWifiScan    ();
static void apiWifiScanStatus   ();
static void apiGetLog           ();
static void apiCmd              ();
static void apiWifiConnnectStat ();
static void apiGetFile          ();
static void apiDelFile          ();
static void apiGetFileList      ();

// apiCMD() handler functions,
// listed in index order
static void apiCmdDefault         (String &result);
static void apiCmdZbRouterRecon   (String &result);
static void apiCmdZbRestart       (String &result);
static void apiCmdZbEnableBsl     (String &result);
static void apiCmdEspReset        (String &result);
static void apiCmdAdapterLan      (String &result);
static void apiCmdAdapterUsb      (String &result);
static void apiCmdLedAct          (String &result);
static void apiCmdZbFlash         (String &result);
static void apiCmdClearLog        (String &result);
static void apiCmdUpdateUrl       (String &result);
static void apiCmdZbCheckFirmware (String &result);
static void apiCmdZbLedToggle     (String &result);
static void apiCmdFactoryReset    (String &result);
static void apiCmdDnsCheck        (String &result);
static void apiCmdClientCheck (String &result);

// functions called exactly once each
// from getRootData():
static inline void getRootEthTab       (DynamicJsonDocument &doc,
                                        bool update,
                                        const String &noConn);
static inline void getRootWifi         (DynamicJsonDocument &doc,
                                        bool update,
                                        const String &noConn);
static inline void getRootHwMisc       (DynamicJsonDocument &doc,
                                        bool update);
static inline void getRootVpnWireGuard (DynamicJsonDocument &doc);
static inline void getRootVpnHusarnet  (DynamicJsonDocument &doc);
static inline void getRootUptime       (DynamicJsonDocument &doc);
static inline void getRootCpuTemp      (DynamicJsonDocument &doc);
static inline void getRootOneWireTemp  (DynamicJsonDocument &doc);
static inline void getRootNvsStats     (DynamicJsonDocument &doc);
static inline void getRootSockets      (DynamicJsonDocument &doc);
static inline void getRootTime         (DynamicJsonDocument &doc);

void webServerHandleClient()
{
    serverWeb.handleClient();
}

void redirectLogin(int msg = 0)
{
    String path = "/login";
    if (msg != 0)
    {
        path = path + "?msg=" + msg;
    }

    serverWeb.sendHeader("Location", path);
    serverWeb.sendHeader("Cache-Control", "no-cache");
    serverWeb.send(301);
}

void handleLoader()
{
    if (captivePortal())
    {
        return;
    }
    if (!is_authenticated())
    {
        redirectLogin();
        return;
    }
    sendGzip(contTypeTextHtml, PAGE_LOADER_html_gz, PAGE_LOADER_html_gz_len);
}

void registerRadioUpload(WebServer &server);
void initWebServer()
{
    // Network/AP recovery can call startServers() again. Keep the existing
    // route list and mutex instead of leaking handlers or replacing a live lock.
    static bool initialized=false;
    if(initialized) return;
    /* ----- LANG FILES | START -----*/
    serverWeb.on("/lg/en.json", []()
                 { sendGzip(contTypeJson, en_json_gz, en_json_gz_len); });
    serverWeb.on("/lg/uk.json", []()
                 { sendGzip(contTypeJson, uk_json_gz, uk_json_gz_len); });
    serverWeb.on("/lg/zh.json", []()
                 { sendGzip(contTypeJson, zh_json_gz, zh_json_gz_len); });
    serverWeb.on("/lg/es.json", []()
                 { sendGzip(contTypeJson, es_json_gz, es_json_gz_len); });
    serverWeb.on("/lg/pt.json", []()
                 { sendGzip(contTypeJson, pt_json_gz, pt_json_gz_len); });
    serverWeb.on("/lg/ru.json", []()
                 { sendGzip(contTypeJson, ru_json_gz, ru_json_gz_len); });
    serverWeb.on("/lg/fr.json", []()
                 { sendGzip(contTypeJson, fr_json_gz, fr_json_gz_len); });
    serverWeb.on("/lg/de.json", []()
                 { sendGzip(contTypeJson, de_json_gz, de_json_gz_len); });
    serverWeb.on("/lg/ja.json", []()
                 { sendGzip(contTypeJson, ja_json_gz, ja_json_gz_len); });
    serverWeb.on("/lg/tr.json", []()
                 { sendGzip(contTypeJson, tr_json_gz, tr_json_gz_len); });
    serverWeb.on("/lg/it.json", []()
                 { sendGzip(contTypeJson, it_json_gz, it_json_gz_len); });
    serverWeb.on("/lg/pl.json", []()
                 { sendGzip(contTypeJson, pl_json_gz, pl_json_gz_len); });
    serverWeb.on("/lg/cz.json", []()
                 { sendGzip(contTypeJson, cz_json_gz, cz_json_gz_len); });
    /* ----- LANG FILES | END -----*/
    /* ----- JS and CSS FILES | START -----*/
    serverWeb.on("/js/i18next.min.js", []()
                 { sendGzip(contTypeTextJs, i18next_min_js_gz, i18next_min_js_gz_len); });
    serverWeb.on("/js/i18nextHttpBackend.min.js", []()
                 { sendGzip(contTypeTextJs, i18nextHttpBackend_min_js_gz, i18nextHttpBackend_min_js_gz_len); });
    serverWeb.on("/js/bootstrap.bundle.min.js", []()
                 { sendGzip(contTypeTextJs, bootstrap_bundle_min_js_gz, bootstrap_bundle_min_js_gz_len); });
    serverWeb.on("/js/masonry.js", []()
                 { sendGzip(contTypeTextJs, masonry_js_gz, masonry_js_gz_len); });
    serverWeb.on("/js/functions.js", []()
                 { sendGzip(contTypeTextJs, functions_js_gz, functions_js_gz_len); });
    serverWeb.on("/js/jquery-min.js", []()
                 { sendGzip(contTypeTextJs, jquery_min_js_gz, jquery_min_js_gz_len); });
    serverWeb.on("/css/style.css", []()
                 { sendGzip(contTypeTextCss, required_css_gz, required_css_gz_len); });
    /* ----- JS and CSS FILES  | END -----*/
    /* ----- SVG FILES | START -----*/
    serverWeb.on("/logo.svg", []()
                 { sendGzip(contTypeTextSvg, logo_svg_gz, logo_svg_gz_len); });
    serverWeb.on("/logo-dark.svg", []()
                 { sendGzip(contTypeTextSvg, logo_dark_svg_gz, logo_dark_svg_gz_len); });
    serverWeb.on("/icons.svg", []()
                 { sendGzip(contTypeTextSvg, icons_svg_gz, icons_svg_gz_len); });

    serverWeb.onNotFound(handleNotFound);

    /* ----- SVG FILES | END -----*/
    /* ----- PAGES | START -----*/
    serverWeb.on("/", handleLoader);
    serverWeb.on("/generate_204", handleLoader);
    serverWeb.on("/general", handleLoader);
    serverWeb.on("/network", handleLoader);
    serverWeb.on("/ethernet", handleLoader);
    serverWeb.on("/zigbee", handleLoader);
    serverWeb.on("/tools", handleLoader);
    serverWeb.on("/mqtt", handleLoader);
    serverWeb.on("/login", []()
                 { if (serverWeb.method() == HTTP_GET) {
                        handleLoginGet();
                    } else if (serverWeb.method() == HTTP_POST) {
                        handleLoginPost();
                    } });
    serverWeb.on("/logout", HTTP_GET, handleLogout);
    /* ----- PAGES | END -----*/
    /* ----- MIST CMDs | END -----*/
    serverWeb.on("/saveParams", HTTP_POST, handleSaveParams);
    serverWeb.on("/cmdZigRST", handleZigbeeRestart);
    serverWeb.on("/cmdZigBSL", handleZigbeeBSL);
    serverWeb.on("/saveFile", handleSavefile);

    serverWeb.on("/api", handleApi);
    serverWeb.on("/api/network/status", HTTP_GET, []() {
        if(!is_authenticated()) { serverWeb.send(401); return; }
        serverWeb.sendHeader("Cache-Control", "no-store");
        DynamicJsonDocument doc(2048);
        networkStatus(doc.to<JsonObject>());
        String body; serializeJson(doc,body);
        serverWeb.send(200,"application/json",body);
    });

    serverWeb.on("/events", handleEvents);
    /* ----- MIST CMDs | END -----*/

    /* ----- OTA | START -----*/

    serverWeb.on("/update", HTTP_POST, handleUpdateRequest, handleEspUpdateUpload);
    serverWeb.on("/api/esp-update",HTTP_GET,[]() {
        if(!is_authenticated()) { serverWeb.send(401); return; }
        static const String boot=String(esp_random(),HEX)+"-"+String(esp_random(),HEX);
        serverWeb.sendHeader("Cache-Control","no-store");
        StaticJsonDocument<512> doc; doc["boot"]=boot; doc["version"]=VERSION;
        doc["state"]=espUpdateState; doc["error"]=espUpdateError;
        memoryStatus(doc.createNestedObject("heap"));
        doc["wifi_connected"]=WiFi.status()==WL_CONNECTED;
        if(WiFi.status()==WL_CONNECTED) doc["wifi_rssi"]=WiFi.RSSI();
        doc["ui_refresh_s"]=webRefreshSeconds(systemCfg.refreshLogs);
        doc["ui_events_dropped"]=eventDrops.load();
        String body; serializeJson(doc,body); serverWeb.send(200,contTypeJson,body);
    });
    // serverWeb.on("/updateZB", HTTP_POST, handleUpdateRequest, handleZbUpdateUpload);

    /* ----- OTA | END -----*/

    backhaulWebRoutes(serverWeb);
    registerRadioUpload(serverWeb);
    const char       *headerkeys[]   = {"Content-Length", "Cookie", "X-Backhaul-Token"};
    constexpr size_t  headerkeyssize = sizeof(headerkeys) / sizeof(char *);
    sendEventMutex = xSemaphoreCreateMutex();
    if(sendEventMutex == NULL) {
        DEBUG_PRINTLN("Error while creating sendEventMutex!");
    }
    serverWeb.collectHeaders(headerkeys, headerkeyssize);
    serverWeb.begin();
    initialized=true;
    LOGD("done");
}

// IPAddress apIP2(192, 168, 1, 1);

bool captivePortal()
{
    if (vars.apStarted)
    {
        IPAddress ip;
        if (!ip.fromString(serverWeb.hostHeader()))
        {
            LOGD("Request redirected to captive portal");
            serverWeb.sendHeader("Location", String("http://") + apIP.toString(), true);
            serverWeb.send(302, "text/plain", "");
            serverWeb.client().stop();
            return true;
        }
    }
    return false;
}

/*
void handleNotFound()
{
    if (vars.apStarted)
    {
        sendGzip(contTypeTextHtml, PAGE_LOADER_html_gz, PAGE_LOADER_html_gz_len);
    }
    else
    {
        serverWeb.send(HTTP_CODE_OK, contTypeText, F("URL NOT FOUND"));
    }
}
*/

void handleNotFound()
{
    if (captivePortal())
    {
        return;
    }
    String message = F("File Not Found\n\n");
    message += F("URI: ");
    message += serverWeb.uri();
    message += F("\nMethod: ");
    message += (serverWeb.method() == HTTP_GET) ? "GET" : "POST";
    message += F("\nArguments: ");
    message += serverWeb.args();
    message += F("\n");

    for (uint8_t i = 0; i < serverWeb.args(); i++)
    {
        message += String(F(" "))
                   + serverWeb.argName(i)
                   + F(": ")
                   + serverWeb.arg(i)
                   + F("\n");
    }
    serverWeb.sendHeader ("Cache-Control", "no-cache, no-store, must-revalidate");
    serverWeb.sendHeader ("Pragma", "no-cache");
    serverWeb.sendHeader ("Expires", "-1");
    serverWeb.send       (404, "text/plain", message);
}

void handleUpdateRequest()
{
    if(!is_authenticated()) { serverWeb.send(401); return; }
    // Validate only after the complete multipart request, not its first file.
    bool complete=espUploadStarted && espUploadReady && espUploadReceived &&
        (!espUploadExpected || espUploadReceived==espUploadExpected) && Update.end(!espUploadExpected);
    if(!complete && espUploadSeen && !*espUpdateError) failEspUpdate("ota_incomplete_or_invalid");
    serverWeb.sendHeader("Connection","close");
    StaticJsonDocument<192> doc;
    doc["result"]=complete ? "esp_updated" : *espUpdateError ? espUpdateError : "missing_file";
    String body; serializeJson(doc,body);
    serverWeb.send(complete ? 200 : 400,contTypeJson,body);
    if(complete) completeEspUpdate();
    else if(espUploadHeld) { backhaulMaintenanceEnd(); espUploadHeld=false; }
    espUploadSeen=espUploadReady=espUploadStarted=false;
}

void handleEspUpdateUpload()
{
    if(!is_authenticated()) return;
    HTTPUpload &upload=serverWeb.upload();
    if(upload.status==UPLOAD_FILE_START)
    {
        if(espUpdateRestartAt) { espUpdateError="restart_pending"; return; }
        if(espUploadSeen) {
            failEspUpdate("unexpected_file"); espUploadStarted=false; return;
        }
        espUploadSeen=true; espUploadReady=false; espUploadReceived=0;
        espUpdateState="uploading"; espUpdateError="";
        espUploadExpected=0;
        if(serverWeb.hasArg("size")) {
            String size=serverWeb.arg("size");
            bool valid=size.length()>0 && size.length()<=7;
            for(size_t i=0;i<size.length();++i) valid=valid && size[i]>='0' && size[i]<='9';
            if(!valid || size.toInt()<=0) { failEspUpdate("invalid_size"); return; }
            espUploadExpected=size.toInt();
        }
        espUploadHeld=backhaulMaintenanceBegin();
        if(!espUploadHeld) { failEspUpdate("radio_busy"); return; }
        Update.onProgress(progressFunc);
        espUploadStarted=Update.begin(espUploadExpected ? espUploadExpected : UPDATE_SIZE_UNKNOWN);
        if(!espUploadStarted) failEspUpdate("ota_begin_failed");
    }
    else if(upload.status==UPLOAD_FILE_WRITE && espUploadStarted)
    {
        if(Update.write(upload.buf,upload.currentSize)!=upload.currentSize) {
            failEspUpdate("ota_write_failed"); espUploadStarted=false;
        }
        espUploadReceived+=upload.currentSize;
    }
    else if(upload.status==UPLOAD_FILE_END) espUploadReady=espUploadStarted;
    else if(upload.status==UPLOAD_FILE_ABORTED) {
        failEspUpdate("upload_aborted");
        if(espUploadHeld) backhaulMaintenanceEnd();
        espUploadHeld=espUploadSeen=espUploadReady=espUploadStarted=false;
    }
}

// SSE transport: this also runs with backhaul Off. Arduino WiFiClient::write
// retries select() for seconds; it must never run in an OTA callback or while
// holding the lock shared with the HTTP task. SSE is recoverable telemetry:
// close a congested/partial stream and let EventSource reconnect for a snapshot.
static void dropEvents() {
    ++eventDrops;
    eventsClient.stop();
}

static void writeEvents(const char *data, size_t length) {
    if(eventsClient.fd()<0) return;
    if(::send(eventsClient.fd(),data,length,MSG_DONTWAIT)!=static_cast<ssize_t>(length))
        dropEvents();
}

static bool eventsConnected() {
    if(!sendEventMutex || xSemaphoreTake(sendEventMutex,0)!=pdTRUE) return false;
    bool connected=eventsClient.connected();
    xSemaphoreGive(sendEventMutex);
    return connected;
}

void handleEvents() {
    if(!is_authenticated()) { serverWeb.send(401); return; }
    // No wait on a worker and no unsynchronised reassignment of its WiFiClient.
    if(!sendEventMutex || xSemaphoreTake(sendEventMutex,0)!=pdTRUE) {
        serverWeb.client().stop();
        return;
    }
    eventsClient.stop();
    eventsClient=serverWeb.client();
    eventsClient.setNoDelay(true);
    static const char headers[]="HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Connection: close\r\nCache-Control: no-cache\r\n\r\nretry: 1500\n\n";
    writeEvents(headers,sizeof(headers)-1);
    xSemaphoreGive(sendEventMutex);
}

static void sendEventRecord(const char *event, const String &data, bool rootFinish) {
    if(!sendEventMutex || xSemaphoreTake(sendEventMutex,0)!=pdTRUE) return;
    if(eventsClient.fd()>=0) {
        String record;
        size_t expected=strlen(event)+data.length()+16;
        static const char finish[]="event: root_update\ndata: finish\n\n";
        if(rootFinish) expected+=sizeof(finish)-1;
        if(!record.reserve(expected)) dropEvents();
        else {
            record+="event: "; record+=event; record+="\ndata: "; record+=data; record+="\n\n";
            if(rootFinish) record+=finish;
            if(record.length()!=expected) dropEvents();
            else writeEvents(record.c_str(),record.length());
        }
    }
    xSemaphoreGive(sendEventMutex);
}

void sendEventSafe(const char *event, const String data) {
    sendEventRecord(event,data,false);
}

void sendGzip(const char *contentType,
              const uint8_t content[],
              uint16_t contentLen)
{
    serverWeb.sendHeader (F("Content-Encoding"),
                          F("gzip"));
    serverWeb.send_P     (HTTP_CODE_OK,
                          contentType,
                          (const char *)content,
                          contentLen);
}

// This isn't called from anywhere,
// does it even work?
void hex2bin(uint8_t *out, const char *in)
{
    // uint8_t sz = 0;
    while (*in)
    {
        while (*in == ' ')
            in++; // skip spaces
        if (!*in)
            break;
        uint8_t c = *in >= 'a' ? *in - 'a' + 10 : *in >= 'A' ? *in - 'A' + 10
                                                             : *in - '0';
        in++;
        c <<= 4;
        if (!*in)
            break;
        c |= *in >= 'a' ? *in - 'a' + 10 : *in >= 'A' ? *in - 'A' + 10
                                                      : *in - '0';
        in++;
        *out++ = c;
        // sz++;
    }
}

static void apiGetLog()
{
    String result;
    result = logPrint();
    serverWeb.send(HTTP_CODE_OK, contTypeText, result);
}

static void apiCmdUpdateUrl(String &result)
{
    if (serverWeb.hasArg(argUrl))
    {
        result=getEspUpdate(serverWeb.arg(argUrl)) ? "esp_updated" : espUpdateError;
    }
    else
    {
        String link = fetchLatestEspFw();
        if (link)
        {
            result=getEspUpdate(link) ? "esp_updated" : espUpdateError;
        }
        else
        {
            LOGW("%s", String(errLink));
            failEspUpdate("download_failed"); result=espUpdateError;
        }
    }
}

static void apiCmdZbCheckFirmware(String &result)
{
    if (zbFwCheck())
    {
        serverWeb.send(HTTP_CODE_OK, contTypeText, result);
    }
    else
    {
        serverWeb.send(HTTP_CODE_INTERNAL_SERVER_ERROR, contTypeText, result);
    }
}

static void apiCmdZbLedToggle(String &result)
{
    if (zbLedToggle())
    {
        serverWeb.send(HTTP_CODE_OK, contTypeText, result);
    }
    else
    {
        serverWeb.send(HTTP_CODE_INTERNAL_SERVER_ERROR, contTypeText, result);
    }
}

static void apiCmdFactoryReset(String &result)
{
    if (serverWeb.hasArg(argConf))
    {
        if (serverWeb.arg(argConf).toInt() == 1)
        {
            serverWeb.send(HTTP_CODE_OK, contTypeText, result);
            factoryReset();
        }
        else
        {
            serverWeb.send(HTTP_CODE_BAD_REQUEST, contTypeText, result);
        }
    }
}

static void apiCmdLedAct(String &result)
{
    if (serverWeb.hasArg(argLed) && serverWeb.hasArg(argAct))
    {
        int ledNum = serverWeb.arg(argLed).toInt();
        int actNum = serverWeb.arg(argAct).toInt();

        LED_t   ledEnum = static_cast<LED_t>(ledNum);
        LEDMode actEnum = static_cast<LEDMode>(actNum);

        if (static_cast<int>(ledEnum) == ledNum && static_cast<int>(actEnum) == actNum)
        {
            String tag = "API";
            serverWeb.send(HTTP_CODE_OK, contTypeText, result);
            if (ledNum == MODE_LED)
            {
                LOGD("%s led %d", ledControl.modeLED.name, actNum);
                ledControl.modeLED.mode = static_cast<LEDMode>(actNum);
            }
            else if (ledNum == POWER_LED)
            {
                LOGD("%s led %d", ledControl.powerLED.name, actNum);
                ledControl.powerLED.mode = static_cast<LEDMode>(actNum);
            }
        }
        else
        {
            serverWeb.send(HTTP_CODE_BAD_REQUEST, contTypeText, result);
        }
    }
    else
    {
        serverWeb.send(HTTP_CODE_BAD_REQUEST, contTypeText, result);
    }
}

static void apiCmdZbFlash(String &result)
{
    BackhaulMaintenance maintenance;
    if(!maintenance) { serverWeb.send(409,contTypeText,"radio_busy"); return; }
    bool updated=false;
    const char* zigbee_firmware_path = "/zigbee/firmware.bin";

    DEBUG_PRINT("[WEB] There are currently ");
    DEBUG_PRINT(vars.connectedClients);
    DEBUG_PRINTLN(" connected clients");
    if(vars.connectedClients != 0) {
        //sendEvent();
    }

    if (serverWeb.hasArg(argUrl))
    {
        updated=flashZigbeefromURL(serverWeb.arg(argUrl).c_str(), zigbee_firmware_path, CCTool);
        if(!updated) {
            DEBUG_PRINTLN("[WEB] Error while downloading and flashing Zigbee firmware");
        }
    }
    else {
        String link = fetchLatestZbFw();
        if (link)
        {
            updated=flashZigbeefromURL(link.c_str(), zigbee_firmware_path, CCTool);
            if(!updated) {
                DEBUG_PRINTLN("[WEB] Error while downloading and flashing ZigBee firmware from link");
            }
        }
        else
        {
            LOGW("%s", String(errLink));
        }
    }
    if(updated) changeZbMode(serverWeb.arg(argFwMode));
    
}

void changeZbMode(String fwMode) {
    if (fwMode == "coordinator") {
        systemCfg.zbRole = COORDINATOR;
        DEBUG_PRINTLN("[WEB] Changed ZbRole to COORDINATOR");
        if(vars.connectedClients == 0) ledControl.powerLED.mode = LED_BLINK_1Hz;

        // Only here because the other FWs dont respond to FW checks
        DEBUG_PRINTLN("[WEB] Checking ZigBee firmware on the CC");
        // The upload handler sends the final response after role persistence and cleanup.
        zbFwCheck();
    }
    else if (fwMode == "router")
    {
        systemCfg.zbRole = ROUTER;
        ledControl.powerLED.mode = LED_ON;
        DEBUG_PRINTLN("[WEB] Changed ZbRole to ROUTER");
    }
    else if (fwMode == "thread")
    {
        systemCfg.zbRole = OPENTHREAD;
        DEBUG_PRINTLN("[WEB] Changed ZbRole to OPENTHREAD");
    }
    saveSystemConfig(systemCfg);
    backhaulRoleChanged();
}

static void apiCmdDefault(String &result)
{
    serverWeb.send(HTTP_CODE_BAD_REQUEST, contTypeText, result);
}

static void apiCmdZbRouterRecon(String &result)
{
    zigbeeRouterRejoin();
}

static void apiCmdZbRestart(String &result)
{
    zigbeeRestart();
}

static void apiCmdZbEnableBsl(String &result)
{
    zigbeeEnableBSL();
}

static void apiCmdEspReset(String &result)
{
    serverWeb.send(HTTP_CODE_OK, contTypeText, result);
    delay(250);
    ESP.restart();
}

static void apiCmdAdapterLan(String &result)
{
    usbModeSet(XZG);
}

static void apiCmdAdapterUsb(String &result)
{
    usbModeSet(ZIGBEE);
}

static void apiCmdClearLog(String &result)
{
    logClear();
}

static void apiCmdZbCheckHardware(String &result)
{
    zbHwCheck();
}

static void apiCmdEraseNvram(String &result)
{
    xTaskCreate(zbEraseNV, "zbEraseNV", 2048, NULL, 5, NULL);
}

static void apiCmdDnsCheck(String &result)
{
    checkDNS();
}

static void apiCmdClientCheck (String &result) {
    serverWeb.send(HTTP_CODE_OK, contTypeText, String(numOfConnectedClients()));
}

static void apiCmd()
{
    static void (*apiCmdFunctions[])(String &result) =
    {
        apiCmdDefault,
        apiCmdZbRouterRecon,
        apiCmdZbRestart,
        apiCmdZbEnableBsl,
        apiCmdEspReset,
        apiCmdAdapterLan,
        apiCmdAdapterUsb,
        apiCmdLedAct,
        apiCmdZbFlash,
        apiCmdClearLog,
        apiCmdUpdateUrl,
        apiCmdZbCheckFirmware,
        apiCmdZbCheckHardware,
        apiCmdZbLedToggle,
        apiCmdFactoryReset,
        apiCmdEraseNvram,
        apiCmdDnsCheck,
        apiCmdClientCheck,
    };
    constexpr int numFunctions = sizeof(apiCmdFunctions) / sizeof(apiCmdFunctions[0]);
    String result = apiWrongArgs;

    if (serverWeb.hasArg(argCmd))
    {
        result = "ok";

        // I add 1 to allow 0 to be default+overflow/invalid case.
        // Ideally, the client would send 1-indexed "cmd"s
        // but then I'd need to modify this in ALL of the client code...
        uint8_t command     = serverWeb.arg(argCmd).toInt() + 1;
        bool    boundsCheck = command < numFunctions;

        apiCmdFunctions[command * boundsCheck](result);

        serverWeb.send(HTTP_CODE_OK, contTypeText, result);
    }

}

static void apiWifiConnnectStat()
{
    String result;
    StaticJsonDocument<70> doc;
    const char *connected = "connected";
    if (WiFi.status() == WL_CONNECTED)
    {
        doc[connected] = true;
        doc["ip"]      = WiFi.localIP().toString();
    }
    else
    {
        doc[connected] = false;
    }
    serializeJson(doc, result);
    serverWeb.send(HTTP_CODE_OK, contTypeJson, result);
}

static void apiGetFile()
{
    String result = apiWrongArgs;

    if (serverWeb.hasArg(argFilename))
    {
        String filename = "/" + serverWeb.arg(argFilename);
        File file = LittleFS.open(filename, "r");
        if (!file)
        {
            return;
        }
        result = "";
        while (file.available() && result.length() < 500) {
            result += (char)file.read();
        }
        file.close();
    }
    serverWeb.send(HTTP_CODE_OK, contTypeText, result);
}

static void apiDelFile()
{
    String result = apiWrongArgs;

    if (serverWeb.hasArg(argFilename))
    {
        String filename = "/" + serverWeb.arg(argFilename);
        LOGW("Remove file %s", filename.c_str());
        LittleFS.remove(filename);
    }
    serverWeb.send(HTTP_CODE_OK, contTypeText, result);
}

static void apiGetParam()
{
    String resp = apiWrongArgs;
    if (serverWeb.hasArg(argParam))
    {
        if (serverWeb.arg(argParam) == "refreshLogs")
        {
            resp = (String)systemCfg.refreshLogs;
        }
        else if (serverWeb.arg(argParam) == "update_root")
        {
            resp = getRootData(true);
        }
        else if (serverWeb.arg(argParam) == "coordMode")
        {
            if (wifiWebSetupInProgress)
            {
                resp = "1";
            }
            else
            {
                resp = (String)systemCfg.workMode;
            }
        }
        else if (serverWeb.arg(argParam) == "zbFwVer")
        {
            resp = String(CCTool.chip.fwRev);
        }
        else if (serverWeb.arg(argParam) == "zbHwVer")
        {
            resp = String(CCTool.chip.hwRev);
        }
        else if (serverWeb.arg(argParam) == "espVer")
        {
            resp = VERSION;
        }
        else if (serverWeb.arg(argParam) == "wifiEnable")
        {
            resp = networkCfg.wifiEnable;
        }
        else if (serverWeb.arg(argParam) == "all")
        {
            resp = makeJsonConfig(&networkCfg, &vpnCfg, &mqttCfg, &systemCfg);
        }
        else if (serverWeb.arg(argParam) == "vars")
        {
            resp = makeJsonConfig(NULL, NULL, NULL, NULL, &vars);
        }
        else if (serverWeb.arg(argParam) == "root")
        {
            resp = getRootData();
        }
    }
    serverWeb.send(HTTP_CODE_OK, contTypeText, resp);
}

static void apiStartWifiScan()
{
    if (WiFi.getMode() == WIFI_OFF)
    { // enable wifi for scan
        WiFi.mode(WIFI_STA);
    }
    // } else if (WiFi.getMode() == WIFI_AP) {  // enable sta for scan
    //     WiFi.mode(WIFI_AP_STA);
    // }
    WiFi.scanNetworks(true);
    serverWeb.send(HTTP_CODE_OK, contTypeTextHtml, apiOk);
}

static void apiWifiScanStatus()
{
    static uint8_t       timeout  = 0;
    DynamicJsonDocument  doc(1024);
    String               result   = "";
    int16_t              scanRes  = WiFi.scanComplete();
    const char          *scanDone = "scanDone";

    doc[scanDone] = false;
    if (scanRes == -2)
    {
        WiFi.scanNetworks(true);
    }
    else if (scanRes > 0)
    {
        doc[scanDone] = true;
        JsonArray wifi = doc.createNestedArray("wifi");
        for (int i = 0; i < scanRes; ++i)
        {
            JsonObject wifi_0 = wifi.createNestedObject();
            wifi_0["ssid"]    = WiFi.SSID(i);
            wifi_0["rssi"]    = WiFi.RSSI(i);
            wifi_0["channel"] = WiFi.channel(i);
            wifi_0["secure"]  = WiFi.encryptionType(i);
        }
        WiFi.scanDelete();
    }
    if (timeout < 10)
    {
        timeout++;
    }
    else
    {
        doc[scanDone] = true;
        WiFi.scanDelete();
        timeout = 0;
    }
    serializeJson(doc, result);
    serverWeb.send(HTTP_CODE_OK, contTypeJson, result);
}

static void apiGetPage()
{
    if (!serverWeb.arg(argPage).length())
    {
        LOGW("wrong arg 'page' %s", serverWeb.argName(1));
        serverWeb.send(500, contTypeText, apiWrongArgs);
        return;
    }
    switch (serverWeb.arg(argPage).toInt())
    {
    case API_PAGE_ROOT:
        handleRoot();
        sendGzip(contTypeTextHtml, PAGE_ROOT_html_gz, PAGE_ROOT_html_gz_len);
        break;
    case API_PAGE_GENERAL:
        handleGeneral();
        sendGzip(contTypeTextHtml, PAGE_GENERAL_html_gz, PAGE_GENERAL_html_gz_len);
        break;
    case API_PAGE_NETWORK:
        handleNetwork();
        sendGzip(contTypeTextHtml, PAGE_NETWORK_html_gz, PAGE_NETWORK_html_gz_len);
        break;
    case API_PAGE_ZIGBEE:
        handleSerial();
        sendGzip(contTypeTextHtml, PAGE_ZIGBEE_html_gz, PAGE_ZIGBEE_html_gz_len);
        break;
    case API_PAGE_TOOLS:
        handleTools();
        sendGzip(contTypeTextHtml, PAGE_TOOLS_html_gz, PAGE_TOOLS_html_gz_len);
        break;
    case API_PAGE_MQTT:
        handleMqtt();
        sendGzip(contTypeTextHtml, PAGE_MQTT_html_gz, PAGE_MQTT_html_gz_len);
        break;
    default:
        break;
    }
}

static void apiGetFileList()
{
    String fileList = "";
    DynamicJsonDocument doc(512);
    JsonArray files = doc.createNestedArray("files");
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    while (file)
    {
        JsonObject jsonfile = files.createNestedObject();
        jsonfile["filename"] = String(file.name());
        jsonfile["size"] = file.size();
        file = root.openNextFile();
    }
    root = LittleFS.open("/config/");
    file = root.openNextFile();
    while (file)
    {
        JsonObject jsonfile = files.createNestedObject();
        jsonfile["filename"] = String("/config/" + String(file.name()));
        jsonfile["size"] = file.size();
        file = root.openNextFile();
    }
    serializeJson(doc, fileList);
    serverWeb.send(HTTP_CODE_OK, contTypeJson, fileList);
}

static void apiDefault()
{
    LOGW("switch (action) err");
}

void handleApi()
{
    // Example api invocation:
    // http://xzg.local/api?action=0&page=0

    // apiFunctions[] will need to correspond to the
    // values set in the JS frontend for "action"
    static void (*apiFunctions[])() = {
        apiDefault,
        apiGetPage,
        apiGetParam,
        apiStartWifiScan,
        apiWifiScanStatus,
        apiGetFileList,
        apiGetFile,
        apiDefault,   // invoked by what was previously API_SEND_HEX.
        apiWifiConnnectStat,
        apiCmd,
        apiGetLog,
        apiDelFile
    };
    constexpr int numFunctions = sizeof(apiFunctions) / sizeof(apiFunctions[0]);
    if (!is_authenticated()) {
        redirectLogin(1);
        return;
    }
    if (serverWeb.argName(0) == argAction) {
        // I add 1 to allow 0 to be default+overflow/invalid case.
        // Ideally, the client would send 1-indexed "action"s
        // but then I'd need to modify this in ALL of the client code...
        const uint8_t action      = serverWeb.arg(argAction).toInt() + 1;
        const bool    boundsCheck = action < numFunctions;
        apiFunctions[action * boundsCheck]();
    }
    else {
        serverWeb.send(500, contTypeText, apiWrongArgs);
    }
}

void handleSaveParams()
{
    if (!is_authenticated())
        return;
    updateConfiguration(serverWeb, systemCfg, networkCfg, vpnCfg, mqttCfg);
}

void updateWebTask(void *parameter)
{
    while (1)
    {
        if(eventsConnected()) {
            // The browser already merges a JSON object. Send the whole snapshot
            // and finish in one write, not two writes for every individual key.
            String root_data=getRootData(true);
            sendEventRecord("root_update",root_data,true);
        }
        // Always yield after work; no catch-up bursts after a slow sensor/read.
        vTaskDelay(pdMS_TO_TICKS(webRefreshSeconds(systemCfg.refreshLogs)*1000));
    }
}

void handleLoginGet()
{
    if (!is_authenticated())
    {
        // LOGD("handleLoginGet !is_authenticated");
        sendGzip(contTypeTextHtml, PAGE_LOGIN_html_gz, PAGE_LOGIN_html_gz_len);
    }
    else
    {
        // LOGD("handleLoginGet else");
        serverWeb.sendHeader("Location", "/");
        serverWeb.sendHeader("Cache-Control", "no-cache");
        serverWeb.send(301);
        // sendGzip(contTypeTextHtml, PAGE_LOADER_html_gz, PAGE_LOADER_html_gz_len);
    }
}
void handleLoginPost()
{
    // Serial.println("Handle Login");
    // String msg;

    /*if (serverWeb.hasHeader("Cookie"))
    {
        // Print cookies
        Serial.print("Found cookie: ");
        String cookie = serverWeb.header("Cookie");
        Serial.println(cookie);
    }*/

    if (serverWeb.hasArg("username") && serverWeb.hasArg("password"))
    {
        // Serial.print("Found parameter: ");

        if (serverWeb.arg("username") == String(systemCfg.webUser) && serverWeb.arg("password") == String(systemCfg.webPass))
        {
            serverWeb.sendHeader("Location", "/");
            serverWeb.sendHeader("Cache-Control", "no-cache");

            String token = sha1(String(systemCfg.webUser) + ":" + String(systemCfg.webPass) + ":" + serverWeb.client().remoteIP().toString());
            serverWeb.sendHeader("Set-Cookie", "XZG_UID=" + token);

            serverWeb.send(301);
            // Serial.println("Log in Successful");
            return;
        }
        // msg = ;
        //  Serial.println("Log in Failed");
        redirectLogin(2);
        return;
    }
}

void handleLogout()
{
    // Serial.println("Disconnection");
    serverWeb.sendHeader("Set-Cookie", "XZG_UID=0");
    serverWeb.sendHeader("Authentication", "fail");
    redirectLogin(3);

    // serverWeb.send_P(401, contTypeTextHtml, (const char *)PAGE_LOGOUT_html_gz, PAGE_LOGOUT_html_gz_len); });*/

    return;
}
// Check if header is present and correct
bool is_authenticated()
{
    if (systemCfg.webAuth)
    {
        if (serverWeb.hasHeader("Cookie"))
        {
            String cookie = serverWeb.header("Cookie");
            String token = sha1(String(systemCfg.webUser) + ":" + String(systemCfg.webPass) + ":" + serverWeb.client().remoteIP().toString());

            if (cookie.indexOf("XZG_UID=" + token) != -1)
            {
                // LOGD("Successful");
                serverWeb.sendHeader("Authentication", "ok");
                return true;
            }
        }
        // LOGD("Failed");
        serverWeb.sendHeader("Authentication", "fail");
        return false;
    }
    else
    {
        return true;
    }
}

void handleGeneral()
{
    DynamicJsonDocument doc(1024);
    String result;

    doc[hwBtnIsKey] = vars.hwBtnIs;
    // doc[hwUartSelIsKey] = vars.hwUartSelIs;
    doc[hwLedPwrIsKey] = vars.hwLedPwrIs;
    doc[hwLedUsbIsKey] = vars.hwLedUsbIs;

    /*switch (systemCfg.workMode)
    {
    case WORK_MODE_USB:
        doc["checkedUsbMode"] = checked;
        break;
    case WORK_MODE_NETWORK:
        doc["checkedLanMode"] = checked;
        break;
    default:
        break;
    }

    if (systemCfg.keepWeb)
    {
        doc[keepWebKey] = checked;
    }*/

    if (systemCfg.disableLedPwr)
    {
        doc["checkedDisableLedPwr"] = checked;
    }
    if (systemCfg.disableLedUSB)
    {
        doc["checkedDisableLedUSB"] = checked;
    }

    doc[hostnameKey] = systemCfg.hostname;
    doc[refreshLogsKey] = systemCfg.refreshLogs;

    if (systemCfg.timeZone)
    {
        doc[timeZoneNameKey] = systemCfg.timeZone;
    }
    doc[ntpServ1Key] = systemCfg.ntpServ1; //.toString();
    doc[ntpServ2Key] = systemCfg.ntpServ2; //.toString();

    doc[nmStartHourKey] = systemCfg.nmStart;
    doc[nmEndHourKey] = systemCfg.nmEnd;
    if (systemCfg.nmEnable)
    {
        doc[nmEnableKey] = checked;
    }

    if (systemCfg.updAutoInst)
    {
        doc[updAutoInstKey] = checked;
    }
    doc[updCheckTimeKey] = systemCfg.updCheckTime;
    doc[updCheckDayKey] = systemCfg.updCheckDay;

    serializeJson(doc, result);
    serverWeb.sendHeader(respHeaderName, result);

    DynamicJsonDocument zones(8000);
    String results;

    JsonArray zonesArray = zones.to<JsonArray>();
    for (int i = 0; i < timeZoneCount; i++)
    {
        zonesArray.add(timeZones[i].zone);
    }

    // size_t usedMemory = zones.memoryUsage();
    // LOGD("Zones used: %s bytes", String(usedMemory));

    serializeJson(zones, results);
    serverWeb.sendHeader(respTimeZonesName, results);
}

void handleNetwork()
{
    String result;
    DynamicJsonDocument doc(1024);

    // doc["pageName"] = "Config WiFi";

    if (networkCfg.ethEnable)
    {
        doc[ethEnblKey] = checked;
    }
    if (networkCfg.ethDhcp)
    {
        doc[ethDhcpKey] = checked;
    }
    doc[ethIpKey] = networkCfg.ethIp;
    doc[ethMaskKey] = networkCfg.ethMask;
    doc[ethGateKey] = networkCfg.ethGate;

    doc[ethDns1Key] = networkCfg.ethDns1;
    doc[ethDns2Key] = networkCfg.ethDns2;

    if (networkCfg.wifiEnable)
    {
        doc[wifiEnblKey] = checked;
    }
    doc[wifiSsidKey] = String(networkCfg.wifiSsid);
    doc[wifiPassKey] = String(networkCfg.wifiPass);
    if (networkCfg.wifiDhcp)
    {
        doc[wifiDhcpKey] = checked;
    }
    doc[wifiIpKey] = networkCfg.wifiIp;
    doc[wifiMaskKey] = networkCfg.wifiMask;
    doc[wifiGateKey] = networkCfg.wifiGate;
    doc[wifiDns1Key] = networkCfg.wifiDns1;
    doc[wifiDns2Key] = networkCfg.wifiDns2;

    switch (networkCfg.wifiMode)
    {
        case WIFI_PROTOCOL_11B:
            doc["1"] = checked;
            break;
        case WIFI_PROTOCOL_11G:
            doc["2"] = checked;
            break;
        case WIFI_PROTOCOL_11N:
            doc["4"] = checked;
            break;
        case WIFI_PROTOCOL_LR:
            doc["8"] = checked;
            break;
        default:
            break;
    }

    switch (networkCfg.wifiPower)
    {
        case WIFI_POWER_19_5dBm:
            doc["78"] = checked;
            break;
        case WIFI_POWER_19dBm:
            doc["76"] = checked;
            break;
        case WIFI_POWER_18_5dBm:
            doc["74"] = checked;
            break;
        case WIFI_POWER_17dBm:
            doc["68"] = checked;
            break;
        case WIFI_POWER_15dBm:
            doc["60"] = checked;
            break;
        case WIFI_POWER_13dBm:
            doc["52"] = checked;
            break;
        case WIFI_POWER_11dBm:
            doc["44"] = checked;
            break;
        case WIFI_POWER_8_5dBm:
            doc["34"] = checked;
            break;
        case WIFI_POWER_7dBm:
            doc["28"] = checked;
            break;
        case WIFI_POWER_5dBm:
            doc["20"] = checked;
            break;
        case WIFI_POWER_2dBm:
            doc["8"] = checked;
            break;
        default:
            break;
    }

    if (hwConfig.eth.mdcPin == -1 || hwConfig.eth.mdiPin == -1)
    {
        doc["no_eth"] = 1;
    }

    if (vpnCfg.wgEnable)
    {
        doc[wgEnableKey] = checked;
    }
    doc[wgLocalIPKey]      = vpnCfg.wgLocalIP.toString();
    doc[wgLocalSubnetKey]  = vpnCfg.wgLocalSubnet.toString();
    doc[wgLocalPortKey]    = vpnCfg.wgLocalPort;
    doc[wgLocalGatewayKey] = vpnCfg.wgLocalGateway.toString();
    doc[wgLocalPrivKeyKey] = vpnCfg.wgLocalPrivKey;
    doc[wgEndAddrKey]      = vpnCfg.wgEndAddr;
    doc[wgEndPubKeyKey]    = vpnCfg.wgEndPubKey;
    doc[wgEndPortKey]      = vpnCfg.wgEndPort;
    doc[wgAllowedIPKey]    = vpnCfg.wgAllowedIP.toString();
    doc[wgAllowedMaskKey]  = vpnCfg.wgAllowedMask.toString();
    if (vpnCfg.wgMakeDefault)
    {
        doc[wgMakeDefaultKey] = checked;
    }
    doc[wgPreSharedKeyKey] = vpnCfg.wgPreSharedKey;

    if (vpnCfg.hnEnable)
    {
        doc[hnEnableKey] = checked;
    }
    doc[hnJoinCodeKey] = vpnCfg.hnJoinCode;
    doc[hnHostNameKey] = vpnCfg.hnHostName;
    doc[hnDashUrlKey]  = vpnCfg.hnDashUrl;

    serializeJson(doc, result);
    serverWeb.sendHeader(respHeaderName, result);
}

void handleSerial()
{
    String result;
    DynamicJsonDocument doc(1024);

    switch (systemCfg.workMode)
    {
    case WORK_MODE_USB:
        doc["usbMode"] = checked;
        break;
    case WORK_MODE_NETWORK:
        doc["lanMode"] = checked;
        break;
    default:
        break;
    }

    switch (systemCfg.serialSpeed)
    {
    case 9600:
        doc["9600"] = checked;
        break;
    case 19200:
        doc["19200"] = checked;
        break;
    case 38400:
        doc["38400"] = checked;
        break;
    case 57600:
        doc["57600"] = checked;
        break;
    case 115200:
        doc["115200"] = checked;
        break;
    case 230400:
        doc["230400"] = checked;
        break;
    case 460800:
        doc["460800"] = checked;
        break;
    default:
        doc["115200"] = checked;
        break;
    }

    doc[socketPortKey] = String(systemCfg.socketPort);
    doc[zbRoleKey]     = systemCfg.zbRole;

    serializeJson(doc, result);
    serverWeb.sendHeader(respHeaderName, result);
}

void handleMqtt()
{
    String result;
    DynamicJsonDocument doc(1024);

    if (mqttCfg.enable)
    {
        doc["enableMqtt"] = checked;
    }
    doc["serverMqtt"]    = mqttCfg.server;
    doc["portMqtt"]      = mqttCfg.port;
    doc["userMqtt"]      = mqttCfg.user;
    doc["passMqtt"]      = mqttCfg.pass;
    doc["topicMqtt"]     = mqttCfg.topic;
    doc["intervalMqtt"]  = mqttCfg.updateInt;
    doc["mqttReconnect"] = mqttCfg.reconnectInt;

    if (mqttCfg.discovery)
    {
        doc["discoveryMqtt"] = checked;
    }

    serializeJson(doc, result);
    serverWeb.sendHeader(respHeaderName, result);
}

static void getRootEthTab(DynamicJsonDocument &doc,
                          bool update,
                          const String &noConn)
{
    // ETHERNET TAB
    const char *ethConn = "ethConn";
    const char *ethMac  = "ethMac";
    const char *ethSpd  = "ethSpd";
    const char *ethDns  = "ethDns";

    if (networkCfg.ethEnable)
    {
        if (!update)
        {
            doc[ethMac] = ETH.macAddress();
        }
        doc[ethConn]    = vars.connectedEther ? 1 : 0;
        doc[ethDhcpKey] = networkCfg.ethDhcp ? 1 : 0;
        if (vars.connectedEther)
        {
            doc[ethSpd]     = ETH.linkSpeed();
            doc[ethIpKey]   = ETH.localIP().toString();
            doc[ethMaskKey] = ETH.subnetMask().toString();
            doc[ethGateKey] = ETH.gatewayIP().toString();
            doc[ethDns]     = vars.savedEthDNS.toString(); // ETH.dnsIP().toString();
        }
        else
        {
            doc[ethSpd]     = noConn;
            doc[ethIpKey]   = networkCfg.ethDhcp ? noConn : ETH.localIP().toString();
            doc[ethMaskKey] = networkCfg.ethDhcp ? noConn : ETH.subnetMask().toString();
            doc[ethGateKey] = networkCfg.ethDhcp ? noConn : ETH.gatewayIP().toString();
            doc[ethDns]     = networkCfg.ethDhcp ? noConn : vars.savedEthDNS.toString(); // ETH.dnsIP().toString();
        }
    }

}

static inline void getRootWifi(DynamicJsonDocument &doc,
                        bool update,
                        const String &noConn)
{
    const char *wifiRssi = "wifiRssi";
    const char *wifiConn = "wifiConn";
    const char *wifiMode = "wifiMode";
    const char *wifiDns  = "wifiDns";

    if (!update)
    {
        doc["wifiMac"] = WiFi.macAddress();
        String boardStr = hwConfig.board;
        if (boardStr.startsWith("Multi"))
        {
            int underscoreIndex = boardStr.indexOf('_');
            if (underscoreIndex != -1)
            {
                String boardNumber = boardStr.substring(underscoreIndex + 1);
                boardNumber.trim();
                const int boardNum = boardNumber.toInt();

                String boardArray[BOARD_CFG_CNT];
                int arrayIndex = 0;

                for (int brdNewIdx = 0; brdNewIdx < BOARD_CFG_CNT; brdNewIdx++)
                {
                    if (brdConfigs[brdNewIdx].ethConfigIndex == brdConfigs[boardNum].ethConfigIndex
                        && brdConfigs[brdNewIdx].zbConfigIndex == brdConfigs[boardNum].zbConfigIndex
                        && brdConfigs[brdNewIdx].mistConfigIndex == brdConfigs[boardNum].mistConfigIndex)
                    {
                        boardArray[arrayIndex++] = brdConfigs[brdNewIdx].board;
                    }
                }

                DynamicJsonDocument jsonDoc(512);
                JsonArray jsonArray = jsonDoc.to<JsonArray>();

                for (int i = 0; i < arrayIndex; i++)
                {
                    jsonArray.add(boardArray[i]);
                }

                String jsonString;
                serializeJson(jsonArray, jsonString);

                doc["boardArray"] = jsonString;
            }
        }
    }

    if (networkCfg.wifiEnable)
    {
        doc[wifiMode]    = 1; //"Client";
        doc[wifiDhcpKey] = networkCfg.wifiDhcp ? 1 : 0;
        if (WiFi.status() == WL_CONNECTED)
        { // STA connected
            doc[wifiSsidKey] = WiFi.SSID();
            doc[wifiRssi]    = WiFi.RSSI();
            doc[wifiConn]    = 1;
            doc[wifiIpKey]   = WiFi.localIP().toString();
            doc[wifiMaskKey] = WiFi.subnetMask().toString();
            doc[wifiGateKey] = WiFi.gatewayIP().toString();
            doc[wifiDns]     = vars.savedWifiDNS.toString(); // WiFi.dnsIP().toString();
        }
        else
        {
            doc[wifiSsidKey] = networkCfg.wifiSsid;
            doc[wifiRssi]    = noConn;
            doc[wifiConn]    = 0;
            doc[wifiIpKey]   = networkCfg.wifiDhcp ? noConn : WiFi.localIP().toString();
            doc[wifiMaskKey] = networkCfg.wifiDhcp ? noConn : WiFi.subnetMask().toString();
            doc[wifiGateKey] = networkCfg.wifiDhcp ? noConn : WiFi.gatewayIP().toString();
            doc[wifiDns]     = networkCfg.wifiDhcp ? noConn : vars.savedWifiDNS.toString(); // WiFi.dnsIP().toString();
        }
    }

    if (vars.apStarted)
    { // AP active
        String AP_NameString;

        doc[wifiMode]    = 2;
        doc[wifiConn]    = 1;
        doc[wifiSsidKey] = vars.deviceId;
        doc[wifiIpKey  ] = WiFi.localIP().toString(); //"192.168.1.1 (XZG web interface)";
        doc[wifiMaskKey] = "255.255.255.0 (Access point)";
        doc[wifiGateKey] = "192.168.1.1 (this device)";
        doc[wifiDhcpKey] = "On (Access point)";
        doc[wifiMode]    = 2;      //"AP";
        doc[wifiRssi]    = noConn; //"N/A";
    }
}

static inline void getRootMqtt(DynamicJsonDocument &doc)
{
    if (mqttCfg.enable)
    {
        const char *mqConnect = "mqConnect";
        const char *mqBroker  = "mqBroker";

        doc[mqBroker]  = mqttCfg.server;
        doc[mqConnect] = vars.mqttConn ? 1 : 0;
    }
}

static inline void getRootVpnWireGuard(DynamicJsonDocument &doc)
{
    if (vpnCfg.wgEnable)
    {
        const char *wgInit       = "wgInit";
        const char *wgDeviceAddr = "wgDeviceAddr";
        const char *wgRemoteAddr = "wgRemoteAddr";
        const char *wgConnect    = "wgConnect";
        const char *wgRemoteIP   = "wgRemoteIp";
        // const char *wgEndPort = "wgEndPort";

        doc[wgInit]       = vars.vpnWgInit ? 1 : 0;
        doc[wgDeviceAddr] = vpnCfg.wgLocalIP.toString();
        doc[wgRemoteAddr] = vpnCfg.wgEndAddr;
        doc[wgConnect]    = vars.vpnWgConnect ? 1 : 0;
        doc[wgRemoteIP]   = vars.vpnWgPeerIp.toString();
        // doc[wgEndPort] = vpnCfg.wgEndPort;
    }
}

static inline void getRootVpnHusarnet(DynamicJsonDocument &doc)
{
    if (vpnCfg.hnEnable)
    {
        const char *hnInit     = "hnInit";
        const char *hnHostName = "hnHostName";

        // doc[wgDeviceAddr] = vpnCfg.wgLocalIP.toString();//WgSettings.localAddr;
        doc[hnHostName] = vpnCfg.hnHostName;
        doc[hnInit] = vars.vpnHnInit ? 1 : 0;
    }
}

static inline void getRootMode(DynamicJsonDocument &doc){
    const char *ccMode = "ccMode";
    doc[ccMode] = systemCfg.zbRole;
}

static inline void getRootUptime(DynamicJsonDocument &doc)
{
    doc["uptime"] = millis(); // readableTime;
}

static inline void getRootCpuTemp(DynamicJsonDocument &doc)
{
    float CPUtemp = getCPUtemp();
    doc["deviceTemp"] = String(CPUtemp);
}

static inline void getRootOneWireTemp(DynamicJsonDocument &doc)
{
    if (vars.oneWireIs)
    {
        float temp = get1wire();
        doc["1wTemp"] = String(temp);
    }
}

// Some misc hardware info,
// maybe organise this more
static inline void getRootHwMisc(DynamicJsonDocument &doc, bool update)
{
    if (update)
    {
        return;
    }
    char verArr[25];
    const char *env = STRINGIFY(BUILD_ENV_NAME);

    if (strcasestr(env, "debug") != NULL)
    {
        sprintf(verArr, "%s (%s)", VERSION, env);
    }
    else
    {
        sprintf(verArr, "%s", VERSION);
    }

    doc["VERSION"] = String(verArr);

    const char *operationalMode = "operationalMode";
    doc[operationalMode] = systemCfg.workMode;

    doc[espUpdAvailKey] = vars.updateEspAvail;
    doc[zbUpdAvailKey]  = vars.updateZbAvail;

    doc[hostnameKey] = systemCfg.hostname;
    doc["hwRev"]    = hwConfig.board;
    doc["espModel"] = String(ESP.getChipModel());
    doc["espCores"] = ESP.getChipCores();
    doc["espFreq"]  = ESP.getCpuFreqMHz();

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    const char *espFlashType = "espFlashType";
    if (chip_info.features & CHIP_FEATURE_EMB_FLASH)
    {
        doc[espFlashType] = 1;
    }
    else
    {
        doc[espFlashType] = 2;
    }

    doc["espFlashSize"] = ESP.getFlashChipSize() / (1024 * 1024);

    if (CCTool.chip.fwRev > 0)
    {
        doc["zigbeeFwRev"] = String(CCTool.chip.fwRev);
    }
    else if(systemCfg.zbRole != COORDINATOR) {
        doc["zigbeeFwRev"] = String(systemCfg.zigBeeFwVersion);
    }
    else
    {
        doc["zigbeeFwRev"] = String(systemCfg.zbFw);
        doc["zbFwSaved"]   = true;
    }

    doc["zigbeeHwRev"]  = CCTool.chip.hwRev;
    doc["zigbeeIeee"]   = CCTool.chip.ieee;
    doc["zigbeeFlSize"] = String(CCTool.chip.flashSize / 1024);

    unsigned int totalFs = LittleFS.totalBytes() / 1024;
    unsigned int usedFs  = LittleFS.usedBytes() / 1024;
    doc["espFsSize"] = totalFs;
    doc["espFsUsed"] = usedFs;

    doc[zbRoleKey]       = systemCfg.zbRole;
    doc["zigbeeFwSaved"] = systemCfg.zbFw;
}

static inline void getRootNvsStats(DynamicJsonDocument &doc)
{
    int total, used;
    getNvsStats(&total, &used);

    doc["espNvsSize"] = total;
    doc["espNvsUsed"] = used;
}

static inline void getRootSockets(DynamicJsonDocument &doc)
{
    const char *connectedSocketStatus = "connectedSocketStatus";
    const char *connectedSocket       = "connectedSocket";

    doc[connectedSocketStatus] = vars.connectedClients;
    doc[connectedSocket]       = vars.socketTime;
}

static inline void getRootTime(DynamicJsonDocument &doc)
{
    doc["localTime"] = getTime();
}

String getRootData(bool update)
{
    DynamicJsonDocument doc(2048);

    const char *noConn = "noConn";
    getRootEthTab(doc, update, noConn);
    getRootWifi(doc, update, noConn);
    getRootHwMisc(doc, update);
    getRootSockets(doc);
    getRootTime(doc);
    getRootUptime(doc);
    getRootCpuTemp(doc);
    getRootOneWireTemp(doc);
    getRootNvsStats(doc);
    getRootMqtt(doc);
    getRootVpnWireGuard(doc);
    getRootVpnHusarnet(doc);
    getRootMode(doc);

    String result;
    serializeJson(doc, result);

    return result;
}

void handleRoot()
{
    String result = getRootData();
    serverWeb.sendHeader(respHeaderName, result);
}

void handleTools()
{
    String result;
    DynamicJsonDocument doc(512);

    doc[hwBtnIsKey]    = vars.hwBtnIs;
    doc[hwLedPwrIsKey] = vars.hwLedPwrIs;
    doc[hwLedUsbIsKey] = vars.hwLedUsbIs;
    // doc[hwUartSelIsKey] = vars.hwUartSelIs;
    // doc["hostname"]     = systemCfg.hostname;
    // doc["refreshLogs"]  = systemCfg.refreshLogs;

    // handle security stuff
    if (systemCfg.disableWeb)
    {
        doc[disableWebKey] = checked;
    }

    if (systemCfg.webAuth)
    {
        doc[webAuthKey] = checked;
    }
    doc[webUserKey] = (String)systemCfg.webUser;
    doc[webPassKey] = (String)systemCfg.webPass;
    if (systemCfg.fwEnabled)
    {
        doc[fwEnabledKey] = checked;
    }
    doc[fwIpKey] = systemCfg.fwIp;

    serializeJson(doc, result);
    serverWeb.sendHeader(respHeaderName, result);
}

void handleSavefile()
{
    if (!is_authenticated())
        return;
    if (serverWeb.method() != HTTP_POST)
    {
        serverWeb.send(405, contTypeText, F("Method Not Allowed"));
    }
    else
    {
        String filename = "/" + serverWeb.arg(0);
        String content  = serverWeb.arg(1);
        File   file     = LittleFS.open(filename, "w");

        LOGD("try %s", filename.c_str());

        if (!file)
        {
            LOGW("Failed to open file for reading");
            return;
        }
        int bytesWritten = file.print(content);
        if (bytesWritten > 0)
        {
            LOGD("File was written, %d", bytesWritten);
        }
        else
        {
            LOGW("File write failed");
        }

        file.close();
        serverWeb.sendHeader(F("Location"), F("/tools"));
        serverWeb.send(303);
    }
}

/* ----- Multi-tool support | START -----*/
void handleZigbeeBSL()
{
    if (!is_authenticated()) { serverWeb.send(401); return; }
    zigbeeEnableBSL();
    serverWeb.send(HTTP_CODE_OK, contTypeText, "Zigbee BSL");
}

void handleZigbeeRestart()
{
    if (!is_authenticated()) { serverWeb.send(401); return; }
    zigbeeRestart();
    serverWeb.send(HTTP_CODE_OK, contTypeText, "Zigbee Restart");
}
/* ----- Multi-tool support | END -----*/

void printLogTime()
{
    String tmpTime;
    unsigned long timeLog = millis();
    tmpTime = String(timeLog, DEC);
    logPush('[');
    for (int j = 0; j < tmpTime.length(); j++)
    {
        logPush(tmpTime[j]);
    }
    logPush(']');
}

void printLogMsg(String msg)
{
    printLogTime();
    logPush(' ');
    logPush('|');
    logPush(' ');
    for (int j = 0; j < msg.length(); j++)
    {
        logPush(msg[j]);
    }
    logPush('\n');
    LOGI("%s", msg.c_str());
}

void progressFunc(unsigned int progress, unsigned int total)
{
    float percent = total ? std::min(99.0f, ((float)progress / total) * 100.0f) : 0;

    sendEventSafe(tagESP_FW_prgs, String(percent));
    // printLogMsg(String(percent));

#ifdef DEBUG
    if (int(percent) % 5 == 0)
    {
        LOGD("Update ESP32 progress: %s of %s | %s%", String(progress), String(total), String(percent));
    }
#endif
};

bool getEspUpdate(String esp_fw_url)
{
    if(espUpdateRestartAt) return false;
    espUpdateState="uploading"; espUpdateError="";
    BackhaulMaintenance maintenance;
    if(!maintenance) { failEspUpdate("radio_busy"); return false; }
    checkDNS();
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure(); // Preserve the stock URL updater's HTTPS policy.
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.begin(client,esp_fw_url);
    http.addHeader("Content-Type","application/octet-stream");
    if(http.GET()!=HTTP_CODE_OK) { http.end(); failEspUpdate("download_failed"); return false; }
    int remaining=http.getSize();
    if(remaining<=0 || !Update.begin(remaining)) {
        http.end(); failEspUpdate("ota_begin_failed"); return false;
    }
    Update.onProgress(progressFunc);
    WiFiClient *stream=http.getStreamPtr();
    uint8_t buffer[512];
    uint32_t lastProgress=millis();
    bool ok=true;
    while(remaining>0) {
        size_t available=stream->available();
        if(available) {
            size_t count=std::min(available,std::min(sizeof(buffer),static_cast<size_t>(remaining)));
            int received=stream->readBytes(buffer,count);
            if(received<=0 || Update.write(buffer,received)!=static_cast<size_t>(received)) { ok=false; break; }
            remaining-=received; lastProgress=millis();
        } else if(!http.connected() || millis()-lastProgress>10000) { ok=false; break; }
        delay(1);
    }
    http.end();
    if(ok && remaining==0 && Update.end()) {
        printLogMsg("[ESP] Update complete; restarting"); completeEspUpdate(); return true;
    } else {
        failEspUpdate("ota_incomplete_or_invalid"); return false;
    }
}

String fetchLatestEspFw(bool refreshDns)
{
    if(refreshDns) checkDNS();
    HTTPClient http;
    http.setConnectTimeout(2500);
    http.setTimeout(3000);
    http.begin("https://docs.codm.de/tools/releases.php");
    int httpCode = http.GET();

    String browser_download_url = "";

    if (httpCode > 0)
    {
        String payload = http.getString();

        DynamicJsonDocument doc(8192);
        deserializeJson(doc, payload);
        JsonArray releases = doc.as<JsonArray>();

        if (releases.size() > 0 && releases[0]["assets"].size() > 1)
        {
            browser_download_url = releases[0]["assets"][1]["browser_download_url"].as<String>();
            // LOGD("browser_download_url: %s", browser_download_url.c_str());
        }
    }
    else
    {
        LOGD("Error on HTTP request");
    }

    http.end();
    return browser_download_url;
}

String fetchLatestZbFw(bool refreshDns)
{
    if(refreshDns) checkDNS();
    HTTPClient http;
    http.setConnectTimeout(2500);
    http.setTimeout(3000);

    http.begin("https://raw.githubusercontent.com/codm/CZC/refs/heads/zb_fws/ti/manifest.json");
    int httpCode = http.GET();

    String browser_download_url = "";

    if (httpCode > 0)
    {
        String payload = http.getString();

        DynamicJsonDocument doc(8192 * 2);
        deserializeJson(doc, payload);

        size_t usedMemory = doc.memoryUsage();
        LOGD("doc used: %s bytes", String(usedMemory));

        String roleKey;
        if (systemCfg.zbRole == COORDINATOR)
        {
            roleKey = "coordinator";
        }
        else if (systemCfg.zbRole == ROUTER)
        {
            roleKey = "router";
        }
        else if (systemCfg.zbRole == OPENTHREAD)
        {
            roleKey = "thread";
        }

        if (doc.containsKey(roleKey) && doc[roleKey].containsKey(CCTool.chip.hwRev))
        {
            JsonObject roleObj = doc[roleKey][CCTool.chip.hwRev].as<JsonObject>();
            for (JsonPair kv : roleObj)
            {
                JsonObject file = kv.value().as<JsonObject>();
                if (file.containsKey("link"))
                {
                    browser_download_url = file["link"].as<String>();
                    if (file.containsKey("baud"))
                    {
                        browser_download_url = browser_download_url + "?b=" + file["baud"].as<int>();
                    }
                    break;
                }
            }
            // LOGD("browser_download_url: %s", browser_download_url.c_str());
        }
    }
    else
    {
        LOGD("Error on HTTP request");
    }

    http.end();
    return browser_download_url;
}

String extractVersionFromURL(String url)
{
    int startPos = url.lastIndexOf('_') + 1;
    int endPos = url.indexOf(".ota.bin");
    if (endPos == -1)
    {
        endPos = url.indexOf(".bin");
    }

    if (startPos != -1 && endPos != -1)
    {
        return url.substring(startPos, endPos);
    }
    return "";
}
