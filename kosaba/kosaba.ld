/* kosaba.ld -- iwac を KosabaOS 用にリンクする配置指示書 (v15)
 *
 * 中身は KosabaOS 側の user.ld と同じ (ユーザープログラムは
 * 仮想アドレス 0x40000000 に置かれ、入口が先頭、という OS との
 * 約束)。OS 側の配置の約束が変わったら、こちらも合わせる。
 */

/* user.ld -- ユーザープログラム用の配置指示書
 *
 * カーネルの kernel.ld と対になる、ユーザープログラム側の約束。
 * プログラムは仮想アドレス 0x40000000 に置かれる前提で組み立てる
 * (その住所に置く仕事はカーネル側がする)。
 */

ENTRY(_start)

SECTIONS {
  . = 0x40000000;

  /* 入口 (_start の .text.start) を必ず先頭に置く。
   * カーネルは配置先の先頭アドレスから実行を始めるため */
  .text : { *(.text.start) *(.text) }
  .rodata : { *(.rodata) }
  .data : { *(.data) }
  .bss : { *(.bss) *(COMMON) }
}
