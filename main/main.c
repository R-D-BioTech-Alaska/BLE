#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_pm.h"
#include "driver/gpio.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"

#define TAG "BT_PM"
#define GPIO_BTN          0   // Use BOOT button on many devkits
#define BTN_ACTIVE_LEVEL  0
#define INACTIVITY_MS     15000 
#define ACTIVITY_WINDOW_MS 3000
#define ACTIVITY_BYTES_THRESH 256

static const char *BLE_DEVICE_NAME = "ESP32-LOWPOWER-HYBRID";
static const char *SPP_DEVICE_NAME = "ESP32-SPP-BOOST";

#define GATT_SVC_UUID        0xABF0
#define GATT_CHAR_RX_UUID    0xABF1  // central -> peripheral (Write)
#define GATT_CHAR_TX_UUID    0xABF2  // peripheral -> central (Notify)
#define GATT_CHAR_CTL_UUID   0xABF3  // control (Write): "BOOST_ON"/"BOOST_OFF"

static const esp_ble_power_type_t PWR_ADV  = ESP_BLE_PWR_TYPE_ADV;
static const esp_ble_power_type_t PWR_CONN = ESP_BLE_PWR_TYPE_CONN_HDL0;

static esp_ble_adv_params_t adv_params_idle = {
    .adv_int_min       = 1600,  
    .adv_int_max       = 1600,
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static esp_ble_adv_params_t adv_params_probe = {
    .adv_int_min       = 160,  
    .adv_int_max       = 160,
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static const uint16_t CONN_ITVL_MIN_ACTIVE = 6;   
static const uint16_t CONN_ITVL_MAX_ACTIVE = 12;  
static const uint16_t CONN_LATENCY_ACTIVE  = 0;
static const uint16_t CONN_TIMEOUT_ACTIVE  = 400; 
static const uint16_t CONN_ITVL_MIN_SAVE = 80;   
static const uint16_t CONN_ITVL_MAX_SAVE = 200;  
static const uint16_t CONN_LATENCY_SAVE  = 4;
static const uint16_t CONN_TIMEOUT_SAVE  = 500;  

typedef enum {
    MODE_IDLE = 0,   
    MODE_PROBE,     
    MODE_ACTIVE,     
} power_mode_t;

static power_mode_t g_mode = MODE_IDLE;
static bool g_ble_connected = false;
static uint16_t g_conn_id = 0xFFFF;
static esp_gatt_if_t g_gatts_if = 0;
static uint16_t g_service_handle = 0;
static uint16_t g_handle_char_rx = 0;
static uint16_t g_handle_char_tx = 0;
static uint16_t g_handle_char_ctl = 0;
static TimerHandle_t inactivity_timer;
static TimerHandle_t activity_window_timer;
static uint32_t window_bytes = 0;
static bool     spp_running = false;
static const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t character_declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t character_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t char_prop_read_notify = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t char_prop_write = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static esp_gatts_attr_db_t gatt_db[] = {
    [0] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&primary_service_uuid, ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(uint16_t), (uint8_t*)&(uint16_t){GATT_SVC_UUID}}
    },

    [1] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&character_declaration_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t*)&(uint8_t){ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY}}
    },

    [2] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){GATT_CHAR_TX_UUID}, ESP_GATT_PERM_READ,
         256, 0, NULL}
    },

    [3] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&character_client_config_uuid,
         ESP_GATT_PERM_READ|ESP_GATT_PERM_WRITE, sizeof(uint16_t), 0, NULL}
    },

    [4] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&character_declaration_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t*)&(uint8_t){char_prop_write}}
    },

    [5] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){GATT_CHAR_RX_UUID},
         ESP_GATT_PERM_WRITE, 256, 0, NULL}
    },

    [6] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&character_declaration_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t*)&(uint8_t){char_prop_write}}
    },

    [7] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){GATT_CHAR_CTL_UUID},
         ESP_GATT_PERM_WRITE, 64, 0, NULL}
    },
};

static void set_tx_power_idle(void) {
    esp_ble_tx_power_set(PWR_ADV, ESP_PWR_LVL_N24);  
    esp_ble_tx_power_set(PWR_CONN, ESP_PWR_LVL_N24);
}

static void set_tx_power_active(void) {
    esp_ble_tx_power_set(PWR_ADV, ESP_PWR_LVL_P9);   
    esp_ble_tx_power_set(PWR_CONN, ESP_PWR_LVL_P9);
}

