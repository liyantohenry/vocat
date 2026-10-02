# ESP-IDF 5.5 environment (ESP32-S3 only; riscv/gdb tools intentionally not installed)
export IDF_PATH=$HOME/esp/esp-idf
export IDF_TOOLS_PATH=$HOME/.espressif
export IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf5.5_py3.11_env
export ESP_ROM_ELF_DIR=$HOME/.espressif/tools/esp-rom-elfs/20260528/
export OPENOCD_SCRIPTS=$HOME/.espressif/tools/openocd-esp32/v0.12.0-esp32-20260831/openocd-esp32/share/openocd/scripts
export PATH=$IDF_PYTHON_ENV_PATH/bin:$IDF_PATH/tools:$HOME/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin:$HOME/.espressif/tools/esp32ulp-elf/2.38_20240113/esp32ulp-elf/bin:$HOME/.espressif/tools/openocd-esp32/v0.12.0-esp32-20260831/openocd-esp32/bin:$PATH
