#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <FS.h>
#include <LittleFS.h>
#include <ETH.h>
#include <CCTools.h>
#include <esp_task_wdt.h>
#include <CronAlarms.h>
#include <freertos/queue.h>
#include <new>
#include <esp_netif.h>
#include "memory_status.h"
#include "network_ipv4.h"
// #include <Husarnet.h> //not available now

#include "config.h"
#include "czc_backhaul.h"
#include "web.h"
#include "log.h"
#include "etc.h"
#include "const/zones.h"
// #include "const/hw.h"
#include "zb.h"

#include <WireGuard-ESP32.h>
static WireGuard wg;

extern HwBrdConfigStruct brdConfigs[BOARD_CFG_CNT];
extern HwEthConfig ethConfigs[ETH_CFG_CNT];
extern HwZbConfig zbConfigs[ZB_CFG_CNT];
extern HwMistConfig mistConfigs[MIST_CFG_CNT];

extern struct HwConfigStruct hwConfig;

extern struct SystemConfigStruct systemCfg;
extern struct NetworkConfigStruct networkCfg;
extern struct VpnConfigStruct vpnCfg;
extern struct MqttConfigStruct mqttCfg;

extern struct SysVarsStruct vars;

extern LEDControl ledControl;

extern CCTools CCTool;

const char *coordMode = "coordMode"; // coordMode node name ?? not name but text field with mode
// const char *prevCoordMode = "prevCoordMode"; // prevCoordMode node name ?? not name but text field with mode

const char *configFileSystem = "/config/system.json";
const char *configFileWifi = "/config/configWifi.json";
const char *configFileEther = "/config/configEther.json";
const char *configFileGeneral = "/config/configGeneral.json";
const char *configFileSecurity = "/config/configSecurity.json";
const char *configFileSerial = "/config/configSerial.json";
const char *configFileMqtt = "/config/configMqtt.json";
const char *configFileWg = "/config/configWg.json";
const char *configFileHw = "/configHw.json";

#include "mbedtls/md.h"

String sha1(String payloadStr)
{
  const char *payload = payloadStr.c_str();

  int size = 20;

  byte shaResult[size];

  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA1;

  const size_t payloadLength = strlen(payload);

  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char *)payload, payloadLength);
  mbedtls_md_finish(&ctx, shaResult);
  mbedtls_md_free(&ctx);

  String hashStr = "";

  for (uint16_t i = 0; i < size; i++)
  {
    String hex = String(shaResult[i], HEX);
    if (hex.length() < 2)
    {
      hex = "0" + hex;
    }
    hashStr += hex;
  }

  return hashStr;
}

#include <OneWire.h>
#include <DallasTemperature.h>

OneWire *oneWire = nullptr;
DallasTemperature *sensor = nullptr;

int check1wire()
{
  int pin = -1;
  vars.oneWireIs = false;
  if (hwConfig.eth.mdcPin != 33 && hwConfig.eth.mdiPin != 33 && hwConfig.eth.pwrPin != 33)
  {
    if (hwConfig.zb.rxPin != 33 && hwConfig.zb.txPin != 33 && hwConfig.zb.bslPin != 33 && hwConfig.zb.rstPin != 33)
    {
      if (hwConfig.mist.btnPin != 33 && hwConfig.mist.uartSelPin != 33 && hwConfig.mist.ledModePin != 33 && hwConfig.mist.ledPwrPin != 33)
      {
        pin = 33;
        vars.oneWireIs = true;
      }
    }
  }
  return pin;
}

void setup1wire(int pin)
{
  if (oneWire != nullptr)
  {
    delete oneWire;
  }
  if (sensor != nullptr)
  {
    delete sensor;
  }

  oneWire = new OneWire(pin);
  sensor = new DallasTemperature(oneWire);

  sensor->begin();
  get1wire();
}