static void request_conn_params(uint16_t conn_id, bool active) {
    esp_ble_conn_update_params_t params = {
        .latency = active ? CONN_LATENCY_ACTIVE : CONN_LATENCY_SAVE,
        .max_int = active ? CONN_ITVL_MAX_ACTIVE : CONN_ITVL_MAX_SAVE,
        .min_int = active ? CONN_ITVL_MIN_ACTIVE : CONN_ITVL_MIN_SAVE,
        .timeout = active ? CONN_TIMEOUT_ACTIVE  : CONN_TIMEOUT_SAVE
    };
    esp_ble_gap_update_conn_params(&params);
}

static void start_advertising(power_mode_t m) {
    esp_ble_adv_params_t *p = (m == MODE_PROBE) ? &adv_params_probe : &adv_params_idle;
    esp_ble_gap_start_advertising(p);
}

static void restart_inactivity_timer(void) {
    if (inactivity_timer) {
        xTimerStop(inactivity_timer, 0);
        xTimerStart(inactivity_timer, 0);
    }
}

static void start_activity_window(void) {
    window_bytes = 0;
    if (activity_window_timer) {
        xTimerStop(activity_window_timer, 0);
        xTimerStart(activity_window_timer, 0);
    }
}

static void stop_spp(void);
static void enter_mode(power_mode_t m) {
    if (g_mode == m) return;
    g_mode = m;
    switch (m) {
        case MODE_IDLE:
            stop_spp();
            set_tx_power_idle();
            if (!g_ble_connected) start_advertising(MODE_IDLE);
            break;
        case MODE_PROBE:
            set_tx_power_idle();
            if (!g_ble_connected) start_advertising(MODE_PROBE);
            break;
        case MODE_ACTIVE:
            set_tx_power_active();
            if (g_ble_connected) request_conn_params(g_conn_id, true);
            break;
    }
    ESP_LOGI(TAG, "Switched to mode: %d", m);
}

static void inactivity_cb(TimerHandle_t xTimer) {
    ESP_LOGI(TAG, "Inactivity timeout -> power-save");
    if (g_ble_connected) {
        request_conn_params(g_conn_id, false);
        set_tx_power_idle();
    }
    enter_mode(MODE_IDLE);
}

static void activity_window_cb(TimerHandle_t xTimer) {
    if (window_bytes >= ACTIVITY_BYTES_THRESH) {
        ESP_LOGI(TAG, "Activity threshold crossed (%" PRIu32 " bytes) -> ACTIVE", window_bytes);
        enter_mode(MODE_ACTIVE);
    } else {
        if (g_ble_connected) {
            request_conn_params(g_conn_id, false);
            set_tx_power_idle();
            enter_mode(MODE_PROBE);
        } else {
            enter_mode(MODE_IDLE);
        }
    }
    start_activity_window();
}

static void spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
    switch (event) {
        case ESP_SPP_START_EVT:
            ESP_LOGI(TAG, "SPP started");
            break;
        case ESP_SPP_SRV_OPEN_EVT:
            ESP_LOGI(TAG, "SPP client connected");
            break;
        case ESP_SPP_CLOSE_EVT:
            ESP_LOGI(TAG, "SPP connection closed");
            break;
        case ESP_SPP_DATA_IND_EVT:
            ESP_LOGI(TAG, "SPP RX %d bytes", param->data_ind.len);
            esp_spp_write(param->data_ind.handle, param->data_ind.len, param->data_ind.data);
            restart_inactivity_timer();
            window_bytes += param->data_ind.len;
            break;
        default:
            break;
    }
}

static void start_spp(void) {
    if (spp_running) return;
    esp_spp_register_callback(spp_cb);
    esp_spp_init(ESP_SPP_MODE_CB);
    esp_bt_dev_set_device_name(SPP_DEVICE_NAME);
    esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, "SPP_SERVER");
    spp_running = true;
    ESP_LOGI(TAG, "SPP boost mode ON");
}

static void stop_spp(void) {
    if (!spp_running) return;
    esp_spp_deinit();
    spp_running = false;
    ESP_LOGI(TAG, "SPP boost mode OFF");
}

static uint8_t adv_service_uuid128[16] = { /* not used, keeping 16-bit svc in adv data */ };
static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = false,
    .min_interval = 0, .max_interval = 0,
    .appearance = 0x00,
    .manufacturer_len = 0, .p_manufacturer_data = NULL,
    .service_data_len = 0, .p_service_data = NULL,
    .service_uuid_len = 0, .p_service_uuid = adv_service_uuid128,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT)
};

