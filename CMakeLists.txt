# CMakeLists.txt principale del progetto NetworkWatcher

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Include componenti locali (esp_littlefs, web_server, ecc.)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

project(NetworkWatcher)

# Genera l'immagine LittleFS per la partizione "storage"
# a partire dalla cartella "data/" e la include nel flash.
littlefs_create_partition_image(storage ${CMAKE_CURRENT_SOURCE_DIR}/data FLASH_IN_PROJECT)