float get1wire()
{
  if (sensor == nullptr)
  {
    LOGW("1w not init");
    return -127.0;
  }
  if (millis() - vars.last1wAsk > 5000)
  {
    sensor->requestTemperatures();
    vars.temp1w = sensor->getTempCByIndex(0);
    LOGD("Temp is %f", vars.temp1w);
    vars.last1wAsk = millis();
  }
  if (vars.temp1w == -127)
  {
    vars.oneWireIs = false;
  }
  return vars.temp1w;
}

void getReadableTime(String &readableTime, unsigned long beginTime)
{
  unsigned long currentMillis;
  unsigned long seconds;
  unsigned long minutes;
  unsigned long hours;
  unsigned long days;
  currentMillis = millis() - beginTime;
  seconds = currentMillis / 1000;
  minutes = seconds / 60;
  hours = minutes / 60;
  days = hours / 24;
  currentMillis %= 1000;
  seconds %= 60;
  minutes %= 60;
  hours %= 24;

  readableTime = String(days) + " d ";

  if (hours < 10)
  {
    readableTime += "0";
  }
  readableTime += String(hours) + ":";

  if (minutes < 10)
  {
    readableTime += "0";
  }
  readableTime += String(minutes) + ":";

  if (seconds < 10)
  {
    readableTime += "0";
  }
  readableTime += String(seconds) + "";
}

float readTemperature(bool clear)
{
  if (clear)
  {
    return (temprature_sens_read() - 32) / 1.8;
  }
  else
  {
    return (temprature_sens_read() - 32) / 1.8 - systemCfg.tempOffset;
  }
}

float getCPUtemp(bool clear)
{
  float CPUtemp = 0.0;
  if (WiFi.getMode() == WIFI_MODE_NULL || WiFi.getMode() == WIFI_OFF)
  {
    WiFi.mode(WIFI_STA); // enable wifi to enable temp sensor
    CPUtemp = readTemperature(clear);
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF); // disable wifi
  }
  else
  {
    CPUtemp = readTemperature(clear);
  }
  return CPUtemp;
}

void zigbeeRouterRejoin()
{
  BackhaulMaintenance maintenance;
  if (!maintenance) { printLogMsg("[ZB] UART busy; operation cancelled"); return; }
  printLogMsg("Router rejoin begin");
  CCTool.routerRejoin();
  printLogMsg("Router in join mode!");
}

void zigbeeEnableBSL()
{
  BackhaulMaintenance maintenance;
  if (!maintenance) { printLogMsg("[ZB] UART busy; operation cancelled"); return; }
  printLogMsg("ZB enable BSL");
  CCTool.enterBSL();
  backhaulBslHold(true);
  printLogMsg("Now you can flash CC2652!");
  if (systemCfg.workMode == WORK_MODE_USB)
  {
    Serial.updateBaudRate(500000);
    Serial2.updateBaudRate(500000);
  }
}

void zigbeeRestart()
{
  BackhaulMaintenance maintenance;
  if (!maintenance) { printLogMsg("[ZB] UART busy; operation cancelled"); return; }
  printLogMsg("ZB RST begin");
  CCTool.restart();
  backhaulBslHold(false);
  printLogMsg("ZB restart was done");
  if (systemCfg.workMode == WORK_MODE_USB)
  {
    Serial.updateBaudRate(systemCfg.serialSpeed);
    Serial2.updateBaudRate(systemCfg.serialSpeed);
  }
}

void usbModeSet(usbMode mode)
{
  // if (vars.hwUartSelIs)
  //{
  // String modeStr = (mode == ZIGBEE) ? "ZIGBEE" : "ESP";
  bool pinValue = (mode == ZIGBEE) ? HIGH : LOW;
  // String msg = "Switched USB to " + modeStr + "";
  // printLogMsg(msg);

  if (mode == ZIGBEE)
  {
    Serial.updateBaudRate(systemCfg.serialSpeed);
  }
  else
  {
    Serial.updateBaudRate(115200);
  }
  // digitalWrite(hwConfig.mist.uartSelPin, pinValue);
  if (pinValue)
  {
    ledControl.modeLED.mode = LED_ON;
  }
  else
  {
    ledControl.modeLED.mode = LED_OFF;
  }
  //}
  // else
  //{
  //  LOGD("NO vars.hwUartSelIs");
  //}
}

