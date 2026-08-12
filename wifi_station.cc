#include "wifi_station.h"
#include <cstring>
#include <algorithm>
#include <utility>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs.h>
#include "nvs_flash.h"
#include <esp_netif.h>
#include <esp_system.h>
#include "ssid_manager.h"

#define TAG "WifiStation"
#define WIFI_EVENT_CONNECTED BIT0
#define WIFI_EVENT_STOPPED BIT1
#define WIFI_EVENT_SCAN_DONE_BIT BIT2
#define MAX_RECONNECT_COUNT_MANUAL 5

WifiStation::WifiStation() {
    // Create the event group
    event_group_ = xEventGroupCreate();

    // 读取配置
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // No saved WiFi credentials yet; defaults are used until the first AddSsid.
        ESP_LOGI(TAG, "No saved WiFi credentials in NVS");
        max_tx_power_ = 0;
        remember_bssid_ = 0;
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        max_tx_power_ = 0;
        remember_bssid_ = 0;
    } else {
        err = nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
        if (err != ESP_OK) {
            max_tx_power_ = 0;
        }
        err = nvs_get_u8(nvs, "remember_bssid", &remember_bssid_);
        if (err != ESP_OK) {
            remember_bssid_ = 0;
        }
        nvs_close(nvs);
    }
}

WifiStation::~WifiStation() {
    Stop();
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void WifiStation::AddAuth(const std::string &&ssid, const std::string &&password) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
}

void WifiStation::Stop() {
    ESP_LOGI(TAG, "Stopping WiFi station");
    
    // Unregister event handlers FIRST to prevent scan done from triggering connect
    if (instance_any_id_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }

    // Stop timer
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
    }

    // Now safe to stop scan, disconnect and stop WiFi (no event callbacks will fire)
    esp_wifi_scan_stop();
    esp_wifi_disconnect();
    esp_wifi_stop();

    if (station_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(station_netif_);
        station_netif_ = nullptr;
    }
    
    // Reset was_connected_ flag to prevent stale state from affecting subsequent sessions
    was_connected_ = false;
    connect_after_scan_.store(false);
    scan_in_progress_.store(false);
    immediate_scan_fallback_.store(false);
    connect_queue_.clear();

    // Clear connected bit
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED);
    
    // Set stopped event AFTER cleanup is complete to unblock WaitForConnected
    // This ensures no race condition with subsequent WiFi operations
    xEventGroupSetBits(event_group_, WIFI_EVENT_STOPPED);
}

void WifiStation::OnScanBegin(std::function<void()> on_scan_begin) {
    on_scan_begin_ = on_scan_begin;
}

void WifiStation::OnScanCompleted(
    std::function<void(const std::vector<WifiAccessPoint>&)> on_scan_completed) {
    on_scan_completed_ = std::move(on_scan_completed);
}

void WifiStation::OnConnectionFailed(std::function<void()> on_connection_failed) {
    on_connection_failed_ = std::move(on_connection_failed);
}

void WifiStation::OnConnect(std::function<void(const std::string& ssid)> on_connect) {
    on_connect_ = on_connect;
}

void WifiStation::OnConnected(std::function<void(const std::string& ssid)> on_connected) {
    on_connected_ = on_connected;
}

void WifiStation::OnDisconnected(std::function<void()> on_disconnected) {
    on_disconnected_ = on_disconnected;
}

void WifiStation::Start() {
    const std::vector<SsidItem> saved_networks = SsidManager::GetInstance().GetSsidList();
    if (!saved_networks.empty()) {
        // Try the most recently successful network first. This removes the normal scan delay.
        direct_mode_ = true;
        scan_fallback_enabled_ = true;
        immediate_scan_fallback_.store(true);
        max_reconnect_count_ = 3;
        ssid_ = saved_networks.front().ssid;
        password_ = saved_networks.front().password;
    } else {
        direct_mode_ = false;
        scan_fallback_enabled_ = true;
        immediate_scan_fallback_.store(false);
    }
    StartInternal();
}

void WifiStation::StartWithoutScan(const std::string& ssid, const std::string& password) {
    direct_mode_ = true;
    scan_fallback_enabled_ = false;
    immediate_scan_fallback_.store(false);
    max_reconnect_count_ = MAX_RECONNECT_COUNT_MANUAL;
    ssid_ = ssid;
    password_ = password;
    StartInternal();
}

