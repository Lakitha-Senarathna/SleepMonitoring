#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "freertos/event_groups.h"

// --- Wi-Fi Configuration ---
#define ESP_WIFI_SSID      "Unlimited"
#define ESP_WIFI_PASS      "99999999"
#define MAXIMUM_RETRY      5

// FreeRTOS event group to signal when we are connected
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;

// Unified Tag for terminal logging
static const char *TAG = "ESP32_SENSOR_NODE";

// I2C Configuration 
#define I2C_MASTER_SCL_IO           22      // GPIO number for I2C master clock
#define I2C_MASTER_SDA_IO           21      // GPIO number for I2C master data
#define I2C_MASTER_NUM              0       // I2C port number for master dev
#define I2C_MASTER_FREQ_HZ          400000  // I2C master clock frequency
#define I2C_MASTER_TX_BUF_DISABLE   0       
#define I2C_MASTER_RX_BUF_DISABLE   0  

// Sensor I2C Addresses 
#define MPU6050_ADDR  0x68  // Can sometimes be 0x69
#define AHT21_ADDR    0x38
#define ENS160_ADDR   0x53  // Can sometimes be 0x52

// MPU6050 Registers
#define MPU_PWR_MGMT_1 0x6B
#define MPU_ACCEL_XOUT_H 0x3B

// ENS160 Registers
#define ENS_OPMODE     0x10
#define ENS_DATA_AQI   0x21
#define ENS_DATA_TVOC  0x22
#define ENS_DATA_ECO2  0x24

// Global MQTT Client Handle (Accessible by the sensor task)
esp_mqtt_client_handle_t mqtt_client = NULL; 

// I2C Initialization 
static esp_err_t i2c_master_init(void) {
    int i2c_master_port = I2C_MASTER_NUM;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    i2c_param_config(i2c_master_port, &conf);
    return i2c_driver_install(i2c_master_port, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
}

// MQTT Event Handler
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected to Broker!");
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT Disconnected");
            break;
            
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Received Data on Topic: %.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "Payload: %.*s", event->data_len, event->data);
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Error Occurred");
            break;
            
        default:
            break;
    }
}

