/* ================================================================
 * 02c-fpconv.c -- 浮動小数点リテラルの変換器 (v46)
 *
 * 10進リテラルを double のビットパターンへ、整数演算
 * だけで正確に (最近接偶数) 変換する部分。設計と作法は
 * src/02-parse.c の fp_bits の節のコメントと
 * doc/float.md にある。
 *
 * なぜ別断片か: v46 で 02-parse.c が大きくなり、その
 * 翻訳単位 (decls.c + 02-parse.c) が断片プロファイル
 * (kosaba/build-frag.sh) の変数・ノード上限に触れた
 * ため、v37 で宣言まわりを 02b-decl.c へ分けた前例に
 * 倣って変換器の関数だけをここへ出した。ビットの
 * 置き場 (fp_bits) と作業場 (fp_bn_a/b/c)、取り込みの
 * 控え (fp_pend / fp_folded) は 02-parse.c に残して
 * あり、名前の登録順は変えていない (02-parse.c 側に
 * 前方宣言が元の位置にある)。
 * ================================================================ */

static void bn_clear(long *w) {
    for (int i = 0; i < 128; i++)
        w[i] = 0;
}

static int bn_is_zero(long *w) {
    for (int i = 0; i < 128; i++)
        if (w[i] != 0)
            return 0;
    return 1;
}

/* ビット長 (最上位の 1 の位置 + 1)。0 なら 0 */
static int bn_bitlen(long *w) {
    for (int i = 127; i >= 0; i--) {
        if (w[i] != 0) {
            int b = 0;
            long v = w[i];
            while (v != 0) {
                b++;
                v = v >> 1;
            }
            return i * 32 + b;
        }
    }
    return 0;
}

/* w = w * m (m は小さい正の数)。128語に収まらなければ
 * 0 を返す (成功は 1) */
static int bn_mul_small(long *w, long m) {
    long carry = 0;
    for (int i = 0; i < 128; i++) {
        long cur = w[i] * m + carry;
        w[i] = cur & 0xFFFFFFFFL;
        carry = cur >> 32;
    }
    return carry == 0;
}

/* w = w + v (v は小さい正の数) */
static void bn_add_small(long *w, long v) {
    long carry = v;
    for (int i = 0; i < 128; i++) {
        if (carry == 0)
            return;
        long cur = w[i] + carry;
        w[i] = cur & 0xFFFFFFFFL;
        carry = cur >> 32;
    }
}

/* dst = src << sh (sh >= 0)。落ちるビットがあれば 0 を
 * 返す。dst と src が同じでもよい (上位の語から書く) */
static int bn_shl(long *dst, long *src, int sh) {
    int words = sh / 32;
    int bits = sh % 32;
    if (words >= 128)
        return bn_is_zero(src);
    for (int j = 128 - words; j < 128; j++)
        if (src[j] != 0)
            return 0;
    if (bits != 0 && (src[127 - words] >> (32 - bits)) != 0)
        return 0;
    for (int i = 127; i >= 0; i--) {
        long v = 0;
        int j = i - words;
        if (j >= 0) {
            v = (src[j] << bits) & 0xFFFFFFFFL;
            if (bits != 0 && j >= 1)
                v = v | (src[j - 1] >> (32 - bits));
        }
        dst[i] = v;
    }
    return 1;
}

static void bn_copy(long *dst, long *src) {
    for (int i = 0; i < 128; i++)
        dst[i] = src[i];
}

/* a と b の大小: 大きければ 1、小さければ -1、等しければ 0 */
static int bn_cmp(long *a, long *b) {
    for (int i = 127; i >= 0; i--) {
        if (a[i] != b[i])
            return a[i] > b[i] ? 1 : -1;
    }
    return 0;
}

/* a = a - b (a >= b の前提) */
static void bn_sub(long *a, long *b) {
    long borrow = 0;
    for (int i = 0; i < 128; i++) {
        long cur = a[i] - b[i] - borrow;
        if (cur < 0) {
            cur = cur + (1L << 32);  /* 2^32 */
            borrow = 1;
        } else
            borrow = 0;
        a[i] = cur;
    }
}

static int fp_feed(int d) {
    if (d == 0) {
        fp_pend++;
        return 1;
    }
    while (fp_pend > 0) {
        if (!bn_mul_small(fp_bn_a, 10))
            return 0;
        fp_pend--;
        fp_folded++;
    }
    if (!bn_mul_small(fp_bn_a, 10))
        return 0;
    bn_add_small(fp_bn_a, d);
    fp_folded++;
    return 1;
}

