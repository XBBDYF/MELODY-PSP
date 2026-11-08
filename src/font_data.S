/* 点阵字库：由 tools/make_font.py 生成，.incbin 进 EBOOT */
    .section .rodata
    .align 6
    .global font12_blob
    .global font12_blob_end
font12_blob:
    .incbin "src/font12.bin"
font12_blob_end:
    .align 6

    .global font16_blob
    .global font16_blob_end
font16_blob:
    .incbin "src/font16.bin"
font16_blob_end:
    .align 6

    .global font22_blob
    .global font22_blob_end
font22_blob:
    .incbin "src/font22.bin"
font22_blob_end:
    .align 6

    .global font32_blob
    .global font32_blob_end
font32_blob:
    .incbin "src/font32.bin"
font32_blob_end:
    .align 6