bool WifiStation::ScanAccessPoints() {
    return StartScan(false, false);
}

std::vector<WifiAccessPoint> WifiStation::GetAccessPoints() const {
    std::lock_guard<std::mutex> lock(access_points_mutex_);
    return access_points_;
}

void WifiStation::StartInternal() {
    // Note: esp_netif_init() and esp_wifi_init() should be called once before calling this method
    // WiFi driver is initialized by WifiManager::Initialize() and kept alive
    
    // Clear stopped event bit so WaitForConnected works properly
    // Clear scan done bit so Stop() can wait for scan to complete
    xEventGroupClearBits(event_group_, WIFI_EVENT_STOPPED | WIFI_EVENT_SCAN_DONE_BIT);
    
    // Create the default WiFi station interface
    station_netif_ = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WifiStation::WifiEventHandler,
                                                        this,
                                                        &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WifiStation::IpEventHandler,
                                                        this,
                                                        &instance_got_ip_));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (max_tx_power_ != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
    }

    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            static_cast<WifiStation*>(arg)->StartScan(true, true);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiScanTimer",
        .skip_unhandled_events = true
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
}

bool WifiStation::StartScan(bool connect_after_scan, bool notify_started) {
    bool expected = false;
    if (!scan_in_progress_.compare_exchange_strong(expected, true)) {
        // Manual refreshes can arrive while an automatic scan is still running.
        // The in-flight scan will publish the same fresh AP list, so treat this as accepted.
        return true;
    }
    connect_after_scan_.store(connect_after_scan);
    const esp_err_t result = esp_wifi_scan_start(nullptr, false);
    if (result != ESP_OK) {
        scan_in_progress_.store(false);
        connect_after_scan_.store(false);
        ESP_LOGW(TAG, "Start scan failed: %s", esp_err_to_name(result));
        return false;
    }
    if (notify_started && on_scan_begin_) {
        on_scan_begin_();
    }
    return true;
}

void WifiStation::ScheduleNextScan() {
    if (timer_handle_ == nullptr) {
        return;
    }
    const esp_err_t result = esp_timer_start_once(timer_handle_, scan_current_interval_microseconds_);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Schedule scan failed: %s", esp_err_to_name(result));
        return;
    }
    UpdateScanInterval();
}

bool WifiStation::WaitForConnected(int timeout_ms) {
    // Wait for either connected or stopped event
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_STOPPED, 
                                    pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);
    // Return true only if connected (not if stopped)
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

void WifiStation::HandleScanResult() {
    scan_in_progress_.store(false);
    const bool connect_after_scan = connect_after_scan_.exchange(false);
    uint16_t ap_num = 0;
    if (esp_wifi_scan_get_ap_num(&ap_num) != ESP_OK) {
        ap_num = 0;
    }
    std::vector<wifi_ap_record_t> ap_records(ap_num);
    if (ap_num > 0 && esp_wifi_scan_get_ap_records(&ap_num, ap_records.data()) != ESP_OK) {
        ap_records.clear();
        ap_num = 0;
    }
    // sort by rssi descending
    std::sort(ap_records.begin(), ap_records.end(), [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        return a.rssi > b.rssi;
    });

    std::vector<WifiAccessPoint> access_points;
    for (const wifi_ap_record_t& ap_record : ap_records) {
        const char* ssid = reinterpret_cast<const char*>(ap_record.ssid);
        const std::string name(ssid, strnlen(ssid, sizeof(ap_record.ssid)));
        if (name.empty() || std::any_of(access_points.begin(), access_points.end(),
                                        [&name](const WifiAccessPoint& item) {
                                            return item.ssid == name;
                                        })) {
            continue;
        }
        access_points.push_back({name, ap_record.rssi, ap_record.authmode});
    }
    {
        std::lock_guard<std::mutex> lock(access_points_mutex_);
        access_points_ = access_points;
    }
    if (on_scan_completed_) {
        on_scan_completed_(access_points);
    }
    if (!connect_after_scan) {
        return;
    }

    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    connect_queue_.clear();
    for (const wifi_ap_record_t& ap_record : ap_records) {
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [ap_record](const SsidItem& item) {
            return strcmp((char *)ap_record.ssid, item.ssid.c_str()) == 0;
        });
        if (it != ssid_list.end()) {
            ESP_LOGI(TAG, "Found AP: %s, BSSID: %02x:%02x:%02x:%02x:%02x:%02x, RSSI: %d, Channel: %d, Authmode: %d",
                (char *)ap_record.ssid, 
                ap_record.bssid[0], ap_record.bssid[1], ap_record.bssid[2],
                ap_record.bssid[3], ap_record.bssid[4], ap_record.bssid[5],
                ap_record.rssi, ap_record.primary, ap_record.authmode);
            WifiApRecord record = {
                .ssid = it->ssid,
                .password = it->password,
                .channel = ap_record.primary,
                .authmode = ap_record.authmode,
                .bssid = {0}
            };
            memcpy(record.bssid, ap_record.bssid, 6);
            connect_queue_.push_back(record);
        }
    }

    if (connect_queue_.empty()) {
        ESP_LOGI(TAG, "No AP found, next scan in %d seconds", scan_current_interval_microseconds_ / 1000 / 1000);
        ScheduleNextScan();
        return;
    }

    StartConnect();
}

