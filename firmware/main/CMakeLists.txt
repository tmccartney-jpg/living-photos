idf_component_register(
    SRCS
        "main.c"
        "hmmd.c"
        "diag.c"
        "presence.c"
        "sdcard.c"
        "frame.c"
        "font_caption.c"
        "../components/waveshare_rgb_lcd_port.c"
    INCLUDE_DIRS
        "."
        "../components")

idf_build_get_property(build_components BUILD_COMPONENTS)
if("lvgl" IN_LIST build_components)
    idf_component_get_property(lvgl_lib lvgl COMPONENT_LIB)
elseif("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
endif()

if(lvgl_lib)
    target_compile_options(${lvgl_lib} PRIVATE -Wno-format)
    # LVGL objects in PSRAM (see lp_lv_mem.h): frees internal RAM for Wi-Fi.
    target_include_directories(${lvgl_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_definitions(${lvgl_lib} PRIVATE
        "LV_MEM_CUSTOM_INCLUDE=\"lp_lv_mem.h\""
        "LV_MEM_CUSTOM_ALLOC=lp_lv_malloc"
        "LV_MEM_CUSTOM_REALLOC=lp_lv_realloc"
        "LV_MEM_CUSTOM_FREE=lp_lv_free")
endif()
