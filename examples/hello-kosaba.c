// hello-kosaba.c -- KosabaOS のユーザープログラムとしての
// Hello World (iwac --kosaba でコンパイルする)
//
// 使い方:
//   iwac --kosaba hello-kosaba.c > hello.elf
//   できた ELF を KosabaOS のファイルシステムに置き、
//   シェルから run で実行すると Hello, World! と出る
//   (カーネルの 'u' キーで uprog.elf として起動してもよい)
//
// ベアメタル版 (hello.c) との違いは舞台の約束:
//   - 出力は UART のレジスタ直書きではなく、組み込みの
//     putc が putc システムコール (番号 1) になる。
//     ユーザーモードからは UART を直接触れないため、
//     uart_putc のような MMIO アクセスはここでは使えない
//   - 終わりは exit システムコール (番号 3) で、
//     シェルやカーネルに制御が戻る
//   - スタックは OS が用意する

// 文字列を出す: 終わりの印 (0) まで1文字ずつ
void kputs(char *s) {
    while (*s != 0) {
        putc(*s);
        s = s + 1;
    }
}

// 入口
int main() {
    kputs("Hello, World!\n");
}
