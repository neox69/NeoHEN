/* The themes that come with NeoHEN, as made by tools/pack_themes.py. */
  .section .rodata
  .global theme_pack
  .global theme_pack_end
  .balign 16
theme_pack:
  .incbin "build/themes.pack"
theme_pack_end:
  .byte 0
