/**
 * @file lv_port_disp_template.c
 *
 */

/*Copy this file as "lv_port_disp.c" and set this value to "1" to enable content*/
#if 1

/*********************
 *      INCLUDES
 *********************/
#include <Arduino.h>
#include "lv_port_disp.h"
#include <stdbool.h>
#include <Arduino_GFX_Library.h>
#include "pincfg.h"
#include "dispcfg.h"

/*********************
 *      DEFINES
 *********************/
#ifndef MY_DISP_HOR_RES
  #if TFT_rot == 0 || TFT_rot == 2
    #define MY_DISP_HOR_RES    TFT_res_W
    #define MY_DISP_VER_RES    TFT_res_H
  #else // TFT_rot == 1 || TFT_rot == 3
    #define MY_DISP_HOR_RES    TFT_res_H
    #define MY_DISP_VER_RES    TFT_res_W
  #endif
#endif

typedef enum {
    LVGL_FLUSH_READY_BIT = (1 << 0),    // Flush is ready to be called again
    LVGL_TE_BIT = (1 << 1),             // TE (Tear Effect) signal received
} lvgl_disp_evt_bits_t;

#define BYTE_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565)) /*will be 2 for RGB565 */

/**********************
 *      TYPEDEFS
 **********************/
static const char *TAG = "LVGL";

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void disp_init(void);
static void disp_flush(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map);

static void lvgl_task(void *arg);
static void tear_interrupt(void *arg);

/**********************
 *  STATIC VARIABLES
 **********************/
static Arduino_DataBus *bus = new Arduino_ESP32QSPI(TFT_CS, TFT_SCK, TFT_SDA0, TFT_SDA1, TFT_SDA2, TFT_SDA3);
static Arduino_GFX *g = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, TFT_res_W, TFT_res_H);
static Arduino_Canvas *gfx = new Arduino_Canvas(TFT_res_W, TFT_res_H, g, 0, 0, TFT_rot);
EventGroupHandle_t lvgl_disp_evt_group = NULL;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_port_disp_init(void)
{
    /*-------------------------
     * Initialize your display
     * -----------------------*/
    disp_init();

    /*------------------------------------
     * Create a display and set a flush_cb
     * -----------------------------------*/
    lv_display_t * disp = lv_display_create(MY_DISP_HOR_RES, MY_DISP_VER_RES);
    lv_display_set_flush_cb(disp, disp_flush);

    /* Example 1
     * One buffer for partial rendering*/
    LV_ATTRIBUTE_MEM_ALIGN
    static uint8_t buf_1_1[MY_DISP_HOR_RES * 10 * BYTE_PER_PIXEL];            /*A buffer for 10 rows*/
    lv_display_set_buffers(disp, buf_1_1, NULL, sizeof(buf_1_1), LV_DISPLAY_RENDER_MODE_PARTIAL);

#if 0
    /* Example 2
     * Two buffers for partial rendering
     * In flush_cb DMA or similar hardware should be used to update the display in the background.*/
    LV_ATTRIBUTE_MEM_ALIGN
    static uint8_t buf_2_1[MY_DISP_HOR_RES * 10 * BYTE_PER_PIXEL];

    LV_ATTRIBUTE_MEM_ALIGN
    static uint8_t buf_2_2[MY_DISP_HOR_RES * 10 * BYTE_PER_PIXEL];
    lv_display_set_buffers(disp, buf_2_1, buf_2_2, sizeof(buf_2_1), LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* Example 3
     * Two buffers screen sized buffer for double buffering.
     * Both LV_DISPLAY_RENDER_MODE_DIRECT and LV_DISPLAY_RENDER_MODE_FULL works, see their comments*/
    LV_ATTRIBUTE_MEM_ALIGN
    static uint8_t buf_3_1[MY_DISP_HOR_RES * MY_DISP_VER_RES * BYTE_PER_PIXEL];

    LV_ATTRIBUTE_MEM_ALIGN
    static uint8_t buf_3_2[MY_DISP_HOR_RES * MY_DISP_VER_RES * BYTE_PER_PIXEL];
    lv_display_set_buffers(disp, buf_3_1, buf_3_2, sizeof(buf_3_1), LV_DISPLAY_RENDER_MODE_DIRECT);
#endif

    lvgl_disp_evt_group = xEventGroupCreate();
    if (lvgl_disp_evt_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        while(1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    BaseType_t res;
    res = xTaskCreate(lvgl_task, "LVGL task", 2048, NULL, 1, NULL);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LVGL task");
        while(1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // TE(Tear Effect≒VSYNC) GPIO setup
    const gpio_config_t te_detect_cfg = {
        .pin_bit_mask = BIT64(TFT_TE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };

    ESP_ERROR_CHECK(gpio_config(&te_detect_cfg));
    gpio_install_isr_service(0);
    ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)TFT_TE, tear_interrupt, NULL));
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void tear_interrupt(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (lvgl_disp_evt_group != NULL) {
        xEventGroupSetBitsFromISR(lvgl_disp_evt_group, LVGL_TE_BIT, &xHigherPriorityTaskWoken);

        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
}

/*Initialize your display and the required peripherals.*/
static void disp_init(void)
{
    // Display setup
    if(!gfx->begin(40 * 1000 * 1000)) {   // 40MHz is the maximum for the display
        esp_rom_printf("Failed to initialize display!\n");
        return;
    }

    bus->sendCommand(0x35); // TE ON
    bus->sendData(0x00);    // TE mode: VSYNC signal

    gfx->fillScreen(WHITE);
}

volatile bool disp_flush_enabled = true;

/* Enable updating the screen (the flushing process) when disp_flush() is called by LVGL
 */
void disp_enable_update(void)
{
    disp_flush_enabled = true;
}

/* Disable updating the screen (the flushing process) when disp_flush() is called by LVGL
 */
void disp_disable_update(void)
{
    disp_flush_enabled = false;
}

/*Flush the content of the internal buffer the specific area on the display.
 *`px_map` contains the rendered image as raw pixel map and it should be copied to `area` on the display.
 *You can use DMA or any hardware acceleration to do this operation in the background but
 *'lv_display_flush_ready()' has to be called when it's finished.*/
static void disp_flush(lv_display_t * disp_drv, const lv_area_t * area, uint8_t * px_map)
{
    if(disp_flush_enabled) {
        uint32_t w = lv_area_get_width(area);
        uint32_t h = lv_area_get_height(area);
        gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
    }

    /*IMPORTANT!!!
     *Inform the graphics library that you are ready with the flushing*/
    lv_display_flush_ready(disp_drv);

    xEventGroupSetBits(lvgl_disp_evt_group, LVGL_FLUSH_READY_BIT); // Signal that flush is ready

}

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(lvgl_disp_evt_group, LVGL_FLUSH_READY_BIT | LVGL_TE_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        if ((bits & LVGL_FLUSH_READY_BIT) && (bits & LVGL_TE_BIT)) {
            gfx->flush();
            xEventGroupClearBits(lvgl_disp_evt_group, LVGL_FLUSH_READY_BIT | LVGL_TE_BIT);
        }
    }
}

#else /*Enable this file at the top*/

/*This dummy typedef exists purely to silence -Wpedantic.*/
typedef int keep_pedantic_happy;
#endif
