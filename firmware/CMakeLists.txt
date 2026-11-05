cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(audiolink)

target_sources(app PRIVATE
  src/audio_out.c
  src/control.c
  src/dac.c
  src/eq.c
  src/lc3_rx.c
  src/le_audio.c
  src/leds.c
  src/main.c
  src/test_signal.c
  src/usb_audio.c
)