void writeDefaultDeviceID(char *arr)
{
    char id_str[MAX_DEV_ID_LONG] = "CZC-";
    const size_t id_str_len = strlen(id_str);
    uint8_t mac[6] = {};
    ETH.macAddress(mac);
    snprintf(&id_str[id_str_len],
             MAX_DEV_ID_LONG - id_str_len,
             "%02X%02X",
             mac[4], mac[5]);
    memcpy(arr, id_str, MAX_DEV_ID_LONG);
}

void writeJsonToFile(const char *path, DynamicJsonDocument &doc)
{
  LOGD("Write defaults to %s", path);
  serializeJsonPretty(doc, Serial);
  File configFile = LittleFS.open(path, FILE_WRITE);
  if (!configFile)
  {
    LOGD("Failed Write");
    if (LittleFS.mkdir(path))
    {
      LOGD("Config dir created");
      delay(500);
      ESP.restart();
    }
    else
    {
      LOGD("mkdir failed");
    }
    // return false;
  }
  else
  {
    serializeJsonPretty(doc, configFile);
  }
  configFile.close();
}

void factoryReset()
{

  LOGD("start");

  ledControl.powerLED.mode = LED_FLASH_3Hz;
  ledControl.modeLED.mode = LED_FLASH_3Hz;

  for (uint8_t i = 0; i < TIMEOUT_FACTORY_RESET; i++)
  {
    LOGD("%d, sec", TIMEOUT_FACTORY_RESET - i);
    delay(1000);
  }

  LittleFS.format();
  if (!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED, "/lfs2", 10)) // change to format anyway
  {
    LOGD("Error with LITTLEFS");
  }

  LittleFS.remove(configFileSerial);
  LittleFS.remove(configFileSecurity);
  LittleFS.remove(configFileGeneral);
  LittleFS.remove(configFileEther);
  LittleFS.remove(configFileWifi);
  LittleFS.remove(configFileSystem);
  LittleFS.remove(configFileWg);
  LittleFS.remove(configFileHw);
  LittleFS.remove(configFileMqtt);
  LOGD("FS Done");
  eraseNVS();
  LOGD("NVS Done");

  ledControl.powerLED.mode = LED_OFF;
  ledControl.modeLED.mode = LED_OFF;
  delay(500);
  ESP.restart();
}

void setClock(void *pvParameters)
{
  checkDNS();
  configTime(0, 0, systemCfg.ntpServ1, systemCfg.ntpServ2);

  const time_t targetTime = 946684800; // 946684800 - is 01.01.2000 in timestamp

  LOGD("Waiting for NTP time sync");
  unsigned long startTryingTime = millis();

  time_t nowSecs = time(nullptr);

  // over 01.01.2000 or longer than 5 minutes
  while ((nowSecs < targetTime) && ((millis() - startTryingTime) < 300000))
  {
    delay(500);
    yield();
    nowSecs = time(nullptr);
  }

  struct tm timeinfo;
  if (localtime_r(&nowSecs, &timeinfo))
  {
    // LOGD("Current GMT time: %s", String(asctime(&timeinfo)).c_str());

    char *zoneToFind = const_cast<char *>(NTP_TIME_ZONE);
    if (systemCfg.timeZone)
    {
      zoneToFind = systemCfg.timeZone;
    }
    const char *gmtOffset = getGmtOffsetForZone(zoneToFind);

    String timezone = "EET-2EEST,M3.5.0/3,M10.5.0/4";

    if (gmtOffset != nullptr)
    {
      LOGD("GMT Offset for %s is %s", zoneToFind, gmtOffset);
      timezone = gmtOffset;
      setTimezone(timezone);
    }
    else
    {
      LOGD("GMT Offset for %s not found.", zoneToFind);
    }

    setupCron();
  }
  else
  {
    LOGD("Failed to get time from NTP server.");
  }
  vTaskDelete(NULL);
}

