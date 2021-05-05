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
        cam_take(&cam_buf);
        int w, h;
        uint8_t *img = jpeg_decode(cam_buf, &w, &h);

        if (img) {
            ESP_LOGI(TAG, "jpeg: w: %d, h: %d\n", w, h);
            // lcd_set_index(0, 0, w - 1, h - 1);
            // lcd_write_data(img, w * h * sizeof(uint16_t));
            free(img);
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

static void http_rest_with_url(void)
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
        .url = "https://greenwatch-photos.s3.amazonaws.com/greenwatch_alpha1620244469.954609?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=ASIARBRC4UMDHWWENT46%2F20210505%2Feu-north-1%2Fs3%2Faws4_request&X-Amz-Date=20210505T195429Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host&X-Amz-Security-Token=IQoJb3JpZ2luX2VjEJT%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEaCmV1LW5vcnRoLTEiRjBEAiBtL%2F4H6UhVuDby0Nsr1bHolorME4KHb7c57U%2BWI0Df2wIge24kgVgHCYjgy5vDzBYVE6wwK3rzTWOXDP5YqtpY9aUq3AEIHRAAGgwwNzIwMTM4ODIxMTgiDCxmHbT82GUXLwrgOSq5ART0rM1htW%2FohyN%2BgCs3Z5ilHiAK2BUoSu2XHsz7Dh4upPCKvl3K7bofh6TSQU3FI2alSHH99%2BqFX7bs6ZhUS3Qs3pVXu4l%2FhsfodoO4dU224Hl7bhEgraGFciSMvVm97Oz6DF%2F6Cdin9tYpzB2LydlR3MUWGzagj3dLV6vBHqgddYXUUmiwLGggAQ%2BYv9W7z5zgwanN%2BAFNDPe868UgoYqa%2Bv8GWDKnY%2BKz9JAnkLJuRm0IwMLykhx8MPTvy4QGOuEB3%2FjdasO%2B27kSes10JWyfkCvAlQORylA5j5sMFxBZ9SoRvw4uZDEJCG83i%2FE3Ddvk35ph0EQ433O2jQTSfTqfMf99d6TdXepUzXi84YVaXFTU%2BlBWtm6jfpcTTjC0Wx1G8%2BCRt5zi7rpVEGpYl61hH3EQwEwuBOFTVOtp5yxyxOZ5frCCv3SRgYADDuBREO6oquovcsdxA9CucKAcnIup3NtZtPbDBIJU1TYwtoeFgVbGRCHJP%2BbkyvTXLD3K4OSUT2zWQJjWCDpPObqqgmnuZzqvQh0RANuDEMTSou7x5gy0&X-Amz-Signature=d63cb9864fbf8f415313615ba2e5c49e4ebb9a704b31a9275c62274a1b37824f",
        .event_handler = _http_event_handler,
        .user_data = local_response_buffer,        // Pass address of local buffer to get response
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    //PUT
    const char *put_data = "Using a Palantír requires a person with great strength of will and wisdom. The Palantíri were meant to ";
    esp_http_client_set_method(client, HTTP_METHOD_PUT);
    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    esp_http_client_set_post_field(client, put_data, strlen(put_data));
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

static void http_test_task(void *pvParameters)
{
    http_rest_with_url();
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
    xTaskCreate(cam_task, "cam_task", 2048, NULL, 6, NULL);

    xTaskCreate(&http_test_task, "http_test_task", 8192, NULL, 5, NULL);
}
