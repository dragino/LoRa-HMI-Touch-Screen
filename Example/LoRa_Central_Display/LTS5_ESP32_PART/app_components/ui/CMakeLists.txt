cmake_minimum_required(VERSION 3.12.4)

idf_component_register(
    SRC_DIRS
        "ui"
        "ui/components"
        "ui/fonts"
        "ui/screens"
	"ui/images"

    INCLUDE_DIRS
        "ui"
        "ui/components"
        "ui/fonts"
        "ui/screens"
	"ui/images"
        
    REQUIRES
        lvgl
        )
        

