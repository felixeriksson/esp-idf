# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# (Not part of the boilerplate)
# This example uses an extra component for common functions such as Wi-Fi and Ethernet connection.
set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common
						 "C:/Users/felix/Desktop/esp/esp-dev-kits/esp32-s2-kaluga-1/components/board"
                         "C:/Users/felix/Desktop/esp/esp-dev-kits/esp32-s2-kaluga-1/components/cam"
                         "C:/Users/felix/Desktop/esp/esp-dev-kits/esp32-s2-kaluga-1/components/jpeg"
                         "C:/Users/felix/Desktop/esp/esp-dev-kits/esp32-s2-kaluga-1/components/sensors"
						 "C:/Users/felix/Desktop/esp/esp-dev-kits/esp32-s2-kaluga-1/components/lcd")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-http-client-example)