// Wi-Fi Event Handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Wi-Fi hardware started, tell it to connect
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // Disconnected. Try to reconnect up to MAXIMUM_RETRY times.
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Retrying Wi-Fi connection...");
        } else {
            // Out of retries, set the failure bit so init_wifi() can unblock
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGE(TAG, "Connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // Success! We got an IP address.
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        // Set the success bit so init_wifi() can unblock and finish
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Wi-Fi Initialization Function
void init_wifi(void) {
    s_wifi_event_group = xEventGroupCreate();

    //  Initialize the underlying TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // Initialize Wi-Fi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register the Event Handler for both Wi-Fi and IP events
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    // Configure Wi-Fi in Station (Client) mode
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASS,
            // Authmode threshold sets the minimum security required
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // Start the Wi-Fi driver (This triggers WIFI_EVENT_STA_START in the handler)
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi initialization finished. Waiting for connection...");

    // Wait here until either the CONNECTED or FAIL bit is set by the event handler
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Successfully connected to AP SSID:%s", ESP_WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID:%s", ESP_WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

// Sensor Initialization Functions
static void init_MPU6050() {
    uint8_t data[2];
    // Wake up MPU6050 (Write 0x00 to PWR_MGMT_1)
    data[0] = MPU_PWR_MGMT_1;
    data[1] = 0x00;
    i2c_master_write_to_device(I2C_MASTER_NUM, MPU6050_ADDR, data, 2, pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "MPU6050 Awake");
}

static void init_ENS160() {
    uint8_t data[2];
    // Initialize ENS160 to Standard Operation Mode (Write 0x02 to OPMODE)
    data[0] = ENS_OPMODE;
    data[1] = 0x02;
    i2c_master_write_to_device(I2C_MASTER_NUM, ENS160_ADDR, data, 2, pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "ENS160 set to Standard Mode");
}

static void init_AHT21() {
    vTaskDelay(pdMS_TO_TICKS(40));

    uint8_t data[3];

    data[0] = 0xBE; // AHT21 Initialization command
    data[1] = 0x08; // Parameter 1
    data[2] = 0x00; // Parameter 2
    i2c_master_write_to_device(I2C_MASTER_NUM, AHT21_ADDR, data, 3, pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "AHT21 Initialized and Calibrated");  
}

// Master function to initialize all sensors with a single call
static void init_all_sensors() {
    init_MPU6050();
    init_ENS160();
    init_AHT21();

    // Delay to let sensors stabilize after power on before triggering measurements.
    vTaskDelay(pdMS_TO_TICKS(100));
}

// Sensor Reading & Data Publishing Task
void sensor_read_publish_task(void *pvParameters) {
    char json_payload[256];
    uint8_t rx_data[7]; // Buffer for receiving I2C bytes
    uint8_t tx_data[3]; // Buffer for transmitting I2C bytes

    while (1) {
        // Read Data from the I2C Bus MPU6050 Accelerometer Data
        float accel_x = 0, accel_y = 0, accel_z = 0;
        tx_data[0] = MPU_ACCEL_XOUT_H;
        if (i2c_master_write_read_device(I2C_MASTER_NUM, MPU6050_ADDR, tx_data, 1, rx_data, 6, pdMS_TO_TICKS(1000)) == ESP_OK) {
            // MPU6050 returns Big Endian data. Combine High and Low bytes.
            int16_t raw_x = (rx_data[0] << 8) | rx_data[1];
            int16_t raw_y = (rx_data[2] << 8) | rx_data[3];
            int16_t raw_z = (rx_data[4] << 8) | rx_data[5];
            
            // Convert to 'g' (Assuming default +/- 2g range where 1g = 16384)
            accel_x = raw_x / 16384.0;
            accel_y = raw_y / 16384.0;
            accel_z = raw_z / 16384.0;
        }
        
        // Read Data from the I2C Bus AHT21 Temperature & Humidity Data
        float temperature = 0, humidity = 0;
        tx_data[0] = 0xAC; // Trigger measurement command
        tx_data[1] = 0x33;
        tx_data[2] = 0x00;
        i2c_master_write_to_device(I2C_MASTER_NUM, AHT21_ADDR, tx_data, 3, pdMS_TO_TICKS(1000));
        
        // AHT21 requires ~80ms to process the measurement
        vTaskDelay(pdMS_TO_TICKS(80)); 

        if (i2c_master_read_from_device(I2C_MASTER_NUM, AHT21_ADDR, rx_data, 7, pdMS_TO_TICKS(1000)) == ESP_OK) {
            // AHT21 packs 20 bits of Hum and 20 bits of Temp across 5 bytes
            uint32_t raw_hum = ((uint32_t)rx_data[1] << 12) | ((uint32_t)rx_data[2] << 4) | (rx_data[3] >> 4);
            uint32_t raw_temp = (((uint32_t)(rx_data[3] & 0x0F)) << 16) | ((uint32_t)rx_data[4] << 8) | rx_data[5];

            humidity = ((float)raw_hum * 100.0) / 1048576.0;         // 2^20 = 1048576
            temperature = ((float)raw_temp * 200.0) / 1048576.0 - 50.0;
        }

        // Read Data from the I2C Bus ENS160 Air Quality Data
        uint16_t eco2 = 0, tvoc = 0;
        uint8_t aqi = 0;
        
        // Read AQI (1 byte)
        tx_data[0] = ENS_DATA_AQI;
        i2c_master_write_read_device(I2C_MASTER_NUM, ENS160_ADDR, tx_data, 1, &aqi, 1, pdMS_TO_TICKS(1000));

        // Read TVOC (2 bytes, Little Endian)
        tx_data[0] = ENS_DATA_TVOC;
        i2c_master_write_read_device(I2C_MASTER_NUM, ENS160_ADDR, tx_data, 1, rx_data, 2, pdMS_TO_TICKS(1000));
        tvoc = rx_data[0] | (rx_data[1] << 8);

        // Read eCO2 (2 bytes, Little Endian)
        tx_data[0] = ENS_DATA_ECO2;
        i2c_master_write_read_device(I2C_MASTER_NUM, ENS160_ADDR, tx_data, 1, rx_data, 2, pdMS_TO_TICKS(1000));
        eco2 = rx_data[0] | (rx_data[1] << 8);

        // Format JSON string
        snprintf(json_payload, sizeof(json_payload), 
            "{"
            "\"accel\": {\"x\": %.2f, \"y\": %.2f, \"z\": %.2f}, "
            "\"environment\": {\"temp_c\": %.1f, \"humidity\": %.1f}, "
            "\"air_quality\": {\"eco2\": %u, \"tvoc\": %u, \"aqi\": %u}"
            "}", 
            accel_x, accel_y, accel_z, 
            temperature, humidity, 
            eco2, tvoc, aqi
        );

        ESP_LOGI(TAG, "Prepared Payload: %s", json_payload);

        // C. Publish the JSON string to the MQTT broker
        if (mqtt_client != NULL) {
            int msg_id = esp_mqtt_client_publish(mqtt_client, "device/esp32/sensors", json_payload, 0, 1, 0);
            
            if (msg_id != -1) {
                ESP_LOGI(TAG, "Sent publish successful, msg_id=%d", msg_id);
            } else {
                ESP_LOGW(TAG, "Failed to publish (Client might be disconnected)");
            }
        } else {
            ESP_LOGW(TAG, "MQTT Client not initialized, skipping publish.");
        }

        // Wait 5 seconds before the next reading
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// Main Function
void app_main(void) {
    ESP_LOGI(TAG, "Starting ESP32 MQTT Sensor Node");

    // Initialize NVS (Crucial for Wi-Fi to save configuration/calibration)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize Hardware (I2C)
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(TAG, "I2C Master initialized successfully");

    // Sensor Initialization
    init_all_sensors();

    // Wi-Fi Initialization
    init_wifi(); 

    // Configure the MQTT Client
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://192.168.1.100", // Update with your broker IP
        .broker.address.port = 1883,
        .credentials.username = "my_user",            // Update or remove if unneeded
        .credentials.authentication.password = "my_pass"
    };

    // Initialize the client and save it to our global handle
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

    // Register the event handler and start the MQTT background task
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    // Spawn the Sensor Reading Task on a dedicated FreeRTOS thread
    xTaskCreate(sensor_read_publish_task, "sensor_task", 4096, NULL, 5, NULL);

    // Main loop yields to background tasks
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); 
    }
}