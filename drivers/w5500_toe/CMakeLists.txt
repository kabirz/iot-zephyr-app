zephyr_library()

zephyr_library_sources_ifdef(CONFIG_W5500_TOE w5500_toe.c)
