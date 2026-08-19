idf_component_register(SRCS "main.cpp" "mesh_codec.cpp" "mavlink_min.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES mbedtls nvs_flash driver esp_timer)
