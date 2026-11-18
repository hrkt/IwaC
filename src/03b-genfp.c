/* ================================================================
 * 03b-genfp.c -- 浮動小数点のコード生成 (v46)
 *
 * double の値を作る・変換する・演算する命令列を出す
 * 部分: 64ビット即値のロード、整数との変換 (fcvt)、
 * 真偽化、受け側の型に合わせた生成 (gen_value_as)、
 * 二項演算 (gen_fp_binop)。設計は doc/float.md。
 *
 * なぜ別断片か: 02c-fpconv.c と同じ容量対策。v46 で
 * 03-gen.c が大きくなり、その翻訳単位が断片プロファイル
 * の入力・命令・再配置の上限に触れたため、浮動小数点の
 * 生成だけをここへ出した。呼び出し側 (03-gen.c) には
 * 前方宣言が元の位置に残してあり、名前の登録順は
 * 変えていない。
 * ================================================================ */

/* 64ビットのビットパターンをレジスタに作る (v46)。
 * 浮動小数点リテラルのビット (double の全域) のため。
 * 作り方: 上位32ビットを gen_load_u32 で 0 埋めで作り、
 * 下位32ビットを 11 + 11 + 10 ビットずつ「ずらして
 * addi」で継ぎ足す。下位の継ぎ足しは即値が正の範囲
 * (2047 まで) に収まるよう桁を分けるので、別の
 * レジスタは要らない。hi / lo の切り分けはマスクと
 * シフトだけで行い、キャストには頼らない (作った
 * コンパイラで結果が食い違わないため、v27 の約束) */
static void emit_load_i64(int reg, long v) {
    long hi = (v >> 32) & 0xFFFFFFFFL;
    long lo = v & 0xFFFFFFFFL;
    if (hi == 0) {
        gen_load_u32(reg, (int)lo);
        return;
    }
    gen_load_u32(reg, (int)hi);
    if (lo == 0) {
        asm_push(OP_SLLI, reg, reg, 0, 32);
        return;
    }
    asm_push(OP_SLLI, reg, reg, 0, 11);
    asm_push(OP_ADDI, reg, reg, 0, (int)((lo >> 21) & 0x7FF));
    asm_push(OP_SLLI, reg, reg, 0, 11);
    asm_push(OP_ADDI, reg, reg, 0, (int)((lo >> 10) & 0x7FF));
    asm_push(OP_SLLI, reg, reg, 0, 10);
    asm_push(OP_ADDI, reg, reg, 0, (int)(lo & 0x3FF));
}


/* 整数レジスタ greg の値を double に変換して浮動小数点
 * レジスタ freg へ入れる (v46)。符号なしの整数からは
 * fcvt.d.lu、それ以外は fcvt.d.l。丸めは fcsr の
 * デフォルト (最近接偶数) */
static void emit_cvt_d(int freg, int greg, int src_et) {
    asm_push((src_et & 16) ? OP_FCVT_D_LU : OP_FCVT_D_L,
             freg, greg, 0, 0);
}

/* ft0 の double を整数に変換して t1 へ入れる (v46)。
 * 切り捨て (fcvt の丸め指定で選ぶ)。範囲外と NaN は
 * RISC-V の飽和 (最大/最小に張り付き、NaN は最大値)
 * になるのが iwac の仕様 (doc/float.md)。変換先の幅への
 * 折り返しは、呼び出し側が emit_canon で行う */
static void emit_cvt_l(int dst_et) {
    asm_push((dst_et & 16) ? OP_FCVT_LU_D : OP_FCVT_L_D,
             REG_T1, REG_FT0, 0, 0);
}

/* ft0 の double を「0 でないか」の整数 0/1 (t1) に
 * 直す (v46)。条件分岐や ! / && / || が整数の世界で
 * 消費するための変換: +0.0 と比べて、等しくなければ
 * 1。NaN は「等しくない」ので 1 (C の作法どおり) */
static void emit_fp_bool(void) {
    asm_push(OP_FMV_D_X, REG_FT1, REG_ZERO, 0, 0);
    asm_push(OP_FEQ_D, REG_T1, REG_FT0, REG_FT1, 0);
    asm_push(OP_XORI, REG_T1, REG_T1, 0, 1);
}

/* 式を受け側の型タグに合わせて生成する (v46):
 * 受け側が double なら値は ft0 へ、整数なら t1 へ。
 * 型が跨ぐときは fcvt で変換する。整数同士なら
 * gen_expr とまったく同じ (出力は変わらない)。
 * 代入・引数・return・要素への書き込みの境界で使う */
static int gen_value_as(int node, int dst) {
    int src = node_etype[node];
    if (dst & 64) {
        if (src & 64)
            return gen_expr(node);  /* double のまま ft0 へ */
        if (!gen_expr(node))
            return 0;
        emit_cvt_d(REG_FT0, REG_T1, src);
        return 1;
    }
    if (src & 64) {
        if (!gen_expr(node))
            return 0;
        emit_cvt_l(dst);
        emit_canon(dst);
        return 1;
    }
    return gen_expr(node);
}

