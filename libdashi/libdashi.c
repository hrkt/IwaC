/* libdashi.c — 共通関数の小さいライブラリ (libdashi)
 *
 * 「Libc とまでは言わない」小物集。設計は
 * doc/libdashi.md。iwac の言語で書き、外部関数は
 * write だけを手で宣言する。
 *
 * 消費者は、使う関数のプロトタイプを自分の
 * ソースの頭に書き、入力にこのファイルを足す
 * (一括) か、オブジェクト化して nori でつなぐ。
 *
 * rt (kosaba_rt.c) には依存しない。rt の側も
 * libdashi には依存しない (自己完結同士)。
 */

long write(int fd, char *buf, long len);

/* ---- 文字列 ---- */

int dashi_strlen(char *s) {
    int n;
    n = 0;
    while (s[n])
        n = n + 1;
    return n;
}

int dashi_streq(char *a, char *b) {
    int i;
    i = 0;
    while (a[i] && a[i] == b[i])
        i = i + 1;
    return a[i] == b[i];
}

/* dst へ src を写し (終わりの 0 も)、写した
 * 長さ (0 を除く) を返す */
int dashi_strcpy(char *dst, char *src) {
    int i;
    i = 0;
    while (src[i]) {
        dst[i] = src[i];
        i = i + 1;
    }
    dst[i] = 0;
    return i;
}

/* dst の pos の位置から src を足し、足した後の
 * pos を返す。dst は 0 で終わっている前提で、
 * 続きも 0 で終わらせる */
int dashi_append(char *dst, int pos, char *src) {
    int i;
    i = 0;
    while (src[i]) {
        dst[pos] = src[i];
        pos = pos + 1;
        i = i + 1;
    }
    dst[pos] = 0;
    return pos;
}

/* ---- 数値 → 文字列 (バッファへ書く) ---- */

/* v を10進で buf へ書き、書いた長さを返す。
 * buf には 24 バイト以上あること (符号＋20桁＋0) */
int dashi_dec(char *buf, long v) {
    char tmp[24];
    int n;
    int i;
    int neg;
    neg = 0;
    if (v < 0) {
        neg = 1;
        v = -v;
    }
    n = 0;
    if (v == 0) {
        tmp[n] = 48;
        n = n + 1;
    }
    while (v > 0) {
        tmp[n] = 48 + (v - (v / 10) * 10);
        n = n + 1;
        v = v / 10;
    }
    i = 0;
    if (neg) {
        buf[i] = 45;
        i = i + 1;
    }
    while (n > 0) {
        n = n - 1;
        buf[i] = tmp[n];
        i = i + 1;
    }
    buf[i] = 0;
    return i;
}

/* v を16進 (小文字、ゼロ詰めなし) で buf へ
 * 書き、書いた長さを返す。負数は扱わない
 * (ビット列として読めるよう、符号なしで進める) */
int dashi_hex(char *buf, long v) {
    char tmp[20];
    int n;
    int i;
    unsigned long u;
    u = v;
    n = 0;
    if (u == 0) {
        tmp[n] = 48;
        n = n + 1;
    }
    while (u > 0) {
        int d;
        d = u - (u / 16) * 16;
        if (d < 10)
            tmp[n] = 48 + d;
        else
            tmp[n] = 87 + d;    /* 'a' - 10 */
        n = n + 1;
        u = u / 16;
    }
    i = 0;
    while (n > 0) {
        n = n - 1;
        buf[i] = tmp[n];
        i = i + 1;
    }
    buf[i] = 0;
    return i;
}

/* ---- 文字列 → 数値 ---- */

/* 数字が続く間だけ読む。先頭の空白と符号も見る */
long dashi_atoi(char *s) {
    long v;
    int i;
    int neg;
    i = 0;
    while (s[i] == 32 || s[i] == 9)
        i = i + 1;
    neg = 0;
    if (s[i] == 45) {
        neg = 1;
        i = i + 1;
    } else if (s[i] == 43) {
        i = i + 1;
    }
    v = 0;
    while (s[i] >= 48 && s[i] <= 57) {
        v = v * 10 + (s[i] - 48);
        i = i + 1;
    }
    if (neg)
        v = -v;
    return v;
}

/* 16進の1文字 → 値。違えば -1 */
int dashi_hexval(int c) {
    if (c >= 48 && c <= 57)
        return c - 48;
    if (c >= 97 && c <= 102)
        return c - 87;          /* 'a' - 10 */
    if (c >= 65 && c <= 70)
        return c - 55;          /* 'A' - 10 */
    return -1;
}

/* ---- 出力 (write の上) ---- */

void dashi_puts(int fd, char *s) {
    write(fd, s, dashi_strlen(s));
}

void dashi_putdec(int fd, long v) {
    char buf[24];
    int n;
    n = dashi_dec(buf, v);
    write(fd, buf, n);
}

void dashi_puthex(int fd, long v) {
    char buf[20];
    int n;
    n = dashi_hex(buf, v);
    write(fd, buf, n);
}
