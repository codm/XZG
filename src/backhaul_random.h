#pragma once
#include <Arduino.h>
#include <bootloader_random.h>
#include <esp_system.h>
#include <mbedtls/ctr_drbg.h>
#include <freertos/semphr.h>

// Seed before Wi-Fi/ADC initialization; immediately release the SAR entropy source.
// TLS then uses a locked DRBG, so the existing Wi-Fi, temperature and VPN functions
// may keep their normal hardware ownership. Never reseed from an inactive RF RNG.
namespace czc_random {
static mbedtls_ctr_drbg_context drbg;
static SemaphoreHandle_t mutex=nullptr;
static bool ready=false;
struct Seed { uint8_t bytes[96]; size_t used=0; bool available=true; };
static Seed seed;
static int entropy(void *arg,unsigned char *out,size_t n) {
    Seed &s=*static_cast<Seed *>(arg);
    if(!s.available || s.used+n>sizeof(s.bytes)) return -1;
    memcpy(out,s.bytes+s.used,n); s.used+=n; return 0;
}
static void begin() {
    mutex=xSemaphoreCreateMutex();
    bootloader_random_enable(); esp_fill_random(seed.bytes,sizeof(seed.bytes)); bootloader_random_disable();
    mbedtls_ctr_drbg_init(&drbg);
    const unsigned char personal[]="CZC full firmware peer TLS v1";
    ready=mutex && mbedtls_ctr_drbg_seed(&drbg,entropy,&seed,personal,sizeof(personal)-1)==0;
    seed.available=false; memset(seed.bytes,0,sizeof(seed.bytes));
    // One million requests, then fail closed until reboot rather than silently
    // reseeding with low-entropy bytes after Wi-Fi has been disabled.
    mbedtls_ctr_drbg_set_reseed_interval(&drbg,1000000);
}
static int fill(void *,unsigned char *out,size_t n) {
    if(!ready || xSemaphoreTake(mutex,pdMS_TO_TICKS(1000))!=pdTRUE) return -1;
    int result=mbedtls_ctr_drbg_random(&drbg,out,n); xSemaphoreGive(mutex); return result;
}
}
