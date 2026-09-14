/* Unmodified callbacks extracted from sealed 0.8.13 modem.c.
 * Source SHA256: 36fcdce30e8e437c655e15ae2d75726a4883186bbbd28cb723342a5a20ba2955
 * Test fixture only; never linked into firmware. */
static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != IP_EVENT) return;

    switch (id) {
    case IP_EVENT_PPP_GOT_IP: {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(MODEM_TAG, "PPP got IP: " IPSTR " gw=" IPSTR " mask=" IPSTR,
                 IP2STR(&e->ip_info.ip),
                 IP2STR(&e->ip_info.gw),
                 IP2STR(&e->ip_info.netmask));
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(s_ppp_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            ESP_LOGI(MODEM_TAG, "PPP DNS main: " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }
        if (esp_netif_get_dns_info(s_ppp_netif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK) {
            ESP_LOGI(MODEM_TAG, "PPP DNS backup: " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }
        s_ppp_up = true;
        xEventGroupSetBits(s_modem_evt, EVT_GOT_IP);
        break;
    }
    case IP_EVENT_PPP_LOST_IP:
        ESP_LOGW(MODEM_TAG, "PPP lost IP");
        s_ppp_up = false;
        xEventGroupSetBits(s_modem_evt, EVT_LOST_IP);
        break;
    default:
        break;
    }
}

static void on_netif_ppp_status(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base != NETIF_PPP_STATUS) return;
    /* PHASE_DEAD = link broken */
    if (id == NETIF_PPP_ERRORUSER) {
        ESP_LOGW(MODEM_TAG, "PPP error from user / disconnect");
        xEventGroupSetBits(s_modem_evt, EVT_PPP_FAIL);
    }
}
