// This file was generated by SquareLine Studio
// SquareLine Studio version: SquareLine Studio 1.4.2
// LVGL version: 8.3.11
// Project name: SquareLine_Project

#ifndef _SQUARELINE_PROJECT_UI_H
#define _SQUARELINE_PROJECT_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

#include "ui_helpers.h"
#include "ui_events.h"

// SCREEN: ui_Screen1
void ui_Screen1_screen_init(void);
extern lv_obj_t * ui_Screen1;
void ui_event_Button1(lv_event_t * e);
extern lv_obj_t * ui_Button1;
extern lv_obj_t * ui_Label1;
extern lv_obj_t * ui_Image1;
extern lv_obj_t * ui_Label3;
extern lv_obj_t * ui_Label5;
extern lv_obj_t * ui_Image3;
extern lv_obj_t * ui_Image4;
// SCREEN: ui_Screen2
void ui_Screen2_screen_init(void);
extern lv_obj_t * ui_Screen2;
void ui_event_Button2(lv_event_t * e);
extern lv_obj_t * ui_Button2;
extern lv_obj_t * ui_Label2;
extern lv_obj_t * ui_Image2;
extern lv_obj_t * ui____initial_actions0;


LV_IMG_DECLARE(ui_img_logo_png);    // assets/logo.png
LV_IMG_DECLARE(ui_img_logo_icon_60x60_png);    // assets/logo_icon_60x60.png
LV_IMG_DECLARE(ui_img_logo_text_160x60_png);    // assets/logo_text_160x60.png
LV_IMG_DECLARE(ui_img_product_info_png);    // assets/product_info.png






void ui_init(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
