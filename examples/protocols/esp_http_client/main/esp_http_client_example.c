/* ESP HTTP Client Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "esp_tls.h"

#include "esp_http_client.h"

#include "cam.h"
#include "ov2640.h"
#include "sensor.h"
#include "sccb.h"
#include "jpeg.h"

#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 2048
static const char *TAG = "HTTP_CLIENT";

#define CAM_WIDTH   (320)
#define CAM_HIGH    (240)
#define CAM_XCLK  1
#define CAM_PCLK  33 
#define CAM_VSYNC 2
#define CAM_HSYNC 3
#define CAM_D0    46  /*!< hardware pins: D2 */  
#define CAM_D1    45  /*!< hardware pins: D3 */
#define CAM_D2    41  /*!< hardware pins: D4 */
#define CAM_D3    42  /*!< hardware pins: D5 */
#define CAM_D4    39  /*!< hardware pins: D6 */
#define CAM_D5    40  /*!< hardware pins: D7 */
#define CAM_D6    21  /*!< hardware pins: D8 */
#define CAM_D7    38  /*!< hardware pins: D9 */
#define CAM_SCL   7
#define CAM_SDA   8

/* Root cert for howsmyssl.com, taken from howsmyssl_com_root_cert.pem

   The PEM file was extracted from the output of this command:
   openssl s_client -showcerts -connect www.howsmyssl.com:443 </dev/null

   The CA root cert is the last cert given in the chain of certs.

   To embed it in the app binary, the PEM file is named
   in the component.mk COMPONENT_EMBED_TXTFILES variable.
*/
extern const char howsmyssl_com_root_cert_pem_start[] asm("_binary_howsmyssl_com_root_cert_pem_start");
extern const char howsmyssl_com_root_cert_pem_end[]   asm("_binary_howsmyssl_com_root_cert_pem_end");

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer;  // Buffer to store response of http request from event handler
    static int output_len;       // Stores number of bytes read
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            /*
             *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
             *  However, event handler can also be used in case chunked encoding is used.
             */
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // If user_data buffer is configured, copy the response into the buffer
                if (evt->user_data) {
                    memcpy(evt->user_data + output_len, evt->data, evt->data_len);
                } else {
                    if (output_buffer == NULL) {
                        output_buffer = (char *) malloc(esp_http_client_get_content_length(evt->client));
                        output_len = 0;
                        if (output_buffer == NULL) {
                            ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                            return ESP_FAIL;
                        }
                    }
                    memcpy(output_buffer + output_len, evt->data, evt->data_len);
                }
                output_len += evt->data_len;
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            if (output_buffer != NULL) {
                // Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
                // ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
                free(output_buffer);
                output_buffer = NULL;
                output_len = 0;
            }
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
            if (err != 0) {
                if (output_buffer != NULL) {
                    free(output_buffer);
                    output_buffer = NULL;
                    output_len = 0;
                }
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            break;
    }
    return ESP_OK;
}

