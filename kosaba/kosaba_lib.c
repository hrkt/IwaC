/* kosaba_lib.c -- iwac を KosabaOS のユーザープログラムとして
 * 動かすための「ホストの代役」(v15)
 *
 * iwac.c が外に頼るのは write / open / read / close の4つだけ
 * (宣言は iwac.c 側にある)。ホストではそれが OS の機能だった。
 * ここでは同じ名前・同じ約束の関数を、KosabaOS のシステム
 * コール (doc/syscalls.md v1) で実装し直す:
 *
 *   open(path)  -> システムコール 4 (open、読み用 fd)
 *   read        -> システムコール 5
 *   close       -> システムコール 6 (読み書き共通)
 *   write(fd=2) -> エラー表示。コンソールへ putc (番号 1) で
 *                  1文字ずつ出す
 *   write(fd=1) -> コンパイル結果の出力先。OS では標準出力の
 *                  リダイレクトが無いので、「hello.elf という
 *                  ファイル」と決め打ちする。create (番号 9) で
 *                  作り、write (番号 10) で書く。1バイトずつの
 *                  呼び出しが多いので、ここでバッファに貯めて
 *                  まとめて書く (kosaba_flush で書き切る)
 *
 * iwac.c には一切手を入れない。リンクする相手を差し替える
 * だけで、同じソースがホストでも OS でも動く。
 */

/* システムコールを1つ呼ぶ道具。番号は a7、引数は a0..a2、
 * 戻り値は a0 (KosabaOS の約束) */
static long syscall3(long num, long a0, long a1, long a2) {
    register long r_a0 __asm__("a0") = a0;
    register long r_a1 __asm__("a1") = a1;
    register long r_a2 __asm__("a2") = a2;
    register long r_a7 __asm__("a7") = num;
    __asm__ volatile("ecall"
                     : "+r"(r_a0)
                     : "r"(r_a1), "r"(r_a2), "r"(r_a7)
                     : "memory");
    return r_a0;
}

/* 番号の約束 (KosabaOS doc/syscalls.md v1) */
static int SYS_PUTC = 1;
static int SYS_OPEN = 4;
static int SYS_READ = 5;
static int SYS_CLOSE = 6;
static int SYS_CREATE = 9;
static int SYS_WRITE = 10;

/* ---- 読み側: そのまま対応する ---- */

int open(char *path, int flags) {
    (void)flags;  /* OS の open は読み専用で、フラグは無い */
    return (int)syscall3(SYS_OPEN, (long)path, 0, 0);
}

long read(int fd, char *buf, long len) {
    return syscall3(SYS_READ, fd, (long)buf, len);
}

int close(int fd) {
    return (int)syscall3(SYS_CLOSE, fd, 0, 0);
}

/* ---- 書き側: fd=2 はコンソール、fd=1 は hello.elf ---- */

static int out_fd = -1;          /* 出力ファイルの
                                    書き用 fd */
/* 出力ファイルの名前。iwac 本体 (01-head.c) が持ち、
 * --out で上書きされる。無指定なら hello.elf (v34) */
extern char *kosaba_out_name;
static char out_buf[8192];       /* まとめて書くための貯め場所 */
static long out_used;            /* 貯まっているバイト数 */

/* 貯めた分をファイルへ書き切る (足りない分は繰り返し書く。
 * OS の write は「入る分だけ書いて書けた数を返す」約束) */
static void flush_out(void) {
    long done = 0;
    while (done < out_used) {
        long n = syscall3(SYS_WRITE, out_fd, (long)(out_buf + done),
                          out_used - done);
        if (n <= 0)
            break;  /* 書けない (容量不足など)。ここでは諦める */
        done += n;
    }
    out_used = 0;
}

/* _start が main の終了後に呼ぶ: 残りを書き切って閉じる */
void kosaba_flush(void) {
    if (out_fd >= 0) {
        flush_out();
        syscall3(SYS_CLOSE, out_fd, 0, 0);
        out_fd = -1;
    }
}

long write(int fd, char *buf, long len) {
    if (fd == 2) {
        /* エラー表示はコンソールへ (putc を1文字ずつ) */
        for (long i = 0; i < len; i++)
            syscall3(SYS_PUTC, (unsigned char)buf[i], 0, 0);
        return len;
    }
    if (fd == 1) {
        if (out_fd < 0) {
            /* 最初の1バイトが来た時点でファイルを作る。
             * コンパイルが失敗したら出力は始まらないので、
             * 半端なファイルは残らない */
            out_fd = (int)syscall3(SYS_CREATE,
                                    (long)(kosaba_out_name ?
                                           kosaba_out_name :
                                           "hello.elf"), 0, 0);
            if (out_fd < 0)
                return -1;
        }
        for (long i = 0; i < len; i++) {
            out_buf[out_used++] = buf[i];
            if (out_used == (long)sizeof(out_buf))
                flush_out();
        }
        return len;
    }
    return -1;
}

/* ---- コンパイラが時々呼び出すメモリ道具 ----
 * -nostdlib でリンクするため、gcc が内部で使う memcpy 等も
 * ここで用意する (単純な1バイトずつの実装で十分) */

void *memcpy(void *dst, const void *src, unsigned long n) {
    char *d = (char *)dst;
    const char *s = (const char *)src;
    for (unsigned long i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

void *memset(void *dst, int c, unsigned long n) {
    char *d = (char *)dst;
    for (unsigned long i = 0; i < n; i++)
        d[i] = (char)c;
    return dst;
}

void *memmove(void *dst, const void *src, unsigned long n) {
    char *d = (char *)dst;
    const char *s = (const char *)src;
    if (d < s) {
        for (unsigned long i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        for (long i = (long)n - 1; i >= 0; i--)
            d[i] = s[i];
    }
    return dst;
}
