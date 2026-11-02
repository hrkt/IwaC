/* ================================================================
 * 入力 (v10): ファイルからプログラムを読む
 *
 * 引数が「.c で終わる名前」ならファイルとして開く
 * (開けなければエラー)。それ以外でも、同名のファイルが
 * 開けるならその中身を、開けないなら引数そのものを
 * プログラムとして読む (従来の使い方もそのまま残す)。
 * 読み込んだソースは src_buf に置く。上限は約64KB。
 * ================================================================ */

static char src_buf[65536];

/* path が ".c" で終わる名前かどうか */
static int ends_with_c(char *path) {
    int len = 0;
    while (path[len] != '\0')
        len++;
    return len >= 2 && path[len - 2] == '.' && path[len - 1] == 'c';
}

/* ファイルを src_buf へ読み込む。
 * 戻り値: 1 = 読めた、0 = ファイルとして開けなかった
 * (引数そのものとして扱ってよい)、-1 = エラー
 * (errmsg に理由。開けたのに読めなかった、大きすぎる等) */
/* ファイルを src_buf の *used_io の位置から後ろへ読み足す。
 * 戻り値: 1 = 読めた、0 = ファイルとして開けなかった、
 * -1 = エラー (errmsg に理由) */
static int append_file(char *path, int *used_io) {
    int fd = open(path, 0);
    if (fd < 0)
        return 0;
    int used = *used_io;
    for (;;) {
        if (used >= 65535) {
            close(fd);
            errmsg = "ファイルが大きすぎます (約64KBまで)";
            return -1;
        }
        long n = read(fd, src_buf + used, 65535 - used);
        if (n < 0) {
            close(fd);
            /* ディレクトリなど、開けても読めないものは
             * 「ファイルではなかった」として扱う */
            return 0;
        }
        if (n == 0)
            break;
        used += (int)n;
    }
    close(fd);
    *used_io = used;
    return 1;
}

static int load_file(char *path) {
    int used = 0;
    int r = append_file(path, &used);
    if (r == 1)
        src_buf[used] = '\0';
    return r;
}

/* 複数ファイル入力 (v17) で開けなかったファイルの名前 */
static char *load_failed_name;

/* 複数のファイルを順につないで src_buf へ読む (v17)。
 * 境目には改行を1つ挟む (トークンがくっつかないように)。
 * こちらは全ファイルが必須: 1つでも開けなければ 0 を
 * 返し、名前を load_failed_name に置く。読みの失敗は -1 */
static int load_files(char **paths, int count) {
    int used = 0;
    load_failed_name = 0;
    for (int i = 0; i < count; i++) {
        if (used > 0) {
            if (used >= 65535) {
                errmsg = "ファイルが大きすぎます (約64KBまで)";
                return -1;
            }
            src_buf[used++] = '\n';
        }
        int r = append_file(paths[i], &used);
        if (r == 0) {
            load_failed_name = paths[i];
            return 0;
        }
        if (r < 0)
            return -1;
    }
    src_buf[used] = '\0';
    return 1;
}

