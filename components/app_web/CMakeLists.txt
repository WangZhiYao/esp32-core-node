set(srcs "app_web.c")

# Frontend files to embed (gzip compressed)
set(embed_files "")
set(frontend_dir "${CMAKE_CURRENT_SOURCE_DIR}/frontend")

# Check if compressed front-end files exist
if(EXISTS "${frontend_dir}/index.html.gz")
    list(APPEND embed_files "${frontend_dir}/index.html.gz")
endif()
if(EXISTS "${frontend_dir}/style.css.gz")
    list(APPEND embed_files "${frontend_dir}/style.css.gz")
endif()
if(EXISTS "${frontend_dir}/app.js.gz")
    list(APPEND embed_files "${frontend_dir}/app.js.gz")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json app_event app_espnow app_weather app_mqtt app_network app_storage app_sntp app_display
    EMBED_FILES ${embed_files}
)
