idf_component_register(SRCS "main.cpp" "mesh_codec.cpp" "mesh_frame.cpp"
                            "mavlink_min.cpp" "failsafe.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES mbedtls nvs_flash driver esp_timer)
