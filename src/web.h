#include <Arduino.h>
void handleEvents();
void initWebServer();
void webServerHandleClient();
void handleLoginGet();
void handleLoginPost();
void handleLogout();
bool is_authenticated();
void handleGeneral();
void handleSecurity();
void handleRoot();
void handleNetwork();
void handleMqtt();
void handleZigbeeBSL();
void handleZigbeeRestart();
void handleSerial();
void handleSavefile();
void handleApi();
void handleUpdateRequest();
void handleEspUpdateUpload();
//void handleZbUpdateUpload();
void handleNotFound();
bool captivePortal();

void sendGzip(const char* contentType, const uint8_t content[], uint16_t contentLen);
void handleTools();
void printLogTime();
void printLogMsg(String msg);
void handleSaveParams();

String getRootData(bool update = false);

void sendEventSafe(const char *event, const String data);
void progressFunc(unsigned int progress, unsigned int total);
void espUpdateLoop();

bool getEspUpdate(String esp_fw_url);
String fetchLatestEspFw(bool refreshDns = true);
String fetchLatestZbFw(bool refreshDns = true);
String extractVersionFromURL(String url);

void updateWebTask(void *parameter);

void changeZbMode(String fwMode);

enum API_PAGE_t : uint8_t
{
    API_PAGE_ROOT,
    API_PAGE_GENERAL,
//    API_PAGE_ETHERNET,
    API_PAGE_NETWORK,
    API_PAGE_ZIGBEE,
    API_PAGE_SECURITY,
    API_PAGE_TOOLS,
    API_PAGE_ABOUT,
    API_PAGE_MQTT,

};
