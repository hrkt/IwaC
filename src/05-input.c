/* ================================================================
 * 入力 (v10): ファイルからプログラムを読む
 *
 * 引数が「.c で終わる名前」ならファイルとして開く
 * (開けなければエラー)。それ以外でも、同名のファイルが
 * 開けるならその中身を、開けないなら引数そのものを
 * プログラムとして読む (従来の使い方もそのまま残す)。
 * 読み込んだソースは src_buf に置く。上限は約464KB。
 * v41: 行継続 (バックスラッシュ+改行) を入力層で消す。
 * 設計は doc/splice.md
 * ================================================================ */

static char src_buf[475136];

/* 行継続 (v41): buf の中のバックスラッシュ+改行を
 * 消す (C の翻訳フェーズ2)。前へ詰めながら書き換える。
 * 対象は LF と CRLF の両方。文字列やコメントの中でも
 * 消えるのが C の決まり (フェーズ2は字句より前)。
 * ソース末尾のバックスラッシュは相手が無いので残る。
 * v51: #include で読んだファイルにも同じ処理が要る
 * ので、任意のバッファにかける形に分けた */
static void remove_splices_in(char *buf) {
    int r = 0;
    int w = 0;
    while (buf[r] != '\0') {
        if (buf[r] == '\\' && buf[r + 1] == '\n') {
            r += 2;
            continue;
        }
        if (buf[r] == '\\' && buf[r + 1] == '\r' &&
            buf[r + 2] == '\n') {
            r += 3;
            continue;
        }
        buf[w] = buf[r];
        w++;
        r++;
    }
    buf[w] = '\0';
}

static void remove_splices(void) {
    remove_splices_in(src_buf);
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
        if (used >= 475135) {
            close(fd);
            errmsg = "ファイルが大きすぎます (約464KBまで)";
            return -1;
        }
        long n = read(fd, src_buf + used, 475135 - used);
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
 * 境目には改行と印コメントを挟む (トークンが
 * くっつかないように + ファイル番号の境目、v58)。
 * こちらは全ファイルが必須: 1つでも開けなければ 0 を
 * 返し、名前を load_failed_name に置く。読みの失敗は -1 */
static int load_files(char **paths, int count) {
    int used = 0;
    load_failed_name = 0;
    for (int i = 0; i < count; i++) {
        if (used > 0) {
            if (used >= 475120) {
                errmsg = "ファイルが大きすぎます (約464KBまで)";
                return -1;
            }
            /* 境目には改行と、ファイル番号を進める
             * 印コメント (v58、02-parse の skip_spaces
             * が見る) を挟む */
            char *sep = "\n/*iwac:file*/\n";
            for (int j = 0; sep[j] != '\0'; j++)
                src_buf[used++] = sep[j];
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
        if (used >= 475135) {
            errmsg = "プログラムが大きすぎます (約464KBまで)";
            return -1;
        }
        src_buf[used] = text[used];
        used++;
    }
    src_buf[used] = '\0';
    remove_splices();
    return 1;
}

/* #include (v51) 用: ファイル全体を buf へ読む。容量は
 * cap (末尾の NUL の分を含む)。行継続も消してから、
 * テキストの長さを返す。戻り値: 0以上 = 長さ、
 * -1 = 開けない (ディレクトリなど読めないものも含む)、
 * -2 = 大きすぎる。前処理 (06) から呼ばれる */
int load_include(char *path, char *buf, int cap) {
    int fd = open(path, 0);
    if (fd < 0)
        return -1;
    int used = 0;
    for (;;) {
        if (used >= cap - 1) {
            close(fd);
            return -2;
        }
        long n = read(fd, buf + used, cap - 1 - used);
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        used += (int)n;
    }
    close(fd);
    buf[used] = '\0';
    remove_splices_in(buf);
    int len = 0;
    while (buf[len] != '\0')
        len++;
    return len;
}

