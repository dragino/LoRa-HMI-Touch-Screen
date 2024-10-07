SET(SOURCES screens/ui_Screen1.c
    screens/ui_Screen2.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    images/ui_img_logo_png.c
    images/ui_img_logo_icon_60x60_png.c
    images/ui_img_logo_text_160x60_png.c
    images/ui_img_product_info_png.c)

add_library(ui ${SOURCES})
