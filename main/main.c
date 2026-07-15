#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_hidd_prf_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hid_dev.h"
#include "nvs_flash.h"

#define DEVICE_NAME "ESP32Gamepad"

#define PIN_X_ADC ADC_CHANNEL_6
#define PIN_Y_ADC ADC_CHANNEL_7
#define PIN_LED GPIO_NUM_2
#define PIN_BUZZER GPIO_NUM_18

#define BUTTON_COUNT 4
#define DEADZONE 350
#define ADC_MIN 0
#define ADC_MAX 4095
#define GAMEPAD_AXIS_MIN -127
#define GAMEPAD_AXIS_MAX 127

static const char *TAG = "joystick_hid";
static const gpio_num_t BUTTON_PINS[BUTTON_COUNT] = {
    GPIO_NUM_27,
    GPIO_NUM_26,
    GPIO_NUM_25,
    GPIO_NUM_33,
};

typedef struct {
    int8_t x;
    int8_t y;
    uint8_t buttons;
    uint8_t raw_buttons;
} input_state_t;

static QueueHandle_t input_queue;
static adc_oneshot_unit_handle_t adc1_handle;
static volatile bool hid_connected;
static volatile bool hid_bonded;
static volatile bool calibrate_requested;
static volatile bool buzz_requested;
static uint16_t hid_conn_id;
static input_state_t latest_state;
static uint16_t center_x = 2048;
static uint16_t center_y = 2048;

static uint8_t hidd_service_uuid128[] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

static esp_ble_adv_data_t hidd_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x03C4,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(hidd_service_uuid128),
    .p_service_uuid = hidd_service_uuid128,
    .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
};

static esp_ble_adv_params_t hidd_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x30,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static int clamp_int(int value, int min, int max)
{
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static int map_range(int value, int in_min, int in_max, int out_min, int out_max)
{
    int denominator = in_max - in_min;
    if (denominator == 0) {
        return out_min;
    }

    int64_t scaled = (int64_t)(value - in_min) * (out_max - out_min);
    return (int)(scaled / denominator + out_min);
}

static int8_t axis_to_gamepad_axis(int value, int center)
{
    int delta = value - center;

    if (delta < DEADZONE && delta > -DEADZONE) {
        return 0;
    }

    if (delta > 0) {
        int start = clamp_int(center + DEADZONE, ADC_MIN, ADC_MAX - 1);
        int mapped = map_range(value, start, ADC_MAX, 1, GAMEPAD_AXIS_MAX);
        return (int8_t)clamp_int(mapped, 1, GAMEPAD_AXIS_MAX);
    }

    int end = clamp_int(center - DEADZONE, ADC_MIN + 1, ADC_MAX);
    int mapped = map_range(value, ADC_MIN, end, GAMEPAD_AXIS_MIN, -1);
    return (int8_t)clamp_int(mapped, GAMEPAD_AXIS_MIN, -1);
}

static int read_adc(adc_channel_t channel)
{
    int value = 0;
    esp_err_t err = adc_oneshot_read(adc1_handle, channel, &value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao ler ADC %d: %s", channel, esp_err_to_name(err));
    }
    return value;
}

static uint16_t read_average(adc_channel_t channel)
{
    int total = 0;

    for (int i = 0; i < 30; i++) {
        total += read_adc(channel);
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    return (uint16_t)(total / 30);
}

static void configure_gpio(void)
{
    uint64_t button_mask = 0;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        button_mask |= 1ULL << BUTTON_PINS[i];
    }

    gpio_config_t button_config = {
        .pin_bit_mask = button_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_config));

    gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << PIN_LED) | (1ULL << PIN_BUZZER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&output_config));
    gpio_set_level(PIN_LED, 0);
    gpio_set_level(PIN_BUZZER, 0);
}

static void configure_adc(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc1_handle));

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, PIN_X_ADC, &channel_config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, PIN_Y_ADC, &channel_config));
}

