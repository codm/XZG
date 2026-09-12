#pragma once
#include <stdint.h>
struct esp_ip4_addr_t { uint32_t addr; };
struct esp_ip6_addr_t { uint32_t addr[4]; };
struct esp_netif_ip_info_t { esp_ip4_addr_t ip; };
struct esp_netif_t { bool up; esp_netif_ip_info_t info; esp_ip6_addr_t ipv6[3]; int count; bool fail; };
enum esp_ip6_addr_type_t { ESP_IP6_ADDR_IS_GLOBAL, ESP_IP6_ADDR_IS_LINK_LOCAL };
constexpr int ESP_OK=0;
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *);
bool esp_netif_is_netif_up(esp_netif_t *);
int esp_netif_get_ip_info(esp_netif_t *,esp_netif_ip_info_t *);
int esp_netif_get_all_ip6(esp_netif_t *,esp_ip6_addr_t *);
int esp_netif_create_ip6_linklocal(esp_netif_t *);
char *esp_ip4addr_ntoa(const esp_ip4_addr_t *,char *,int);
esp_ip6_addr_type_t esp_netif_ip6_get_addr_type(esp_ip6_addr_t *);
