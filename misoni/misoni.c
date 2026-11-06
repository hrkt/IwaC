/* misoni.c — Misoni 本体 (KosabaOS 上で動くドライバ)
 *
 * マニフェスト (misoni.txt) を読み、obj 行ごとに
 * iwac.elf を exec し、最後に nori.elf を exec して
 * out の名前の ELF を作る。形式はホスト側の
 * misoni/misoni.sh と共通 (doc/linking.md 段5)。
 *
 * OS 側の前提: exec の多段化 (Nish → misoni →
 * iwac / nori の2段目が要る) と、出力名の指定
 * (iwac / nori の --out、v34)。
 *
 * exec の引数文字列は「プログラム名から始まる
 * 行全体」を渡すのが OS の約束 (名前は argv[0]
 * になる。空なら名前だけが argv[0] になる)。
 *
 * 自分では fd 1 に何も書かない (書くと rt が
 * hello.elf を作ってしまう)。進捗は fd 2 へ。
 * iwac の言語 (部分集合) で書く。外部関数は
 * open / read / close / write だけを手で宣言し、
 * exec / create などは syscall3 で直接叩く。
 */

long write(int fd, char *buf, long len);
int open(char *path, int flags);
long read(int fd, char *buf, long len);
int close(int fd);

/* ---- 小物 ---- */

int slen(char *s) {
    int n;
    n = 0;
    while (s[n])
        n = n + 1;
    return n;
}

int seq(char *a, char *b) {
    int i;
    i = 0;
    while (a[i] && a[i] == b[i])
        i = i + 1;
    return a[i] == b[i];
}

void say(char *s) { write(2, s, slen(s)); }

void say_n(long n) {
    char b[16];
    int i;
    if (n == 0) {
        say("0");
        return;
    }
    i = 0;
    while (n > 0) {
        b[i] = 48 + (n - (n / 10) * 10);
        i = i + 1;
        n = n / 10;
    }
    while (i > 0) {
        i = i - 1;
        write(2, b + i, 1);
    }
}

/* ---- マニフェスト ---- */

char mbuf[8192];        /* マニフェスト全文 */
int mlen;               /* その長さ */
char out_name[128];     /* out 行の名前 */
int have_out;

/* obj 行の控え: 名前1つ + ソースの並び。
 * ソースの並びは srcs_pool に空白区切りでつないで置く */
char srcs_pool[4096];
char *obj_name[16];
char *obj_srcs[16];
int nobj;

/* 行 [ls, le) の単語を ws に切り分ける。
 * 切り分けはその場で空白を 0 に潰す (mbuf は使い捨て)。
 * 戻り値 = 単語の数 */
char *words[24];
int split_line(int ls, int le) {
    int nw;
    int i;
    int start;
    nw = 0;
    i = ls;
    while (i < le) {
        while (i < le && (mbuf[i] == 32 || mbuf[i] == 9))
            i = i + 1;
        if (i >= le)
            break;
        start = i;
        while (i < le && mbuf[i] != 32 && mbuf[i] != 9)
            i = i + 1;
        mbuf[i] = 0;
        i = i + 1;
        if (nw < 24) {
            words[nw] = mbuf + start;
            nw = nw + 1;
        }
    }
    return nw;
}

/* 文字列 t を dst の pos から足し、新しい pos を返す */
int append_str(char *dst, int pos, char *t) {
    int i;
    i = 0;
    while (t[i]) {
        dst[pos] = t[i];
        pos = pos + 1;
        i = i + 1;
    }
    dst[pos] = 0;
    return pos;
}

int load_manifest(char *path) {
    int fd;
    long n;
    fd = open(path, 0);
    if (fd < 0) {
        say("misoni: マニフェストが開けません: ");
        say(path);
        say("\n");
        return 0;
    }
    n = read(fd, mbuf, 8192);
    close(fd);
    if (n < 0) {
        say("misoni: マニフェストが読めません\n");
        return 0;
    }
    mlen = n;
    /* 行ごとに解釈する */
    int ls;
    int pos;
    ls = 0;
    pos = 0;
    have_out = 0;
    nobj = 0;
    while (pos <= mlen) {
        int le;
        le = pos;
        while (le < mlen && mbuf[le] != 10)
            le = le + 1;
        if (le > ls) {
            int nw;
            nw = split_line(ls, le);
            char *w0;
            w0 = words[0];
            if (nw >= 1 && w0[0] != 35) {  /* # は注釈 */
                if (seq(words[0], "out") && nw == 2) {
                    char *w1;
                    int p;
                    w1 = words[1];
                    p = 0;
                    while (w1[p] && p < 127) {
                        out_name[p] = w1[p];
                        p = p + 1;
                    }
                    out_name[p] = 0;
                    have_out = 1;
                } else if (seq(words[0], "obj") && nw >= 3) {
                    if (nobj < 16) {
                        obj_name[nobj] = words[1];
                        /* ソースの並びを1つの文字列に
                         * つないで控える */
                        char *dst;
                        dst = srcs_pool + nobj * 256;
                        int p;
                        p = 0;
                        for (int k = 2; k < nw; k++) {
                            if (k > 2)
                                p = append_str(dst, p, " ");
                            p = append_str(dst, p, words[k]);
                        }
                        obj_srcs[nobj] = dst;
                        nobj = nobj + 1;
                    }
                }
            }
        }
        if (le >= mlen)
            break;
        ls = le + 1;
        pos = ls;
    }
    return 1;
}

/* ---- 駆動 ---- */

char args[512];         /* exec の引数文字列 (OS の上限内) */

/* 1段コンパイル: iwac.elf を exec する */
int run_iwac(int i) {
    int p;
    long st;
    p = 0;
    p = append_str(args, p, "iwac.elf --kosaba --object --out ");
    p = append_str(args, p, obj_name[i]);
    p = append_str(args, p, " ");
    p = append_str(args, p, obj_srcs[i]);
    say("misoni: iwac ");
    say(args);
    say("\n");
    st = syscall3(8, "iwac.elf", args, 0);
    if (st != 0) {
        say("misoni: コンパイルに失敗しました (status ");
        say_n(st);
        say("): ");
        say(obj_name[i]);
        say("\n");
        return 0;
    }
    return 1;
}

/* リンク: nori.elf を exec する */
int run_nori(void) {
    int p;
    long st;
    p = 0;
    p = append_str(args, p, "nori.elf --out ");
    p = append_str(args, p, out_name);
    for (int i = 0; i < nobj; i++) {
        p = append_str(args, p, " ");
        p = append_str(args, p, obj_name[i]);
    }
    say("misoni: nori ");
    say(args);
    say("\n");
    st = syscall3(8, "nori.elf", args, 0);
    if (st != 0) {
        say("misoni: リンクに失敗しました (status ");
        say_n(st);
        say(")\n");
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    char *mpath;
    mpath = "misoni.txt";
    if (argc >= 2)
        mpath = argv[1];
    if (!load_manifest(mpath))
        return 1;
    if (!have_out || nobj == 0) {
        say("misoni: out 行と obj 行が要ります\n");
        return 1;
    }
    for (int i = 0; i < nobj; i++) {
        if (!run_iwac(i))
            return 1;
    }
    if (!run_nori())
        return 1;
    say("misoni: できました: ");
    say(out_name);
    say("\n");
    return 0;
}