void setLedsDisable(bool all)
{
  if (vars.hwLedPwrIs || vars.hwLedUsbIs)
  {
    LOGD("setLedsDisable", "%s", String(all));
    if (all)
    {
      ledControl.powerLED.active = false;
      ledControl.modeLED.active = false;
    }
    else
    {
      ledControl.powerLED.active = !systemCfg.disableLedPwr;
      ledControl.modeLED.active = !systemCfg.disableLedUSB;
    }
  }
}

void nmActivate()
{
  LOGD("start");
  setLedsDisable(true);
}

void nmDeactivate()
{
  LOGD("end");
  setLedsDisable();
}

// DNS repair must not call WiFi/ETH.config: those also stop DHCP and set IP.
static bool restoreNetworkDns(const char *key, IPAddress address)
{
  esp_netif_t *netif=esp_netif_get_handle_from_ifkey(key);
  if (!netif || !esp_netif_is_netif_up(netif) || !ipv4Assigned(uint32_t(address))) return false;
  esp_netif_dns_info_t dns{};
  dns.ip.type=ESP_IPADDR_TYPE_V4;
  dns.ip.u_addr.ip4.addr=uint32_t(address);
  return esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns)==ESP_OK;
}

bool checkDNS(bool setup)
{
  auto check=[&](const char *key, const char *label, bool connected, IPAddress current, IPAddress &saved) {
    if (!connected || current==saved) return;
    const char *action=nullptr;
    if (ipv4Assigned(uint32_t(current))) {
      // A new valid DHCP resolver wins over the cached one, including when
      // Ethernet and Wi-Fi share the SDK's DNS table. Do not rewrite it.
      if (setup) { saved=current; action="Saved"; }
    } else if (ipv4Assigned(uint32_t(saved)) && restoreNetworkDns(key,saved)) {
      action="Restored";
    }
    if (action) {
      char buffer[100];
      snprintf(buffer,sizeof(buffer),"[DNS] %s %s - %s",action,label,saved.toString().c_str());
      printLogMsg(buffer);
    }
  };
  if (networkCfg.wifiEnable)
    check("WIFI_STA_DEF","WiFi",WiFi.status()==WL_CONNECTED,WiFi.dnsIP(),vars.savedWifiDNS);
  if (networkCfg.ethEnable)
    check("ETH_DEF","ETH",ETH.linkUp(),ETH.dnsIP(),vars.savedEthDNS);
  return true;
}

/*void reCheckDNS()
{
  checkDNS();
}*/

