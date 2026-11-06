/* kosaba_rt.c -- iwac の言語で書いた KosabaOS 用ランタイム (v27)
 *
 * iwac 本体が外に頼るのは write / open / read / close の4つ
 * だけ (宣言は src/01-head.c にある)。GCC 版では kosaba/
 * kosaba_lib.c がその相手をしてきた (ホストの代役)。
 * これは同じ約束の実装を iwac 自身の言語で書き直したもの:
 *
 *   open(path)  -> システムコール 4 (open、読み用 fd)
 *   read        -> システムコール 5
 *   close       -> システムコール 6
 *   write(fd=2) -> エラー表示。putc (番号 1) でコンソールへ
 *   write(fd=1) -> コンパイル結果の出力先。OS に標準出力の
 *                  リダイレクトは無いので、kosaba_lib.c と
 *                  同じ約束で「hello.elf というファイル」に
 *                  する。create (番号 9) で作り、write
 *                  (番号 10) で書く。1バイトずつの呼び出しが
 *                  多いので、ここで貯めてまとめて書く。
 *                  貯めた分の書き切りは kosaba_flush が担う
 *                  (入口コードが main の後に呼ぶ約束、v27)
 *
 * iwac 本体の入力にこのファイルを一緒に渡すと、外部関数が
 * 全部そろって、iwac が iwac 自身をコンパイルできる
 * (セルフホスト)。システムコールは組み込みの syscall3
 * で呼ぶ。
 *
 * 並びの約束: このファイルは本体の断片 (src/0*.c) の
 * 後ろに置くこと。宣言 (プロトタイプ) より先に定義が
 * 来ると「同じ名前の関数が二つある」と言われる。
 *
 * 確認用の仕掛け: IWAC_RT_DIGEST が定義されていると、
 * fd=1 へ出たバイト列の digest (バイト数、合計、ハッシュ)
 * を kosaba_flush がコンソールに出す。OS のファイルには
 * 入り切らない大きな出力 (セルフコンパイルの結果など) の
 * 「中身が正しいか」を、バイト列そのものを取り出さずに
 * 確かめるために使う。通常のビルドでは定義しない。
 */

char *kosaba_out_name;  /* 出力ファイルの名前。
                         * プログラム側 (iwac の --out 等)
                         * が入れなければ hello.elf (v34) */
int out_fd;       /* 出力ファイルの書き用 fd */
int out_started;  /* 0 = まだ create していない */
long out_used;    /* バッファに貯まっているバイト数 */
char out_buf[8192];

#ifdef IWAC_RT_DIGEST
long dg_sum;   /* 出力バイトの合計 */
long dg_hash;  /* 出力バイトのハッシュ (sdbm 風) */
long dg_len;   /* 出力バイト数 */
#endif

/* ---- 読み側: そのまま対応する ---- */

int open(char *path, int flags) {
    /* OS の open は読み専用で、フラグは無い */
    return syscall3(4, path, 0, 0);
}

long read(int fd, char *buf, long len) {
    return syscall3(5, fd, buf, len);
}

int close(int fd) {
    return syscall3(6, fd, 0, 0);
}

/* ---- 書き側 ---- */

/* 貯めた分をファイルへ書き切る (足りない分は繰り返し
 * 書く。OS の write は「入る分だけ書いて書けた数を返す」
 * 約束) */
void flush_out(void) {
    long done;
    done = 0;
    while (done < out_used) {
        long n;
        n = syscall3(10, out_fd, out_buf + done, out_used - done);
        if (n <= 0)
            break;  /* 書けない (容量不足など)。ここでは諦める */
        done = done + n;
    }
    out_used = 0;
}

#ifdef IWAC_RT_DIGEST
/* digest 表示用の小さな道具 (コンソールへ putc で出す) */
void rt_putc(int c) {
    syscall3(1, c, 0, 0);
}

void rt_puts(char *s) {
    long i;
    i = 0;
    while (s[i] != 0) {
        rt_putc(s[i]);
        i = i + 1;
    }
}

void rt_puthex(long v) {
    int sh;
    for (sh = 60; sh >= 0; sh = sh - 4) {
        int d;
        d = (v >> sh) & 15;
        if (d < 10)
            rt_putc(48 + d);
        else
            rt_putc(87 + d);
    }
}

void rt_putdec(long v) {
    char tmp[24];
    int n;
    n = 0;
    if (v == 0) {
        rt_putc(48);
        return;
    }
    while (v > 0) {
        tmp[n] = 48 + (v % 10);
        n = n + 1;
        v = v / 10;
    }
    while (n > 0) {
        n = n - 1;
        rt_putc(tmp[n]);
    }
}
#endif

/* 入口コードが main の終了後に呼ぶ (v27 の約束):
 * 残りを書き切って閉じる */
void kosaba_flush(void) {
    if (out_started) {
        flush_out();
        syscall3(6, out_fd, 0, 0);
        out_started = 0;
    }
#ifdef IWAC_RT_DIGEST
    rt_puts("digest len=");
    rt_putdec(dg_len);
    rt_puts(" sum=");
    rt_puthex(dg_sum);
    rt_puts(" hash=");
    rt_puthex(dg_hash);
    rt_putc(10);
#endif
}

long write(int fd, char *buf, long len) {
    long i;
    if (fd == 2) {
        /* エラー表示はコンソールへ (putc を1文字ずつ) */
        for (i = 0; i < len; i++)
            syscall3(1, buf[i], 0, 0);
        return len;
    }
    if (fd == 1) {
        if (out_started == 0) {
            /* 最初の1バイトが来た時点でファイルを作る。
             * コンパイルが失敗したら出力は始まらないので、
             * 半端なファイルは残らない */
            out_fd = syscall3(9, kosaba_out_name ?
                              kosaba_out_name : "hello.elf",
                              0, 0);
            if (out_fd < 0)
                return -1;
            out_started = 1;
        }
        for (i = 0; i < len; i++) {
            out_buf[out_used] = buf[i];
#ifdef IWAC_RT_DIGEST
            dg_sum = dg_sum + buf[i];
            dg_hash = dg_hash * 65599 + buf[i];
            dg_len = dg_len + 1;
#endif
            out_used = out_used + 1;
            if (out_used == 8192)
                flush_out();
        }
        return len;
    }
    return -1;
}