static void http_rest_with_url(char *put_data, int put_data_length)
{
    char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};
    /**
     * NOTE: All the configuration parameters for http_client must be spefied either in URL or as host and path parameters.
     * If host and path parameters are not set, query parameter will be ignored. In such cases,
     * query parameter should be specified in URL.
     *
     * If URL as well as host and path parameters are specified, values of host and path will be considered.
     */
    esp_http_client_config_t config = {
        .url = "https://greenwatch-photos.s3.amazonaws.com/greenwatch_alpha1620482363.590716?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=ASIARBRC4UMDOBBFCJJX%2F20210508%2Feu-north-1%2Fs3%2Faws4_request&X-Amz-Date=20210508T135923Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host&X-Amz-Security-Token=IQoJb3JpZ2luX2VjENb%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCmV1LW5vcnRoLTEiRjBEAiBt3smlx4%2BCvqt0C7TVHPA%2FUvGU5HrepkpJLb6dJiRjuQIgAK7YwJT0fVZCoANb1ZzBBqUAKGvLKj9uW3O5ccNg150qogIIXxAAGgwwNzIwMTM4ODIxMTgiDOMbUMeiaMGN0JVVJir%2FAVzlZn6DXgqNLaVsMJSlN0Ji%2BDOD58%2FYl95lpHNQIwGji8uVVMCQmYDiRM2epJu4vtMbRtCiRZQqzNi0Erds4ZvVQ2KtlmLZqy3D%2FGJ7ynhyt%2FyDAkA%2FFc1VAFNM46oJfzBoQRgtnKFygWv0MWTGk2TP6ikrSdIdZryl%2F5D6eMYPQf6f2d%2BKrgh3ZaHMPtlfCOOo%2FkqfLACWWAGEMM8fONKaOLzMZImAZ5l0kZIgUFN2kv99n4Jpq7Uil4MiAvYZ%2BAXXzOT9X2Kw50ubAkBp%2F%2BKjreE5NXrcO8yFLKJD1MmViNCltqlFb00rraZsbU00DwQvpycehELnXuC5UyKnwzC6stqEBjqbASEsP8JyiORUdxQtpVrTegNspTTWpphPTfTNG0Cmu4b4c8Eeg8wGQadarfVgVlmXzD4X6c6OaQQW6tmEyitTe8h%2BOb83g8EmBA4QeXGor5LSXd44GVn5GvCIse1ls57SfLdc54C3nIp52ZO6xl%2F5Wo%2B7HpUPbU1MhIul4YLJ1zLQwkYIDios5%2Fn2T6jvaD905QYEpc2Oa%2Bpn02lo&X-Amz-Signature=9c828a76c8505654f4e60294e37eaea0c8ca4c2e7038179ad5ba2d7f2f6bc474",
        .event_handler = _http_event_handler,
        .user_data = local_response_buffer,        // Pass address of local buffer to get response
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    //PUT
    esp_http_client_set_method(client, HTTP_METHOD_PUT);
    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    ESP_LOGI(TAG, "put_data_length: %d", put_data_length);
    esp_http_client_set_post_field(client, put_data, put_data_length);//strlen(put_data));
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP PUT Status = %d, content_length = %d",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP PUT request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

static void cam_task(void *arg)
{    cam_config_t cam_config = {
        .bit_width    = 8,
        .mode.jpeg    = 1,
        .xclk_fre     = 16 * 1000 * 1000,
        .pin  = {
            .xclk     = CAM_XCLK,
            .pclk     = CAM_PCLK,
            .vsync    = CAM_VSYNC,
            .hsync    = CAM_HSYNC,
        },
        .pin_data     = {CAM_D0, CAM_D1, CAM_D2, CAM_D3, CAM_D4, CAM_D5, CAM_D6, CAM_D7},
        .vsync_invert = true,
        .hsync_invert = false,
        .size = {
            .width    = CAM_WIDTH,
            .high     = CAM_HIGH,
        },
        .max_buffer_size = 8 * 1024,
        .task_stack      = 1024,
        .task_pri        = configMAX_PRIORITIES
    };

    /*!< With PingPang buffers, the frame rate is higher, or you can use a separate buffer to save memory */
    cam_config.frame1_buffer = (uint8_t *)heap_caps_malloc(CAM_WIDTH * CAM_HIGH * 2 * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    cam_config.frame2_buffer = (uint8_t *)heap_caps_malloc(CAM_WIDTH * CAM_HIGH * 2 * sizeof(uint8_t), MALLOC_CAP_SPIRAM);

    cam_init(&cam_config);

    sensor_t sensor;
    int camera_version = 0;      /*!<If the camera version is determined, it can be set to manual mode */
    SCCB_Init(CAM_SDA, CAM_SCL);
    sensor.slv_addr = SCCB_Probe();
    ESP_LOGI(TAG, "sensor_id: 0x%x\n", sensor.slv_addr);

    camera_version = 2640;
    if (sensor.slv_addr == 0x30 || camera_version == 2640) { /*!< Camera: OV2640 */
        ESP_LOGI(TAG, "OV2640 init start...");

        if (OV2640_Init(0, 1) != 0) {
            goto fail;
        }

        if (cam_config.mode.jpeg) {
            OV2640_JPEG_Mode();
        } else {
            OV2640_RGB565_Mode(false);	/*!< RGB565 mode */
        }

        OV2640_ImageSize_Set(800, 600);
        OV2640_ImageWin_Set(0, 0, 800, 600);
        OV2640_OutSize_Set(CAM_WIDTH, CAM_HIGH);
    }

    ESP_LOGI(TAG, "camera init done\n");
    cam_start();

    while (1) {
        uint8_t *cam_buf = NULL;
        uint32_t cam_buf_len = cam_take(&cam_buf);
        int w, h;
        uint8_t *img = jpeg_decode(cam_buf, &w, &h);

        if (img) {
            ESP_LOGI(TAG, "jpeg: w: %d, h: %d\n", w, h);
            // lcd_set_index(0, 0, w - 1, h - 1);
            // lcd_write_data(img, w * h * sizeof(uint16_t));
            ESP_LOGI(TAG, "free heap w/ img: %d", esp_get_free_heap_size());
            const char *put_data = "BlubbUsing a Palantír requires a person with great strength of will and wisdom. The Palantíri were meant to ";
            http_rest_with_url((unsigned char *)cam_buf, cam_buf_len);
            free(img);
            ESP_LOGI(TAG, "free heap w/o img: %d", esp_get_free_heap_size());
        }
        cam_give(cam_buf);
        /*!< Use a logic analyzer to observe the frame rate */
        vTaskDelay(20000/portTICK_PERIOD_MS);
    }


fail:
    free(cam_config.frame1_buffer);
    free(cam_config.frame2_buffer);
    cam_deinit();
    vTaskDelete(NULL);
}

static void http_test_task(void *pvParameters)
{
    const char *put_data = "BlubbUsing a Palantír requires a person with great strength of will and wisdom. The Palantíri were meant to ";
//    http_rest_with_url(put_data);
    ESP_LOGI(TAG, "Finish http example");
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());
    ESP_LOGI(TAG, "Connected to AP, begin http example");
    xTaskCreate(cam_task, "cam_task", 2048+8192, NULL, 5, NULL);
    //http_rest_with_url();

    //xTaskCreate(&http_test_task, "http_test_task", 8192, NULL, 5, NULL);
}