static void hidd_event_callback(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    switch (event) {
    case ESP_HIDD_EVENT_REG_FINISH:
        if (param->init_finish.state == ESP_HIDD_INIT_OK) {
            esp_ble_gap_set_device_name(DEVICE_NAME);
            esp_ble_gap_config_adv_data(&hidd_adv_data);
        }
        break;

    case ESP_HIDD_EVENT_BLE_CONNECT:
        hid_connected = true;
        hid_conn_id = param->connect.conn_id;
        ESP_LOGI(TAG, "HID conectado");
        break;

    case ESP_HIDD_EVENT_BLE_DISCONNECT:
        hid_connected = false;
        hid_bonded = false;
        ESP_LOGI(TAG, "HID desconectado");
        esp_ble_gap_start_advertising(&hidd_adv_params);
        break;

    case ESP_HIDD_EVENT_BLE_LED_REPORT_WRITE_EVT:
        if (param->led_write.length > 0) {
            gpio_set_level(PIN_LED, param->led_write.data[0] ? 1 : 0);
        }
        break;

    default:
        break;
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&hidd_adv_params);
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        ESP_LOGI(TAG, "Anunciando BLE HID como %s", DEVICE_NAME);
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        hid_bonded = param->ble_security.auth_cmpl.success;
        ESP_LOGI(TAG, "Pareamento BLE: %s", hid_bonded ? "ok" : "falhou");
        break;

    default:
        break;
    }
}

static void configure_ble_hid(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_hidd_profile_init());

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_hidd_register_callbacks(hidd_event_callback));

    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key)));
}

static void input_task(void *arg)
{
    (void)arg;
    bool last_buttons[BUTTON_COUNT] = {true, true, true, true};

    center_x = read_average(PIN_X_ADC);
    center_y = read_average(PIN_Y_ADC);
    ESP_LOGI(TAG, "Centro calibrado: x=%u y=%u", center_x, center_y);

    while (true) {
        if (calibrate_requested) {
            calibrate_requested = false;
            center_x = read_average(PIN_X_ADC);
            center_y = read_average(PIN_Y_ADC);
            ESP_LOGI(TAG, "Recalibrado: x=%u y=%u", center_x, center_y);
        }

        input_state_t state = {
            .x = axis_to_gamepad_axis(read_adc(PIN_X_ADC), center_x),
            .y = axis_to_gamepad_axis(read_adc(PIN_Y_ADC), center_y),
            .buttons = 0,
            .raw_buttons = 0,
        };

        for (int i = 0; i < BUTTON_COUNT; i++) {
            bool released = gpio_get_level(BUTTON_PINS[i]) != 0;
            if (released != last_buttons[i]) {
                vTaskDelay(pdMS_TO_TICKS(20));
                released = gpio_get_level(BUTTON_PINS[i]) != 0;
            }

            if (!released) {
                state.raw_buttons |= (uint8_t)(1U << i);
            }
            last_buttons[i] = released;
        }

        state.buttons = state.raw_buttons & 0x0F;

        latest_state = state;
        xQueueOverwrite(input_queue, &state);
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

static void hid_report_task(void *arg)
{
    (void)arg;
    input_state_t state;

    while (true) {
        if (xQueueReceive(input_queue, &state, portMAX_DELAY) == pdTRUE) {
            if (hid_connected && hid_bonded) {
                esp_hidd_send_gamepad_value(hid_conn_id, state.buttons, state.x, state.y);
            }
        }
    }
}

static void output_task(void *arg)
{
    (void)arg;
    uint8_t last_buttons = 0;
    bool blink = false;

    while (true) {
        if (buzz_requested || (latest_state.raw_buttons != 0 && latest_state.raw_buttons != last_buttons)) {
            buzz_requested = false;
            gpio_set_level(PIN_BUZZER, 1);
            vTaskDelay(pdMS_TO_TICKS(35));
            gpio_set_level(PIN_BUZZER, 0);
        }
        last_buttons = latest_state.raw_buttons;

        if (hid_connected) {
            gpio_set_level(PIN_LED, latest_state.raw_buttons ? 0 : 1);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            blink = !blink;
            gpio_set_level(PIN_LED, blink);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
}

void app_main(void)
{
    input_queue = xQueueCreate(1, sizeof(input_state_t));
    if (input_queue == NULL) {
        ESP_LOGE(TAG, "Nao foi possivel criar a fila de entrada");
        return;
    }

    configure_gpio();
    configure_adc();
    configure_ble_hid();

    xTaskCreate(input_task, "input_task", 4096, NULL, 5, NULL);
    xTaskCreate(hid_report_task, "hid_report_task", 4096, NULL, 4, NULL);
    xTaskCreate(output_task, "output_task", 2048, NULL, 3, NULL);
}
