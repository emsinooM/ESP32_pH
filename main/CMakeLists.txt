set(COMPONENT_INCLUDE_DIRS
    inc
    ../config
)

idf_component_register(SRC_DIRS "src"
                    INCLUDE_DIRS "." ${COMPONENT_INCLUDE_DIRS}
                    REQUIRES    coreMQTT
                                azure-sdk-for-c 
                                azure-iot-middleware-freertos 
                                sample-azure-iot 
                                driver 
                                nvs_flash
                                esp_netif
                                esp_wifi
                                esp_https_ota
                                app_update
                                esp_http_server
                                
                    )
