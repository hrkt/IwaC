/* ================================================================
 * 入力 (v10): ファイルからプログラムを読む
 *
 * 引数が「.c で終わる名前」ならファイルとして開く
 * (開けなければエラー)。それ以外でも、同名のファイルが
 * 開けるならその中身を、開けないなら引数そのものを
 * プログラムとして読む (従来の使い方もそのまま残す)。
 * 読み込んだソースは src_buf に置く。上限は約384KB。
 * v41: 行継続 (バックスラッシュ+改行) を入力層で消す。
 * 設計は doc/splice.md
 * ================================================================ */

static char src_buf[393216];

/* 行継続 (v41): src_buf の中のバックスラッシュ+改行を
 * 消す (C の翻訳フェーズ2)。前へ詰めながら書き換える。
 * 対象は LF と CRLF の両方。文字列やコメントの中でも
 * 消えるのが C の決まり (フェーズ2は字句より前)。
 * ソース末尾のバックスラッシュは相手が無いので残る */
static void remove_splices(void) {
    int r = 0;
    int w = 0;
    while (src_buf[r] != '\0') {
        if (src_buf[r] == '\\' && src_buf[r + 1] == '\n') {
            r += 2;
            continue;
        }
        if (src_buf[r] == '\\' && src_buf[r + 1] == '\r' &&
            src_buf[r + 2] == '\n') {
            r += 3;
            continue;
        }
        src_buf[w] = src_buf[r];
        w++;
        r++;
    }
    src_buf[w] = '\0';
}

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
        if (used >= 393215) {
            close(fd);
            errmsg = "ファイルが大きすぎます (約384KBまで)";
            return -1;
        }
        long n = read(fd, src_buf + used, 393215 - used);
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
    if (r == 1) {
        src_buf[used] = '\0';
        remove_splices();
    }
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
            if (used >= 393215) {
                errmsg = "ファイルが大きすぎます (約384KBまで)";
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
    /* 連結後の全体に一度だけかける (設計参照) */
    remove_splices();
    return 1;
}

/* 引数そのものとして渡されたプログラムを src_buf へ
 * 移す (v41)。行継続の処理をファイル入力と揃えるため。
 * 戻り値: 1 = 移せた、-1 = エラー (errmsg に理由) */
int load_text(char *text) {
    int used = 0;
    while (text[used] != '\0') {
        if (used >= 393215) {
            errmsg = "プログラムが大きすぎます (約384KBまで)";
            return -1;
        }
        src_buf[used] = text[used];
        used++;
    }
    src_buf[used] = '\0';
    remove_splices();
    return 1;
}

