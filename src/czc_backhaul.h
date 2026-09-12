#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>

void backhaulLoad();
void backhaulBegin();
void backhaulLoop();
bool backhaulEnabled();
bool backhaulConfigured();
bool backhaulAuthorizeWebMutation();
void backhaulWebRoutes(WebServer &server);
bool backhaulStatus(JsonDocument &document);
bool backhaulMaintenanceBegin();
void backhaulMaintenanceEnd();
void backhaulBslHold(bool hold);
void backhaulRoleChanged();

class BackhaulMaintenance {
    bool acquired;
public:
    BackhaulMaintenance():acquired(backhaulMaintenanceBegin()) {}
    ~BackhaulMaintenance() { if(acquired) backhaulMaintenanceEnd(); }
    explicit operator bool() const { return acquired; }
    BackhaulMaintenance(const BackhaulMaintenance &)=delete;
};