static esp_ble_adv_data_t scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = true,
};

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
            start_advertising(g_mode);
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            ESP_LOGI(TAG, "Advertising start, status=%d", param->adv_start_cmpl.status);
            break;

        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            ESP_LOGI(TAG, "Conn params updated: intv=%d-%d, lat=%d, timeout=%d",
                     param->update_conn_params.min_int, param->update_conn_params.max_int,
                     param->update_conn_params.latency, param->update_conn_params.timeout);
            break;

        default:
            break;
    }
}

static void gatts_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                     esp_ble_gatts_cb_param_t *param) {
    switch (event) {
        case ESP_GATTS_REG_EVT:
            g_gatts_if = gatts_if;
            esp_ble_gap_set_device_name(BLE_DEVICE_NAME);
            esp_ble_gap_config_adv_data(&adv_data);
            esp_ble_gap_config_adv_data(&scan_rsp_data);
            esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, sizeof(gatt_db)/sizeof(gatt_db[0]), 0);
            break;

        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            if (param->add_attr_tab.status == ESP_GATT_OK) {
                g_service_handle  = param->add_attr_tab.handles[0];
                g_handle_char_tx  = param->add_attr_tab.handles[2];
                g_handle_char_rx  = param->add_attr_tab.handles[5];
                g_handle_char_ctl = param->add_attr_tab.handles[7];
                esp_ble_gatts_start_service(g_service_handle);
            } else {
                ESP_LOGE(TAG, "Attr table create failed 0x%x", param->add_attr_tab.status);
            }
            break;

        case ESP_GATTS_CONNECT_EVT:
            g_conn_id = param->connect.conn_id;
            g_ble_connected = true;
            ESP_LOGI(TAG, "BLE connected, conn_id=%u", g_conn_id);
            enter_mode(MODE_PROBE);
            request_conn_params(g_conn_id, false);
            start_activity_window();
            restart_inactivity_timer();
            break;

        case ESP_GATTS_DISCONNECT_EVT:
            g_ble_connected = false;
            ESP_LOGI(TAG, "BLE disconnected");
            g_conn_id = 0xFFFF;
            stop_spp();
            enter_mode(MODE_IDLE);
            start_advertising(MODE_IDLE);
            break;

        case ESP_GATTS_WRITE_EVT: {
            const esp_gatts_write_evt_param *wr = &param->write;
            if (wr->handle == g_handle_char_rx && wr->len > 0) {
                window_bytes += wr->len;
                restart_inactivity_timer();
                esp_ble_gatts_send_indicate(g_gatts_if, param->write.conn_id, g_handle_char_tx,
                                            wr->len, (uint8_t*)wr->value, false);
            }
            if (wr->handle == g_handle_char_ctl && wr->len > 0) {
                if (!memcmp(wr->value, "BOOST_ON", 8)) {
                    enter_mode(MODE_ACTIVE);
                    start_spp(); 
                } else if (!memcmp(wr->value, "BOOST_OFF", 9)) {
                    stop_spp();
                    enter_mode(MODE_PROBE);
                }
                restart_inactivity_timer();
            }
            break;
        }

        default:
            break;
    }
}

static void button_task(void *arg) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);

    int last = gpio_get_level(GPIO_BTN);
    for (;;) {
        int lvl = gpio_get_level(GPIO_BTN);
        if (lvl == BTN_ACTIVE_LEVEL && last != BTN_ACTIVE_LEVEL) {
            ESP_LOGI(TAG, "Button -> PROBE window");
            enter_mode(MODE_PROBE);
        }
        last = lvl;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_pm_config_t pmc = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 80,
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        .light_sleep_enable = true,
#else
        .light_sleep_enable = false,
#endif
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pmc));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)); 
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_cb));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(0x55)); // arbitrary app id

    inactivity_timer = xTimerCreate("inact", pdMS_TO_TICKS(INACTIVITY_MS), pdFALSE, NULL, inactivity_cb);
    activity_window_timer = xTimerCreate("awindow", pdMS_TO_TICKS(ACTIVITY_WINDOW_MS), pdTRUE, NULL, activity_window_cb);
    xTimerStart(activity_window_timer, 0);
    set_tx_power_idle();
    enter_mode(MODE_IDLE);

    xTaskCreate(button_task, "btn", 2048, NULL, 10, NULL);

    ESP_LOGI(TAG, "Ready: BLE advertising (low power). Write 'BOOST_ON' to CTL char to enable SPP boost.");
}
