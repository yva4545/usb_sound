cmake_minimum_required(VERSION 3.22)
project(audio_host_tests LANGUAGES C)
enable_testing()

add_executable(audio_host_tests test_audio.c)
target_compile_features(audio_host_tests PRIVATE c_std_11)
target_compile_options(audio_host_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(audio_host_tests PRIVATE
    stubs
    ../Core/Inc
    ../Middlewares/ST/STM32_USB_Device_Library/Core/Inc
    ../Middlewares/ST/STM32_USB_Device_Library/Class/AUDIO/Inc
)
add_test(NAME audio_capture_and_usb COMMAND audio_host_tests)