/* 10進リテラル (s の先頭 len 文字、形は parse_number_node
 * が切り出した「数字 [. 数字] [(e|E) [+|-] 数字]」) を
 * double のビットパターンに変換する。成功したら 1 を
 * 返して *out にビットを置く。失敗は 0 で errmsg を置く。
 * 符号は扱わない (単項マイナスは NODE_NEG が担う)。
 * 範囲外は IEEE の丸めの帰結として Inf / 0 になる */
static int fp_parse_bits(char *s, int len, long *out) {
    int i = 0;
    int started = 0;  /* 有意な数字が始まったか */
    long k = 0;       /* 値 = M × 10^k の k */
    bn_clear(fp_bn_a);
    fp_pend = 0;
    fp_folded = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        int d = s[i] - '0';
        if (d != 0 || started) {
            started = 1;
            if (!fp_feed(d)) {
                errmsg = "浮動小数点リテラルが長すぎます";
                return 0;
            }
        }
        i++;
    }
    if (i < len && s[i] == '.') {
        i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            int d = s[i] - '0';
            k--;
            if (d != 0 || started) {
                started = 1;
                if (!fp_feed(d)) {
                    errmsg = "浮動小数点リテラルが長すぎます";
                    return 0;
                }
            }
            i++;
        }
    }
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        int neg = 0;
        if (i < len && (s[i] == '+' || s[i] == '-')) {
            neg = s[i] == '-';
            i++;
        }
        long e = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            e = e * 10 + (s[i] - '0');
            if (e > 100000)
                e = 100000;  /* 飽和 (範囲外は下で Inf / 0) */
            i++;
        }
        if (neg)
            k = k - e;
        else
            k = k + e;
    }
    if (!started) {
        *out = 0;  /* 全部 0 のリテラルは +0.0 */
        return 1;
    }
    /* 10進の大きさ (最上位の桁の位置) で、明らかに
     * 範囲外のものを先に落とす:
     *   dec_exp > 309  → 1e310 以上で double の最大を
     *                    必ず超える → Inf
     *   dec_exp < -324 → 最小の subnormal (約 4.94e-324)
     *                    の半分未満 → 0
     * 境目 (-324 と 309) は下の正確な計算で決める */
    {
        long dec_exp = (long)(fp_folded + fp_pend - 1) + k;
        if (dec_exp > 309) {
            *out = 2047L << 52;  /* Inf のビット */
            return 1;
        }
        if (dec_exp < -324) {
            *out = 0;
            return 1;
        }
    }
    /* 保留していた末尾の 0 を M へ畳む (M の桁数は
     * dec_exp の見積もりに数え済み。ここで畳まないと
     * 値が 10^pend 倍ずれる) */
    while (fp_pend > 0) {
        if (!bn_mul_small(fp_bn_a, 10)) {
            errmsg = "浮動小数点リテラルが長すぎます";
            return 0;
        }
        fp_pend--;
        fp_folded++;
    }
    /* A / B の形に直す (B は fp_bn_b、割る数は fp_bn_c
     * に置く。分子は fp_bn_a のまま使う) */
    bn_clear(fp_bn_b);
    fp_bn_b[0] = 1;
    if (k >= 0) {
        for (long j = 0; j < k; j++)
            if (!bn_mul_small(fp_bn_a, 10)) {
                errmsg = "浮動小数点リテラルが長すぎます";
                return 0;
            }
    } else {
        for (long j = 0; j < -k; j++)
            if (!bn_mul_small(fp_bn_b, 10)) {
                errmsg = "浮動小数点リテラルが長すぎます";
                return 0;
            }
    }
    /* 2進指数 e: 2^e <= A/B < 2^(e+1)。ビット長の差から
     * 見積もり、比べを1回して 1 以内で直す */
    int e = bn_bitlen(fp_bn_a) - bn_bitlen(fp_bn_b);
    if (e >= 0) {
        if (!bn_shl(fp_bn_c, fp_bn_b, e)) {
            errmsg = "浮動小数点リテラルが長すぎます";
            return 0;
        }
        if (bn_cmp(fp_bn_a, fp_bn_c) < 0)
            e--;
    } else {
        if (!bn_shl(fp_bn_c, fp_bn_a, -e)) {
            errmsg = "浮動小数点リテラルが長すぎます";
            return 0;
        }
        if (bn_cmp(fp_bn_c, fp_bn_b) < 0)
            e--;
    }
    /* 取り出しの基準 (通常は e、subnormal になる範囲では
     * -1022 で頭打ち) と、割る数 D = B × 2^et を作る */
    int et = e < -1022 ? -1022 : e;
    if (et >= 0) {
        if (!bn_shl(fp_bn_c, fp_bn_b, et)) {
            errmsg = "浮動小数点リテラルが長すぎます";
            return 0;
        }
    } else {
        if (!bn_shl(fp_bn_a, fp_bn_a, -et)) {
            errmsg = "浮動小数点リテラルが長すぎます";
            return 0;
        }
        bn_copy(fp_bn_c, fp_bn_b);
    }
    /* 長除法: 値 / 2^et は [1, 2) (subnormal では
     * [0.5, 1)) にある。先頭のビットを取り、残りを
     * 2倍しながら 52 ビット取り出す。最後に guard
     * ビットと sticky (残りが 0 でないか) で最近接
     * 偶数に丸める */
    long mant = 0;
    if (bn_cmp(fp_bn_a, fp_bn_c) >= 0) {
        mant = 1;
        bn_sub(fp_bn_a, fp_bn_c);
    }
    for (int j = 0; j < 52; j++) {
        if (!bn_shl(fp_bn_a, fp_bn_a, 1)) {
            errmsg = "浮動小数点リテラルが長すぎます";
            return 0;
        }
        mant = mant << 1;
        if (bn_cmp(fp_bn_a, fp_bn_c) >= 0) {
            mant = mant + 1;
            bn_sub(fp_bn_a, fp_bn_c);
        }
    }
    if (!bn_shl(fp_bn_a, fp_bn_a, 1)) {
        errmsg = "浮動小数点リテラルが長すぎます";
        return 0;
    }
    int guard = 0;
    if (bn_cmp(fp_bn_a, fp_bn_c) >= 0) {
        guard = 1;
        bn_sub(fp_bn_a, fp_bn_c);
    }
    if (guard && (!bn_is_zero(fp_bn_a) || (mant & 1) != 0)) {
        mant++;
        if (mant == (1L << 53)) {
            /* 丸めで桁が上がった: 仮数を戻して指数を上げる */
            mant = mant >> 1;
            et++;
        }
    }
    if (et > 1023) {
        *out = 2047L << 52;  /* Inf */
        return 1;
    }
    if (mant >= (1L << 52)) {
        /* 通常数 (subnormal から繰り上がった場合も含む) */
        *out = ((long)(et + 1023) << 52) | (mant - (1L << 52));
        return 1;
    }
    /* subnormal: 指数の欄は 0 で、仮数がそのまま入る */
    *out = mant;
    return 1;
}

