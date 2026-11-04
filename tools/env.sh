# ESP-IDF v6.0.2 环境（Git Bash / MSYS）
export IDF_PATH="/d/software/esp-idf/.espressif/v6.0.2/esp-idf"
export IDF_TOOLS_PATH="/c/Espressif"
export IDF_PYTHON_ENV_PATH="/c/Espressif/tools/python/v6.0.2/venv"

# 基础工具（Git Bash usr/bin 必须在前，否则 dirname/ls 等缺失）
export PATH="/c/Users/admin/.workbuddy/binaries/PortableGit/versions/1.2.0/usr/bin:/c/Windows/System32:/c/Windows:$PATH"

# IDF 工具链
export PATH="$IDF_PYTHON_ENV_PATH/Scripts:$PATH"
export PATH="$IDF_TOOLS_PATH/tools/cmake/3.30.2/bin:$PATH"
export PATH="$IDF_TOOLS_PATH/tools/ninja/1.12.1:$PATH"
export PATH="$IDF_TOOLS_PATH/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin:$PATH"
export PATH="$IDF_TOOLS_PATH/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin:$PATH"
export PATH="$IDF_TOOLS_PATH/tools/idf-exe/1.0.3:$PATH"

export ESP_IDF_VERSION="v6.0.2"
export IDF_PYTHON_LEGACY=0
