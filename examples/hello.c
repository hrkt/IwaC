// hello.c -- iwac でコンパイルする Hello World
//
// 使い方:
//   iwac hello.c > hello.bin
//   (できた hello.bin を QEMU の -kernel で読み込むと
//    シリアルに Hello, World! と出る)
//
// stdio.h のようなものは無いので、出力は UART へ
// 直接書く関数を自分で定義する (KosabaOS の uart.c と
// 同じ形)。

// 1文字出す: UART の送信窓口が空くまで待ってから書く
void uart_putc(char c) {
    while ((*0x10000005 & 32) == 0) {
    }
    *0x10000000 = c;
}

// 文字列を出す: 0 (終わりの印) まで1文字ずつ
void uart_puts(char *s) {
    while (*s != 0) {
        uart_putc(*s);
        s = s + 1;
    }
}

// 入口 (v12 から main の形で書ける)
int main() {
    uart_puts("Hello, World!\n");
}