/* 整数 (long) の値を double のビットパターンにする
 * (ファイル範囲の double 変数に整数の初期化子を書いた
 * ときの変換。実行時の fcvt.d.l と同じ結果になる) */
static long fp_bits_from_long(long v) {
    if (v == 0)
        return 0;
    unsigned long u;
    long sign = 0;
    if (v < 0) {
        sign = 1;
        /* -(v + 1) + 1 なら、最小の long でも溢れない */
        u = (unsigned long)(-(v + 1)) + 1;
    } else
        u = (unsigned long)v;
    if ((u & (1UL << 63)) != 0) {
        /* 大きさが 2^63 (v が最小の long) のときだけ。
         * ちょうど表せるので仮数は 0 */
        return (long)((unsigned long)sign << 63) | (1086L << 52);
    }
    /* 最上位の 1 の位置 e をビット検査で探す (u は
     * 2^63 未満なので、右シフトは符号つきでも論理でも
     * 同じ結果になる範囲で使う) */
    int e = 62;
    while (e > 0 && (u & (1UL << e)) == 0)
        e--;
    unsigned long s;
    if (e <= 52) {
        s = u << (52 - e);  /* ちょうど収まる (丸めなし) */
    } else {
        int sh = e - 52;
        s = u >> sh;
        unsigned long rem = u & ((1UL << sh) - 1);
        unsigned long half = 1UL << (sh - 1);
        if (rem > half || (rem == half && (s & 1) != 0)) {
            s = s + 1;
            if (s == (1UL << 53)) {
                s = s >> 1;
                e++;
            }
        }
    }
    return (long)((unsigned long)sign << 63) |
           ((long)(e + 1023) << 52) |
           (long)(s - (1UL << 52));
}