void setupCron()
{
  // Cron.create(const_cast<char *>("30 */1 * * * *"), reCheckDNS, false);

  // const String time = systemCfg.updCheckTime;
  static char formattedTime[16];
  int seconds, hours, minutes;

  String wday = systemCfg.updCheckDay;

  seconds = random(1, 59);

  // char timeArray[6];
  // String(systemCfg.updCheckTime).toCharArray(timeArray, sizeof(timeArray));

  sscanf(systemCfg.updCheckTime, "%d:%d", &hours, &minutes);

  snprintf(formattedTime, sizeof(formattedTime), "%d %d %d * * %s", seconds, minutes, hours, wday);

  // LOGD("UPD cron %s", String(formattedTime));
  printLogMsg("[UPD_CHK] cron " + String(formattedTime));

  Cron.create(const_cast<char *>(formattedTime), checkUpdateAvail, false); // 0 0 */6 * * *

  if (systemCfg.nmEnable)
  {

    time_t nowSecs = time(nullptr);
    struct tm timeinfo;
    localtime_r(&nowSecs, &timeinfo);
    int currentTimeInMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;

    int sTargetHour, sTargetMinute, startTimeInMinutes;
    int eTargetHour, eTargetMinute, endTimeInMinutes;

    sscanf(systemCfg.nmStart, "%d:%d", &sTargetHour, &sTargetMinute);
    startTimeInMinutes = sTargetHour * 60 + sTargetMinute;

    sscanf(systemCfg.nmEnd, "%d:%d", &eTargetHour, &eTargetMinute);
    endTimeInMinutes = eTargetHour * 60 + eTargetMinute;

    if (startTimeInMinutes <= endTimeInMinutes)
    {
      if (currentTimeInMinutes >= startTimeInMinutes && currentTimeInMinutes < endTimeInMinutes)
      {
        nmActivate();
      }
      else
      {
        nmDeactivate();
      }
    }
    else
    {
      if (currentTimeInMinutes >= startTimeInMinutes || currentTimeInMinutes < endTimeInMinutes)
      {
        nmActivate();
      }
      else
      {
        nmDeactivate();
      }
    }

    char startCron[30];
    char endCron[30];
    strcpy(startCron, convertTimeToCron(String(systemCfg.nmStart)));
    strcpy(endCron, convertTimeToCron(String(systemCfg.nmEnd)));
    LOGD("cron", "NM start %s", startCron);
    LOGD("cron", "NM end %s", endCron);

    Cron.create(const_cast<char *>(startCron), nmActivate, false);
    Cron.create(const_cast<char *>(endCron), nmDeactivate, false);
  }
}

void setTimezone(String timezone)
{
  // LOGD("Setting Timezone");
  setenv("TZ", timezone.c_str(), 1); //  Now adjust the TZ.  Clock settings are adjusted to show the new local time
  tzset();
  time_t nowSecs = time(nullptr);
  struct tm timeinfo;
  localtime_r(&nowSecs, &timeinfo);

  String timeNow = asctime(&timeinfo);
  timeNow.remove(timeNow.length() - 1);
  printLogMsg("[Time] " + timeNow);
}

const char *getGmtOffsetForZone(const char *zone)
{
  for (int i = 0; i < timeZoneCount; i++)
  {
    if (strcmp(zone, timeZones[i].zone) == 0)
    {
      // Zone found, return GMT Offset
      return timeZones[i].gmtOffset;
    }
  }
  // Zone not found
  return nullptr;
}

String getTime()
{
  time_t nowSecs = time(nullptr);
  struct tm timeinfo;
  localtime_r(&nowSecs, &timeinfo);

  String timeNow = asctime(&timeinfo);
  timeNow.remove(timeNow.length() - 1);
  return timeNow;
}

char *convertTimeToCron(const String &time)
{
  static char formattedTime[16];
  int hours, minutes;

  char timeArray[6];
  time.toCharArray(timeArray, sizeof(timeArray));

  sscanf(timeArray, "%d:%d", &hours, &minutes);

  snprintf(formattedTime, sizeof(formattedTime), "0 %d %d * * *", minutes, hours);

  return formattedTime;
}

HwConfigStruct *getBrdConfig()
{
  bool ethOk = false;
  bool btnOk = false;
  bool zbOk = false;

  static HwConfigStruct bestConfig = {};
  bestConfig.eth = ethConfigs[CZC_1_ETH_CONFIG];
  bestConfig.zb = zbConfigs[CZC_1_ZB_CONFIG];
  bestConfig.mist = mistConfigs[CZC_1_MIST_CONFIG];
  strlcpy(bestConfig.board, czc_board_name, sizeof(bestConfig.board));

  return &bestConfig;
}

