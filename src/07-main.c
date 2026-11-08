/* ================================================================
 * main: 層を順に呼ぶだけ
 *   parse (失敗したら何も出さない) → gen (同) → write
 * ================================================================ */

/* 文字列が同じかどうか (道具。#define の展開でも使う) */
static int str_eq(char *a, char *b) {
    int i = 0;
    while (a[i] != '\0' && a[i] == b[i])
        i++;
    return a[i] == b[i];
}

int main(int argc, char **argv) {
    /* 出力ターゲットの選択 (v14): 先頭に --kosaba があれば
     * KosabaOS のユーザープログラム用 (ELF) に切り替える */
    int argi = 1;
    target_kosaba = 0;
    obj_mode = 0;
    asari_mode = 0;
    elf_mode = 0;
    if (argc >= 2 && str_eq(argv[1], "--kosaba")) {
        target_kosaba = 1;
        argi = 2;
    }
    /* オブジェクト出力は ELF (v39) と Asari 形式
     * (v38) の2つ。旧 --object (.iwo) は v42 で引退
     * した (doc/iwo-retirement.md)。
     * Asari 形式 (doc/asari.md): --asari とすると、
     * 符号化する前の命令列をテキストで出す。符号化は
     * 独立プログラム Asari の仕事になる。
     * 生成の作法はオブジェクト出力と同じ (obj_mode も立てる) */
    if (argc >= argi + 1 && str_eq(argv[argi], "--asari")) {
        obj_mode = 1;
        asari_mode = 1;
        argi++;
    }
    /* ELF オブジェクト (v39、doc/elf-object.md):
     * --elf とすると、ELF の ET_REL で出す。生成の
     * 作法はオブジェクト出力と同じ */
    if (argc >= argi + 1 && str_eq(argv[argi], "--elf")) {
        obj_mode = 1;
        elf_mode = 1;
        argi++;
    }
    /* --out <名前>: KosabaOS 上での出力ファイル名
     * (v34。rt が hello.elf の代わりに使う) */
    if (argc >= argi + 2 && str_eq(argv[argi], "--out")) {
        kosaba_out_name = argv[argi + 1];
        argi += 2;
    }
    if (obj_mode && !target_kosaba) {
        print_str(2, "iwac: --asari / --elf は --kosaba と一緒に使います\n");
        return 1;
    }
    if (argc < argi + 1) {
        print_str(2, "usage: iwac <ファイル.c>   (例: iwac hello.c > hello.bin)\n       iwac <ファイル1.c> <ファイル2.c> ...   (複数ファイルはつないで読む)\n       iwac '<プログラム>'   (従来の形: 引数そのものをコンパイル)\n       iwac --kosaba <ファイル.c>   (KosabaOS 用の ELF を出す)\n       iwac --kosaba --asari <ファイル.c>   (Asari 形式 (命令列のテキスト) を出す\n       iwac --kosaba --elf <ファイル.c>   (ELF オブジェクトを出す (複数ファイルは連結して1つに)\n       iwac --kosaba --out <名前> <ファイル.c>   (OS 上の出力名を指定))\n");
        return 1;
    }
    if (target_kosaba)
        LOAD_BASE = 0x40000000u;  /* OS のユーザー領域の先頭 */

    /* 入力の決定: 引数が2つ以上なら全部ファイルとして
     * つないで読む (v17)。1つだけなら従来どおり:
     * .c ファイルは必ずファイルとして読む。それ以外は、
     * ファイルが開ければファイル、無ければ引数そのもの
     * がプログラム */
    char *src = argv[argi];
    if (argc - argi >= 2) {
        int loaded = load_files(&argv[argi], argc - argi);
        if (loaded <= 0) {
            print_str(2, "iwac: ");
            if (loaded < 0)
                print_str(2, errmsg);
            else {
                print_str(2, "ファイルが開けません: ");
                print_str(2, load_failed_name);
            }
            print_str(2, "\n");
            return 1;
        }
        src = src_buf;
    } else {
        int must_file = ends_with_c(argv[argi]);
        int loaded = load_file(argv[argi]);
        if (loaded < 0 || (must_file && loaded == 0)) {
            print_str(2, "iwac: ");
            if (loaded < 0)
                print_str(2, errmsg);
            else {
                print_str(2, "ファイルが開けません: ");
                print_str(2, argv[argi]);
            }
            print_str(2, "\n");
            return 1;
        }
        if (loaded == 1) {
            src = src_buf;
        } else {
            /* 引数そのものがプログラム: src_buf へ移して
             * から読む (行継続の処理を揃える、v41) */
            if (load_text(argv[argi]) < 0) {
                print_str(2, "iwac: ");
                print_str(2, errmsg);
                print_str(2, "\n");
                return 1;
            }
            src = src_buf;
        }
    }

    /* 前処理: #define を展開してから parse に渡す (v13) */
    if (!expand_defines(src)) {
        print_str(2, "iwac: ");
        print_str(2, errmsg);
        print_str(2, "\n");
        return 1;
    }
    src = exp_buf;

    if (!parse_program(src)) {
        print_str(2, "iwac: ");
        print_str(2, errmsg);
        print_str(2, "\n");
        return 1;
    }

    if (!gen_program()) {
        print_str(2, "iwac: ");
        print_str(2, errmsg);
        print_str(2, "\n");
        return 1;
    }

    write_binary();
    return 0;
}