/* 式の値がポインタや住所か (v46)。浮動小数点の演算の
 * 相手に紛れ込んだら止めるための点検。直接の形だけを
 * 見て、深いポインタの連鎖は従来の緩さに任せる */
static int fp_operand_is_ptr(int node) {
    int k = node_kind[node];
    if (k == NODE_STR || k == NODE_ARRAY || k == NODE_ADDR ||
        k == NODE_FADDR || k == NODE_ROWADDR)
        return 1;
    if (k == NODE_VAR)
        return var_plevel[node_val[node]] > 0 ||
               var_fsig[node_val[node]] != 0;
    if (k == NODE_MEMBER || k == NODE_MELEM)
        return smem_plevel[node_val[node]] > 0 ||
               smem_fsig[node_val[node]] != 0;
    if (k == NODE_INDEX || k == NODE_INDEX2)
        return arr_plevel[node_val[node]] > 0 ||
               arr_fsig[node_val[node]] != 0;
    return 0;
}

/* 浮動小数点の二項演算の生成 (v46): 骨格は整数の
 * 二項演算と同じで、値の置き場が ft0 / ft1 になる。
 * 左を ft0 へ (整数なら変換)、右が葉なら ft1 へ、
 * 葉でなければ左を fsd で退避してから右を計算する。
 * 四則は fadd.d 等、比較は feq.d / flt.d / fle.d で
 * 結果を整数 (t1 の 0/1) に戻す。> と >= は両辺を
 * 入れ替えた flt / fle で表す */
static int gen_fp_binop(int node) {
    int k = node_kind[node];
    if (k == NODE_MOD || k == NODE_AND || k == NODE_OR ||
        k == NODE_XOR || k == NODE_SHL || k == NODE_SHR) {
        errmsg = "浮動小数点に % やビット演算・シフトは使えません";
        return 0;
    }
    int lhs = node_lhs[node];
    int rhs = node_rhs[node];
    if (fp_operand_is_ptr(lhs) || fp_operand_is_ptr(rhs)) {
        errmsg = "浮動小数点の演算にポインタや住所は使えません";
        return 0;
    }
    if (!gen_value_as(lhs, 8 + 64))
        return 0;
    if (is_leaf(rhs)) {
        /* 右が葉: ビットか整数値を t2 に作って ft1 へ */
        gen_load_leaf(REG_T2, rhs);
        if (node_etype[rhs] & 64)
            asm_push(OP_FMV_D_X, REG_FT1, REG_T2, 0, 0);
        else
            emit_cvt_d(REG_FT1, REG_T2, node_etype[rhs]);
    } else {
        asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
        asm_push(OP_FSD, 0, REG_SP, REG_FT0, 0);
        if (!gen_value_as(rhs, 8 + 64))
            return 0;
        asm_push(OP_FMV_D, REG_FT1, REG_FT0, REG_FT0, 0);
        asm_push(OP_FLD, REG_FT0, REG_SP, 0, 0);
        asm_push(OP_ADDI, REG_SP, REG_SP, 0, 16);
    }
    if (k == NODE_ADD)
        asm_push(OP_FADD_D, REG_FT0, REG_FT0, REG_FT1, 0);
    else if (k == NODE_SUB)
        asm_push(OP_FSUB_D, REG_FT0, REG_FT0, REG_FT1, 0);
    else if (k == NODE_MUL)
        asm_push(OP_FMUL_D, REG_FT0, REG_FT0, REG_FT1, 0);
    else if (k == NODE_DIV)
        asm_push(OP_FDIV_D, REG_FT0, REG_FT0, REG_FT1, 0);
    else if (k == NODE_EQ)
        asm_push(OP_FEQ_D, REG_T1, REG_FT0, REG_FT1, 0);
    else if (k == NODE_NE) {
        asm_push(OP_FEQ_D, REG_T1, REG_FT0, REG_FT1, 0);
        asm_push(OP_XORI, REG_T1, REG_T1, 0, 1);
    }
    else if (k == NODE_LT)
        asm_push(OP_FLT_D, REG_T1, REG_FT0, REG_FT1, 0);
    else if (k == NODE_GT)
        asm_push(OP_FLT_D, REG_T1, REG_FT1, REG_FT0, 0);
    else if (k == NODE_LE)
        asm_push(OP_FLE_D, REG_T1, REG_FT0, REG_FT1, 0);
    else if (k == NODE_GE)
        asm_push(OP_FLE_D, REG_T1, REG_FT1, REG_FT0, 0);
    else {
        errmsg = "内部エラー: 浮動小数点にできない演算です";
        return 0;
    }
    return 1;
}