void wgBegin()
{
  checkDNS();
  if (!wg.is_initialized())
  {
    const char *wg_preshared_key = nullptr;
    if (vpnCfg.wgPreSharedKey[0] != '\0')
    {
      wg_preshared_key = vpnCfg.wgPreSharedKey;
      LOGD("vpnCfg.wgPreSharedKey is used");
    }

    if (!wg.begin(
            vpnCfg.wgLocalIP,
            vpnCfg.wgLocalSubnet,
            vpnCfg.wgLocalPort,
            vpnCfg.wgLocalGateway,
            vpnCfg.wgLocalPrivKey,
            vpnCfg.wgEndAddr,
            vpnCfg.wgEndPubKey,
            vpnCfg.wgEndPort,
            vpnCfg.wgAllowedIP,
            vpnCfg.wgAllowedMask,
            vpnCfg.wgMakeDefault,
            wg_preshared_key))
    {
      printLogMsg(String("Failed to initialize WG"));
      vars.vpnWgInit = false;
    }
    else
    {
      printLogMsg(String("WG was initialized"));
      /*LOGD("%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s",
           String(vpnCfg.wgLocalIP.toString().c_str()),
           String(vpnCfg.wgLocalSubnet.toString().c_str()),
           String(vpnCfg.wgLocalPort),
           String(vpnCfg.wgLocalGateway.toString().c_str()),
           String(vpnCfg.wgLocalPrivKey).c_str(),
           String(vpnCfg.wgEndAddr).c_str(),
           String(vpnCfg.wgEndPubKey).c_str(),
           String(vpnCfg.wgEndPort),
           String(vpnCfg.wgAllowedIP.toString().c_str()),
           String(vpnCfg.wgAllowedMask.toString().c_str()),
           String(vpnCfg.wgMakeDefault),
           String(wg_preshared_key).c_str());*/
      vars.vpnWgInit = true;
    }
  }
}

void wgLoop()
{

  // IPAddress ip = ;

  int checkPeriod = 5; // to do vpnCfg.checkTime;
  ip_addr_t lwip_ip;

  lwip_ip.u_addr.ip4.addr = static_cast<uint32_t>(vpnCfg.wgLocalIP);

  if (wg.is_initialized())
  {
    if (vars.vpnWgCheckTime == 0)
    {
      vars.vpnWgCheckTime = millis() + 1000 * checkPeriod;
    }
    else
    {
      if (vars.vpnWgCheckTime <= millis())
      {
        uint16_t wgLocalPort = vpnCfg.wgLocalPort;
        vars.vpnWgCheckTime = millis() + 1000 * checkPeriod;
        if (wg.is_peer_up(&lwip_ip, &wgLocalPort))
        {
          vars.vpnWgPeerIp = (lwip_ip.u_addr.ip4.addr);
          if (!vars.vpnWgConnect)
          {
            LOGD("Peer with IP %s connect", vars.vpnWgPeerIp.toString().c_str());
          }
          vars.vpnWgConnect = true;
        }
        else
        {
          if (vars.vpnWgConnect)
          {
            LOGD("Peer disconnect");
          }
          vars.vpnWgPeerIp = INADDR_NONE;
          vars.vpnWgConnect = false;
        }
      }
    }
  }
  else
  {
    vars.vpnWgInit = false;
  }
}

/* //not available now
void hnBegin()
{
  Husarnet.selfHostedSetup(vpnCfg.hnDashUrl);
  Husarnet.join(vpnCfg.hnJoinCode, vpnCfg.hnHostName);
  Husarnet.start();
}
*/

