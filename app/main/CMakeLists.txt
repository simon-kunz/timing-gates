idf_component_register(SRCS main.c settings.c console.c beacons.c tagrun.c adv.c radio.c INCLUDE_DIRS "."
                       REQUIRES tlog_timing esp_timer esp_wifi esp_event esp_netif nvs_flash esp_app_format hal bt)
