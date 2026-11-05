idf_component_register(
    SRCS
        "src/bitstream.c"
        "src/buffers.c"
        "src/dct32.c"
        "src/dequant.c"
        "src/dqchan.c"
        "src/huffman.c"
        "src/hufftabs.c"
        "src/imdct.c"
        "src/mp3dec.c"
        "src/mp3tabs.c"
        "src/polyphase.c"
        "src/scalfact.c"
        "src/stproc.c"
        "src/subband.c"
        "src/trigtabs.c"
    INCLUDE_DIRS
        "src"
)

# Helix MP3 解码器（RealNetworks RSFDL 免费授权，源自 ESP8266Audio 的移植）。
# 纯 C 定点实现，无平台相关汇编（assembly.h 里 XSCALE_ARM 分支不启用），
# 在 ESP32-S3 上以 C 编译即可，无需额外编译选项。
