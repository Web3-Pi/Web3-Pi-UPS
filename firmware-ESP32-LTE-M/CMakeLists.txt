# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
# Per-netif MIB2 octet counters (lwIP PPP counts ifin/ifoutoctets) — feeds
# bytes_tx/bytes_rx in net.status telemetry. Must be a GLOBAL define: it
# changes the struct netif layout, so every unit including lwIP headers has
# to agree on it (a per-component define would be an ABI mismatch).
idf_build_set_property(COMPILE_OPTIONS "-DMIB2_STATS=1" APPEND)
project(firmware-ESP32-LTE-M)
