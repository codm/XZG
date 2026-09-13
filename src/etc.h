#include <ETH.h>
#include "const/hw.h"


void getReadableTime(String &readableTime, unsigned long beginTime);

String sha1(String payloadStr);

int check1wire();
void setup1wire(int pin);
float get1wire();

extern HwBrdConfigStruct brdConfigs[BOARD_CFG_CNT];

HwConfigStruct *getBrdConfig();

float getCPUtemp(bool clear = false);

void zigbeeRouterRejoin();
void zigbeeEnableBSL();
void zigbeeRestart();

void usbModeSet(usbMode mode);

void writeDefaultDeviceID(char *arr);
void writeJsonToFile(const char *path, DynamicJsonDocument &doc);

#define TIMEOUT_FACTORY_RESET 3

void factoryReset();

void setLedsDisable(bool all = false);
void cronTest();
void nmActivate();
bool checkDNS(bool setup = false);
void setupCron();

void setClock(void *pvParameters);
void setTimezone(String timezone);
const char *getGmtOffsetForZone(const char *zone);
char *convertTimeToCron(const String &time);

void wgBegin();
void wgLoop();

void hnBegin();

void ledTask(void *parameter);
String getTime();

void checkUpdateAvail();
void updateCheckLoop();

int numOfConnectedClients();
