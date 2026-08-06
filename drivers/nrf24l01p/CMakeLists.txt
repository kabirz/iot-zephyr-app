zephyr_library()

zephyr_library_sources_ifdef(CONFIG_NRF24L01P nrf24l01p.c)
zephyr_library_sources_ifdef(CONFIG_NRF24L01P_CRYPT nrf24_crypt.c)
zephyr_include_directories(.)