void ledTask(void *parameter)
{
  LEDSettings *led = (LEDSettings *)parameter;
  TickType_t lastWakeTime = xTaskGetTickCount();
  int previousMode = LED_OFF;

  while (1)
  {
    // LOGD("%d | led %s | m %d", millis(), led->name, led->mode);
    if (led->pin == -1)
      continue;
    if (!led->active)
    {
      digitalWrite(led->pin, LOW);
      vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(500));
      continue;
    }

    switch (led->mode)
    {
    case LED_OFF:
      previousMode = led->mode;
      digitalWrite(led->pin, LOW);
      vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(500));
      break;
    case LED_ON:
      previousMode = led->mode;
      digitalWrite(led->pin, HIGH);
      vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(500));
      break;
    case LED_TOGGLE:
      if (digitalRead(led->pin) == LOW)
      {
        digitalWrite(led->pin, HIGH);
        led->mode = LED_ON;
      }
      else
      {
        digitalWrite(led->pin, LOW);
        led->mode = LED_OFF;
      }
      vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(500));
      break;
    case LED_BLINK_5T:
      for (int j = 0; j < 5; j++)
      {
        digitalWrite(led->pin, HIGH);
        vTaskDelay(pdMS_TO_TICKS(500));
        digitalWrite(led->pin, LOW);
        vTaskDelay(pdMS_TO_TICKS(500));
      }
      led->mode = static_cast<LEDMode>(previousMode);
      break;
    case LED_BLINK_1T:
      digitalWrite(led->pin, HIGH);
      vTaskDelay(pdMS_TO_TICKS(500));
      digitalWrite(led->pin, LOW);
      vTaskDelay(pdMS_TO_TICKS(500));
      led->mode = static_cast<LEDMode>(previousMode);
      break;
    case LED_BLINK_1Hz:
    case LED_FLASH_1Hz:
      previousMode = led->mode;
      digitalWrite(led->pin, (led->mode == LED_BLINK_1Hz) ? HIGH : LOW);
      vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(800));
      digitalWrite(led->pin, (led->mode == LED_BLINK_1Hz) ? LOW : HIGH);
      vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(200));
      break;
    case LED_BLINK_3Hz:
    case LED_FLASH_3Hz:
      previousMode = led->mode;
      for (int j = 0; j < 3; j++)
      {
        digitalWrite(led->pin, (led->mode == LED_BLINK_3Hz) ? HIGH : LOW);
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(267));
        digitalWrite(led->pin, (led->mode == LED_BLINK_3Hz) ? LOW : HIGH);
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(100));
      }
      break;
    }
  }
}

int compareVersions(String v1, String v2)
{
  int v1_major = v1.substring(0, v1.indexOf('.')).toInt();
  int v2_major = v2.substring(0, v2.indexOf('.')).toInt();

  if (v1_major != v2_major)
  {
    return v1_major - v2_major;
  }

  String v1_suffix = v1.substring(v1.indexOf('.') + 1);
  String v2_suffix = v2.substring(v2.indexOf('.') + 1);

  if (v1_suffix.length() == 0)
    return -1;
  if (v2_suffix.length() == 0)
    return 1;

  return v1_suffix.compareTo(v2_suffix);
}

// Only the worker performs internet I/O. Flags, logs and automatic installation
// stay on the original UI task, so unavailable DNS/HTTPS cannot stall WebServer.
struct UpdateCheckResult { String esp,zb; };
static QueueHandle_t updateCheckResults=nullptr;
static bool updateCheckRunning=false;
static bool updateCheckDeferred=false;
static uint32_t updateCheckDeferredAt=0;
static void updateCheckTask(void *arg)
{
  auto *result=static_cast<UpdateCheckResult *>(arg);
  result->esp=fetchLatestEspFw(false);
  result->zb=fetchLatestZbFw(false);
  xQueueSend(updateCheckResults,&result,portMAX_DELAY);
  vTaskDelete(nullptr);
}