void WifiStation::StartConnect() {
    auto ap_record = connect_queue_.front();
    connect_queue_.erase(connect_queue_.begin());
    ssid_ = ap_record.ssid;
    password_ = ap_record.password;

    if (on_connect_) {
        on_connect_(ssid_);
    }

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));
    strcpy((char *)wifi_config.sta.ssid, ap_record.ssid.c_str());
    strcpy((char *)wifi_config.sta.password, ap_record.password.c_str());
    if (remember_bssid_) {
        wifi_config.sta.channel = ap_record.channel;
        memcpy(wifi_config.sta.bssid, ap_record.bssid, 6);
        wifi_config.sta.bssid_set = true;
    }
    wifi_config.sta.listen_interval = 10;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    reconnect_count_ = 0;
    ESP_ERROR_CHECK(esp_wifi_connect());
}

int8_t WifiStation::GetRssi() {
    // Check if connected first
    if (!IsConnected()) {
        return 0;  // Return 0 if not connected
    }
    
    // Get station info
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get AP info: %s", esp_err_to_name(err));
        return 0;
    }
    return ap_info.rssi;
}

uint8_t WifiStation::GetChannel() {
    // Check if connected first
    if (!IsConnected()) {
        return 0;  // Return 0 if not connected
    }
    
    // Get station info
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get AP info: %s", esp_err_to_name(err));
        return 0;
    }
    return ap_info.primary;
}

bool WifiStation::IsConnected() {
    return xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED;
}

void WifiStation::SetScanIntervalRange(int min_interval_seconds, int max_interval_seconds) {
    scan_min_interval_microseconds_ = min_interval_seconds * 1000 * 1000;
    scan_max_interval_microseconds_ = max_interval_seconds * 1000 * 1000;
    scan_current_interval_microseconds_ = scan_min_interval_microseconds_;
}

void WifiStation::SetPowerSaveLevel(WifiPowerSaveLevel level) {
    wifi_ps_type_t ps_type;
    switch (level) {
        case WifiPowerSaveLevel::LOW_POWER:
            ps_type = WIFI_PS_MAX_MODEM;  // Maximum power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: LOW_POWER (MAX_MODEM)");
            break;
        case WifiPowerSaveLevel::BALANCED:
            ps_type = WIFI_PS_MIN_MODEM;  // Minimum power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: BALANCED (MIN_MODEM)");
            break;
        case WifiPowerSaveLevel::PERFORMANCE:
        default:
            ps_type = WIFI_PS_NONE;       // No power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: PERFORMANCE (NONE)");
            break;
    }
    ESP_ERROR_CHECK(esp_wifi_set_ps(ps_type));
}

void WifiStation::UpdateScanInterval() {
    // Apply exponential backoff: double the interval, up to max
    if (scan_current_interval_microseconds_ < scan_max_interval_microseconds_) {
        scan_current_interval_microseconds_ *= 2;
        if (scan_current_interval_microseconds_ > scan_max_interval_microseconds_) {
            scan_current_interval_microseconds_ = scan_max_interval_microseconds_;
        }
    }
}

