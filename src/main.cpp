/* Using LVGL with Arduino requires some extra steps:
   Be sure to read the docs here: https://docs.lvgl.io/master/details/integration/framework/arduino.html
   To use the built-in examples and demos of LVGL uncomment the includes below respectively.
   You also need to copy 'lvgl/examples' to 'lvgl/src/examples'. Similarly for the demos 'lvgl/demos' to 'lvgl/src/demos'.
*/

#include <lvgl.h>
#include "lv_port_disp.h"

#include <Arduino.h>
#include "pincfg.h"
#include "dispcfg.h"
#include "AXS15231B_touch.h"
#include "SD_MMC.h"
#include "FS.h"
#include <vector>
#include "Audio.h"
#include "esp_timer.h"

#define MP3_FOLDER "/music"

AXS15231B_Touch touch(Touch_SCL, Touch_SDA, Touch_INT, Touch_ADDR, TFT_rot);

std::vector<char *> v_fileContent;
File dir;
void listDir(fs::FS &fs, const char *dirname, uint8_t levels);

Audio audio;
const char audioDir[] = MP3_FOLDER;
lv_obj_t *lb_info = NULL;

int curr_show_index = 0;

#if LV_USE_LOG != 0
// Log to serial console
void my_print(lv_log_level_t level, const char *buf) {
    LV_UNUSED(level);
    Serial.println(buf);
    Serial.flush();
}
#endif

// Callback so LVGL know the elapsed time
uint32_t millis_cb(void) {
    return millis();
}

// Read the touchpad
void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
    uint16_t x, y;
    if (touch.touched()) {
        // Read touched point from touch module
        touch.readData(&x, &y);

        // Set the coordinates
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void setup() {
    #ifdef ARDUINO_USB_CDC_ON_BOOT
    delay(2000);
    #endif

    Serial.begin(115200);
    Serial.println("Arduino_GFX LVGL ");
    String LVGL_Arduino = String('V') + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch() + " example";
    Serial.println(LVGL_Arduino);

    // Switch backlight on
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    // Touch setup
    if(!touch.begin()) {
        Serial.println("Failed to initialize touch module!");
        return;
    }
    touch.enOffsetCorrection(true);
    touch.setOffsets(Touch_X_min, Touch_X_max, TFT_res_W-1, Touch_Y_min, Touch_Y_max, TFT_res_H-1);

    // Init LVGL
    lv_init();
    lv_port_disp_init();

    // Set a tick source so that LVGL will know how much time elapsed
    lv_tick_set_cb(millis_cb);

    // Register print function for debugging
    #if LV_USE_LOG != 0
    lv_log_register_print_cb(my_print);
    #endif

#if 0
    // Initialize the display buffer
    uint32_t screenWidth = gfx->width();
    uint32_t screenHeight = gfx->height();
    uint32_t bufSize = screenWidth * screenHeight / 10;
    lv_color_t *disp_draw_buf = (lv_color_t *)heap_caps_malloc(bufSize * 2, /*MALLOC_CAP_INTERNAL*/MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
    if (!disp_draw_buf) {
        Serial.println("LVGL failed to allocate display buffer!");
        return;
    }

    // Initialize the display driver
    
    lv_display_t *disp = lv_display_create(screenWidth, screenHeight);
    lv_display_set_flush_cb(disp, my_disp_flush);
    lv_display_set_buffers(disp, disp_draw_buf, NULL, bufSize * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
#endif

    // Initialize the input device driver
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touchpad_read);

    /* Option 1: Create a simple label */
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "Digital Audio Player");
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 10);

    /* Option 2: LVGL Demo. 
       Don't forget to enable the demos in lv_conf.h. E.g. LV_USE_DEMOS_WIDGETS
    */
    // lv_demos_create(nullptr, 0);
    //lv_demo_widgets();
    // lv_demo_benchmark();
    // lv_demo_keypad_encoder();
    // lv_demo_music();
    // lv_demo_stress();

    lb_info = lv_label_create(lv_scr_act());
    lv_label_set_text(lb_info, "---");
    lv_obj_align(lb_info, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_set_style_text_line_space(lb_info, 12, 0);
    lv_label_set_long_mode(lb_info, LV_LABEL_LONG_WRAP);

    // ryk test ===>
    #if 1
    //lv_obj_t * label;

    lv_obj_t * btn1 = lv_btn_create(lv_scr_act());
    // lv_obj_add_event_cb(btn1, event_handler, LV_EVENT_ALL, NULL);
    lv_obj_align(btn1, LV_ALIGN_CENTER, 0, -40);

    label = lv_label_create(btn1);
    lv_label_set_text(label, "Button");
    lv_obj_center(label);

    lv_obj_t * btn2 = lv_btn_create(lv_scr_act());
    // lv_obj_add_event_cb(btn2, event_handler, LV_EVENT_ALL, NULL);
    lv_obj_align(btn2, LV_ALIGN_CENTER, 0, 40);
    lv_obj_add_flag(btn2, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_height(btn2, LV_SIZE_CONTENT);

    label = lv_label_create(btn2);
    lv_label_set_text(label, "Toggle");
    lv_obj_center(label);
    #endif
    // ryk test <===

    SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
    if (!SD_MMC.begin("/sdmmc", true, false, 20000))
    {
        esp_rom_printf("Card Mount Failed\n");
        while (1)
        {
        };
    }

    audio.setPinout(AUDIO_I2S_BCK_IO, AUDIO_I2S_LRCK_IO, AUDIO_I2S_DO_IO);
    audio.setVolume(10); // 0...21 Will need to add a volume setting in the app
    dir = SD_MMC.open(audioDir);
    listDir(SD_MMC, audioDir, 1);
    if (v_fileContent.size() > 0)
    {
        const char *s = (const char *)v_fileContent[v_fileContent.size() - 1];
        esp_rom_printf("playing %s\n", s);
        audio.connecttoFS(SD_MMC, s);
        v_fileContent.pop_back();
    }
}

void loop() {
    lv_task_handler();
    audio.loop();
}

void listDir(fs::FS &fs, const char *dirname, uint8_t levels)
{
  esp_rom_printf("Listing directory: %s\n", dirname);

  File root = fs.open(dirname);
  if (!root)
  {
    esp_rom_printf("Failed to open directory\n");
    return;
  }
  if (!root.isDirectory())
  {
    esp_rom_printf("Not a directory\n");
    return;
  }

  File file = root.openNextFile();
  while (file)
  {
    if (file.isDirectory())
    {
      if (levels)
      {
        listDir(fs, file.path(), levels - 1);
      }
    }
    else
    {
      v_fileContent.insert(v_fileContent.begin(), strdup(file.path()));
    }
    file = root.openNextFile();
  }
  esp_rom_printf("found files : %d\n", v_fileContent.size());
  root.close();
  file.close();
}

void audio_info(const char *info)
{
  esp_rom_printf("info        \n");
  esp_rom_printf(info);

  lv_label_set_text(lb_info, info);
}

void audio_id3data(const char *info)
{ // id3 metadata
  esp_rom_printf("id3data     \n");

  lv_label_set_text(lb_info, info);
}

void audio_eof_mp3(const char *info)
{ // end of file
  esp_rom_printf("eof_mp3     \n");

  lv_label_set_text(lb_info, info);

  if (v_fileContent.size() == 0)
  {
    return;
  }
  const char *s = (const char *)v_fileContent[v_fileContent.size() - 1];
  esp_rom_printf("playing %s\n", s);
  audio.connecttoFS(SD_MMC, s);
  v_fileContent.pop_back();
}