void checkUpdateAvail()
{
  if(vars.apStarted || vars.zbFlashing || updateCheckRunning) return;
  auto memory=memoryBudget();
  if(!memory.backgroundHttps()) {
    if(!updateCheckDeferred) {
      char line[128];
      snprintf(line,sizeof(line),"[UPD_CHK] Deferred: heap=%lu largest=%lu; keeping RAM for UI/TLS",
               static_cast<unsigned long>(memory.free),static_cast<unsigned long>(memory.largest));
      printLogMsg(line);
    }
    updateCheckDeferred=true; updateCheckDeferredAt=millis(); return;
  }
  updateCheckDeferred=false;
  if(!updateCheckResults) updateCheckResults=xQueueCreate(1,sizeof(UpdateCheckResult *));
  if(!updateCheckResults) return;
  auto *result=new(std::nothrow) UpdateCheckResult;
  if(!result) return;
  checkDNS();
  updateCheckRunning=true;
  if(xTaskCreatePinnedToCore(updateCheckTask,"update-check",8192,result,1,nullptr,0)!=pdPASS) {
    updateCheckRunning=false; delete result;
    updateCheckDeferred=true; updateCheckDeferredAt=millis();
    memory=memoryBudget(); char line[112];
    snprintf(line,sizeof(line),"[UPD_CHK] Cannot start background check: heap=%lu largest=%lu",
             static_cast<unsigned long>(memory.free),static_cast<unsigned long>(memory.largest));
    printLogMsg(line);
  }
}

void updateCheckLoop()
{
  if(updateCheckDeferred && uint32_t(millis()-updateCheckDeferredAt)>=60000) {
    updateCheckDeferredAt=millis(); checkUpdateAvail();
  }
  if(!updateCheckResults || vars.zbFlashing) return;
  UpdateCheckResult *result=nullptr;
  if(xQueueReceive(updateCheckResults,&result,0)!=pdTRUE) return;
  updateCheckRunning=false;
  String latestReleaseUrlEsp=result->esp, latestReleaseUrlZb=result->zb;
  delete result;
  if (!vars.apStarted)
  {
    const char *ESPkey = "ESP";
    const char *ZBkey = "ZB";
    const char *FoundKey = "Found ";
    const char *NewFwKey = " new fw: ";
    const char *TryKey = "try to install";
    String latestVersionEsp = extractVersionFromURL(latestReleaseUrlEsp);
    String latestVersionZb = extractVersionFromURL(latestReleaseUrlZb);

    LOGD("%s %s", ESPkey, latestVersionEsp.c_str());
    LOGD("%s %s", ZBkey, latestVersionZb.c_str());

    String sVersion(VERSION);
    LOGD("sVersion: %s", sVersion);
    String subVersion = sVersion.substring(1);
    LOGD("SubVer: %s", subVersion);

    if (latestVersionEsp.length() > 0 && compareVersions(latestVersionEsp, subVersion) > 0)
    {
      vars.updateEspAvail = true;
      printLogMsg(String(FoundKey) + String(ESPkey) + String(NewFwKey) + latestVersionEsp);
      if (systemCfg.updAutoInst && !backhaulConfigured())
      {
        printLogMsg(String(TryKey));
        getEspUpdate(latestReleaseUrlEsp);
      }
    }
    else
    {
      vars.updateEspAvail = false;
    }

    if (CCTool.chip.fwRev > 0 && latestVersionZb.length() > 0 && compareVersions(latestVersionZb, String(CCTool.chip.fwRev)) > 0)
    {
      vars.updateZbAvail = true;
      printLogMsg(String(FoundKey) + String(ZBkey) + String(NewFwKey) + latestVersionZb);
      if (systemCfg.updAutoInst && !backhaulConfigured())
      {
        printLogMsg(String(TryKey));
        const char* zigbee_firmware_path= "/zigbee/firmware.bin";
        if(!flashZigbeefromURL(latestReleaseUrlZb.c_str(), zigbee_firmware_path, CCTool))
          DEBUG_PRINTLN("[ETC, AUTO UPDATE] Error while downloading and flashing Zigbee firmware from link");
        ESP.restart();
      }
    }
    else
    {
      vars.updateZbAvail = false;
    }
  }
}

int numOfConnectedClients() {
  return vars.connectedClients;
}