// Static event handler functions
void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    if (event_id == WIFI_EVENT_STA_START) {
        if (this_->direct_mode_) {
            // 直连模式：不主动扫描，直接根据 ssid_/password_ 连接
            wifi_config_t wifi_config;
            bzero(&wifi_config, sizeof(wifi_config));
            // 防止越界，使用 strncpy 并保证以 '\0' 结尾
            strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid),
                    this_->ssid_.c_str(),
                    sizeof(wifi_config.sta.ssid) - 1);
            strncpy(reinterpret_cast<char*>(wifi_config.sta.password),
                    this_->password_.c_str(),
                    sizeof(wifi_config.sta.password) - 1);
            wifi_config.sta.listen_interval = 10;
            // 直连模式下没有扫描得到的 BSSID/信道信息，仅依赖 SSID
            wifi_config.sta.bssid_set = false;

            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
            ESP_ERROR_CHECK(esp_wifi_connect());

            if (this_->on_connect_) {
                this_->on_connect_(this_->ssid_);
            }
        } else {
            this_->StartScan(true, true);
        }
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(this_->event_group_, WIFI_EVENT_SCAN_DONE_BIT);
        this_->HandleScanResult();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);
        
        // Notify disconnected callback only once when transitioning from connected to disconnected
        bool was_connected = this_->was_connected_;
        this_->was_connected_ = false;
        if (was_connected && this_->on_disconnected_) {
            ESP_LOGI(TAG, "WiFi disconnected, notifying callback");
            this_->on_disconnected_();
        }
        
        if (this_->reconnect_count_ < this_->max_reconnect_count_) {
            if (this_->on_connect_) {
                this_->on_connect_(this_->ssid_);
            }
            esp_wifi_connect();
            this_->reconnect_count_++;
            ESP_LOGI(TAG, "Reconnecting %s (attempt %d / %d)", this_->ssid_.c_str(), this_->reconnect_count_, this_->max_reconnect_count_);
            return;
        }

        if (!this_->connect_queue_.empty()) {
            this_->StartConnect();
            return;
        }

        if (this_->direct_mode_) {
            if (!this_->scan_fallback_enabled_) {
                if (this_->on_connection_failed_) {
                    this_->on_connection_failed_();
                }
                return;
            }
            this_->direct_mode_ = false;
            this_->reconnect_count_ = 0;
            this_->immediate_scan_fallback_.store(false);
            ESP_LOGI(TAG, "Direct connection failed, scanning saved networks");
            this_->StartScan(true, true);
            return;
        }

        if (this_->immediate_scan_fallback_.exchange(false)) {
            ESP_LOGI(TAG, "Reconnect failed, scanning saved networks");
            this_->StartScan(true, true);
            return;
        }

        ESP_LOGI(TAG, "No saved AP found, retry scan in %d seconds",
                 this_->scan_current_interval_microseconds_ / 1000 / 1000);
        this_->ScheduleNextScan();
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);

    char ip_address[16];
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
    this_->ip_address_ = ip_address;
    ESP_LOGI(TAG, "Got IP: %s", this_->ip_address_.c_str());
    
    xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);
    this_->was_connected_ = true;  // Mark as connected for disconnect notification
    if (this_->on_connected_) {
        this_->on_connected_(this_->ssid_);
    }
    const std::vector<SsidItem> saved_networks = SsidManager::GetInstance().GetSsidList();
    const auto saved_network = std::find_if(saved_networks.begin(), saved_networks.end(),
                                            [this_](const SsidItem& item) {
                                                return item.ssid == this_->ssid_;
                                            });
    if (saved_network != saved_networks.end()) {
        SsidManager::GetInstance().SetDefaultSsid(
            static_cast<int>(std::distance(saved_networks.begin(), saved_network)));
    }
    // Future disconnects start with a direct retry, then scan every saved network if needed.
    this_->direct_mode_ = false;
    this_->scan_fallback_enabled_ = true;
    this_->immediate_scan_fallback_.store(true);
    this_->connect_queue_.clear();
    this_->reconnect_count_ = 0;
    
    // Reset scan interval to minimum for fast reconnect if disconnected later
    this_->scan_current_interval_microseconds_ = this_->scan_min_interval_microseconds_;
}
