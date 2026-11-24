/* ================================================================
 * 03-genstmt.c -- 文と関数全体のコード生成 (v49 で分割)
 *
 * 文の生成 (gen_stmt / gen_switch)、関数の生成
 * (gen_func)、プログラム全体の生成 (gen_all /
 * gen_program)、飛び先の後書きの道具 (patch)、
 * スタック要否の走査を置く。式の生成 (gen_expr) と
 * その道具は 03-gen.c 側にある。
 *
 * なぜ別断片か: 03b-genfp.c と同じ容量対策。v49 の
 * ポインタ演算の修正で 03-gen.c の翻訳単位が育ち
 * (オブジェクト 295,112 バイト)、nori 断片プロファイル
 * の入力上限を超えたため、後半をここへ出した。
 * ファイル名は連結順で 03-gen.c の直後・03b-genfp.c の
 * 前に来るように付け、名前の登録順は変えていない。
 * 03-gen.c 側から呼ぶことはなく、こちらから呼ぶ相手は
 * すべて decls.c に宣言がある (共有の仕組みどおり)。
 * ================================================================ */

/* ループの break / continue の飛び先を後から書き直すための
 * 道具 (v20)。飛び先は「そのループの生成が終わるまで」
 * 分からないので、jal を仮置きし、その命令番号をリストに
 * 繋いでおく。リストは patch_next[] で命令同士を繋ぐ
 * (命令番号がそのままリストの要素になる)。
 * ループの入れ子には、深さごとの先頭で対応する */
static int loop_depth;
static int loop_break_head[32];
static int loop_cont_head[32];
static int sw_depth;            /* switch の入れ子の深さ (v43) */
static int sw_break_head[32];   /* switch の break の飛び先リスト */
static int ctrl_depth;          /* 制御構造 (ループと switch) の
                                 * 入れ子の深さ。break が最も内側の
                                 * どちらを抜けるかを決める (v43) */
static char ctrl_is_switch[65]; /* 深さごとの種別: 1 なら switch */
static int patch_next[24576];

/* 関数の return の飛び先 (出口は関数の生成の最後まで
 * 分からない) も同じ道具で積む */
static int return_patch_head;
static int gen_in_func;  /* 関数の生成中なら 1 */
static int gen_cur_func;  /* 生成中の関数の番号 (v45)。
                           * return の照合と sret のために使う */

/* 仮の jal を置き、番号をリストの先頭に繋ぐ */
static void emit_patch_jump(int *head) {
    int i = nasm;
    asm_push(OP_JAL, 0, 0, 0, 0);
    if (nasm == i)
        return;  /* 表が一杯で push が止まった (v44)。
                  * 生成の最後で「命令が多すぎます」に
                  * なるので、ここでは繋がない */
    patch_next[i] = *head;
    *head = i;
}

/* リストに積んだ jal 全部の飛び先を target に書き直す */
static void apply_patches(int head, int target) {
    for (int i = head; i != -1; i = patch_next[i])
        asm_imm[i] = target;
}

static int gen_switch(int node);  /* switch の生成 (本体は
                                   * gen_stmt の後ろ、v43) */

/* 条件の式を生成し、結果を t1 の 0 / 非0 に揃える
 * (v46)。double の条件は ft0 に値が残るので、整数の
 * 0/1 に直してから分岐の道具 (emit_beq_t1z) に渡す */
static int gen_cond_expr(int node) {
    if (!gen_expr(node))
        return 0;
    if (node_etype[node] & 64)
        emit_fp_bool();
    return 1;
}

/* 条件分岐を置き、飛び先のパッチリストに繋ぐ */
static void emit_patch_branch(int op, int rs1, int rs2,
                              int *head) {
    int i = emit_branch(op, rs1, rs2);
    patch_next[i] = *head;
    *head = i;
}

/* 条件を「分岐」として生成する (v53、設計は
 * doc/branch-fusion.md)。sense が 0 なら「偽なら
 * 飛ぶ」、1 なら「真なら飛ぶ」分岐を、head の
 * リストに繋ぐ (飛び先は呼び出し側が apply_patches
 * で確定させる)。比較は 0/1 を作らず大小分岐に
 * 直し、&& || ! は短絡のまま再帰する。それ以外
 * (double が絡むもの、呼び出し、素の値) は
 * gen_cond_expr で 0/1 を作る従来形に落とす */
static int gen_cond_branch(int node, int *head, int sense) {
    int k = node_kind[node];
    if (k == NODE_LAND) {
        if (!sense) {
            /* 偽なら飛ぶ: どちらかが偽なら飛ぶ */
            if (!gen_cond_branch(node_lhs[node], head, 0))
                return 0;
            return gen_cond_branch(node_rhs[node], head, 0);
        }
        /* 真なら飛ぶ: 左が偽なら右の判定へ落ちる */
        int skip = -1;
        if (!gen_cond_branch(node_lhs[node], &skip, 0))
            return 0;
        if (!gen_cond_branch(node_rhs[node], head, 1))
            return 0;
        apply_patches(skip, nasm);
        return 1;
    }
    if (k == NODE_LOR) {
        if (sense) {
            /* 真なら飛ぶ: どちらかが真なら飛ぶ */
            if (!gen_cond_branch(node_lhs[node], head, 1))
                return 0;
            return gen_cond_branch(node_rhs[node], head, 1);
        }
        /* 偽なら飛ぶ: 左が真なら右の判定へ落ちる */
        int skip = -1;
        if (!gen_cond_branch(node_lhs[node], &skip, 1))
            return 0;
        if (!gen_cond_branch(node_rhs[node], head, 0))
            return 0;
        apply_patches(skip, nasm);
        return 1;
    }
    if (k == NODE_NOT)
        /* ! は向きを反転して再帰 */
        return gen_cond_branch(node_lhs[node], head, !sense);
    if ((k == NODE_EQ || k == NODE_NE || k == NODE_LT ||
         k == NODE_GT || k == NODE_LE || k == NODE_GE) &&
        !(node_etype[node_lhs[node]] & 64) &&
        !(node_etype[node_rhs[node]] & 64)) {
        /* 比較の融合: 両辺を t1 / t2 に揃え、0/1 を
         * 作らずに分岐する。まず「偽なら飛ぶ」命令を
         * 選び、真向きは反対の命令にする。符号なしは
         * どちらかが uint64 のとき (gen_expr の比較と
         * 同じ判定) */
        if (!gen_binop_operands(node))
            return 0;
        int ucmp = (node_etype[node_lhs[node]] == (8 + 16 + 32)) ||
                   (node_etype[node_rhs[node]] == (8 + 16 + 32));
        int op;
        int rs1 = REG_T1;
        int rs2 = REG_T2;
        if (k == NODE_LT)
            op = ucmp ? OP_BGEU : OP_BGE;
        else if (k == NODE_GT) {
            op = ucmp ? OP_BGEU : OP_BGE;
            rs1 = REG_T2;
            rs2 = REG_T1;
        } else if (k == NODE_LE) {
            op = ucmp ? OP_BLTU : OP_BLT;
            rs1 = REG_T2;
            rs2 = REG_T1;
        } else if (k == NODE_GE)
            op = ucmp ? OP_BLTU : OP_BLT;
        else if (k == NODE_EQ)
            op = OP_BNE;
        else
            op = OP_BEQ;
        if (sense)
            op = branch_inv(op);
        emit_patch_branch(op, rs1, rs2, head);
        return 1;
    }
    /* 落とし先: 従来形 (0/1 を作って beq / bne) */
    if (!gen_cond_expr(node))
        return 0;
    emit_patch_branch(sense ? OP_BNE : OP_BEQ,
                      REG_T1, REG_ZERO, head);
    return 1;
}

static int gen_stmt(int node) {
    int kind = node_kind[node];
    if (kind == NODE_PREINC || kind == NODE_POSTINC ||
        kind == NODE_PREDEC || kind == NODE_POSTDEC ||
        kind == NODE_EPRE || kind == NODE_EPOST) {
        /* 増減文: 式として生成し、結果 (t1) は捨てる
         * (v19。要素への増減は v25 で追加) */
        return gen_expr(node);
    }
    if (kind == NODE_DECL) {
        /* 配列の宣言: 置き場所はコンパイラが静的に確保する
         * だけなので、実行時のコードは何も出ない */
        return 1;
    }
    if (kind == NODE_INDEX_STORE || kind == NODE_ELEM_STORE) {
        /* 要素への書き込み: 要素の住所を t2 に作り
         * (gen_elem_addr)、そこへ値を書く。要素の大きさ
         * が 1 なら sb、8 なら sd (v23)。
         * 書く値が葉なら「住所を t2 → 値を t1 → 書く」。
         * 値も式なら、住所をスタックへ退避してから値を
         * 計算する (値の計算が t2 を使うため)。
         * INDEX_STORE はノード自体が住所の材料で、値は
         * rhs。ELEM_STORE は lhs が要素を表す式ノードで、
         * 値は同じく rhs */
        int addr_node = (kind == NODE_INDEX_STORE) ? node
                                                   : node_lhs[node];
        int val_node = node_rhs[node];
        /* 構造体の要素への丸ごと代入 (v45): 要素が
         * 構造体なら、右辺の構造体の値を複写する */
        {
            int dst_tag = (kind == NODE_INDEX_STORE)
                ? (arr_plevel[node_val[node]] == 0
                       ? arr_struct[node_val[node]] : 0)
                : value_struct(node_lhs[node]);
            if (dst_tag > 0) {
                if (!gen_elem_addr(addr_node))
                    return 0;
                return gen_struct_copy_rhs(val_node, dst_tag);
            }
            if (value_struct(val_node) > 0) {
                errmsg = "構造体の値は構造体にだけ代入できます";
                return 0;
            }
        }
        /* 要素が関数ポインタなら署名を照合する (v44) */
        {
            int want = (kind == NODE_INDEX_STORE)
                ? arr_fsig[node_val[node]]
                : elem_fsig_of(node_lhs[node]);
            if (!check_fsig(want, val_node))
                return 0;
        }
        int dst_et = elem_etype(addr_node);
        if (!gen_elem_addr(addr_node))
            return 0;
        if (!is_leaf(val_node)) {
            asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
            asm_push(OP_SD, 0, REG_SP, REG_T2, 0);
            /* 値は要素の型に合わせて生成する (v46:
             * double の要素なら ft0 に残る) */
            if (!gen_value_as(val_node, dst_et))
                return 0;
            asm_push(OP_LD, REG_T2, REG_SP, 0, 0);
            asm_push(OP_ADDI, REG_SP, REG_SP, 0, 16);
        } else if (dst_et & 64) {
            /* double の要素への書き込み (v46): 葉の
             * 値をビットか整数として t1 に作り、
             * ft0 へ移すか変換する */
            gen_load_leaf(REG_T1, val_node);
            if (node_etype[val_node] & 64)
                asm_push(OP_FMV_D_X, REG_FT0, REG_T1, 0, 0);
            else
                emit_cvt_d(REG_FT0, REG_T1, node_etype[val_node]);
        } else if (node_etype[val_node] & 64) {
            /* 整数の要素へ double の値を書く (v46) */
            gen_load_leaf(REG_T1, val_node);
            asm_push(OP_FMV_D_X, REG_FT0, REG_T1, 0, 0);
            emit_cvt_l(dst_et);
            emit_canon(dst_et);
        } else {
            gen_load_leaf(REG_T1, val_node);
        }
        if (dst_et & 64)
            asm_push(OP_FSD, 0, REG_T2, REG_FT0, 0);
        else
            asm_push(mem_store_op(dst_et),
                     0, REG_T2, REG_T1, 0);
        return 1;
    }
    if (kind == NODE_VDECL) {
        /* 変数の宣言だけ: 置き場所は parse で予約済みで、
         * 実行時のコードは何も出ない (v21) */
        return 1;
    }
    if (kind == NODE_ASSIGN) {
        /* 構造体の丸ごと代入 (v45): 代入先が構造体の
         * 変数なら、右辺の構造体の値を複写する */
        if (var_struct[node_val[node]] > 0 &&
            var_plevel[node_val[node]] == 0) {
            gen_var_addr(REG_T2, node_val[node]);
            return gen_struct_copy_rhs(node_lhs[node],
                                       var_struct[node_val[node]]);
        }
        if (value_struct(node_lhs[node]) > 0) {
            errmsg = "構造体の値は構造体にだけ代入できます";
            return 0;
        }
        /* 代入先が関数ポインタなら署名を照合する (v44) */
        if (!check_fsig(var_fsig[node_val[node]], node_lhs[node]))
            return 0;
        /* 値は代入先の型に合わせて生成する (v46:
         * double の変数なら ft0 に残る) */
        int vet = var_value_etype(node_val[node]);
        if (!gen_value_as(node_lhs[node], vet))
            return 0;
        if (vet & 64) {
            gen_var_addr(REG_T2, node_val[node]);
            asm_push(OP_FSD, 0, REG_T2, REG_FT0, 0);
            return 1;
        }
        /* 結果 (t1) を変数の置き場所へ書く (代入、v21)。
         * 値がどの型の正規形かも渡す (v56: レジスタ
         * 割り付けの書き込みで正規化を省く判断に使う) */
        gen_store_var(node_val[node], REG_T1,
                      node_etype[node_lhs[node]]);
        /* ループ最適化 (v57): 帰納変数への代入なら、
         * 歩くポインタを進める */
        loop_xform_on_mod(node);
        return 1;
    }
    if (kind == NODE_PUTC) {
        if (!gen_expr(node_lhs[node]))
            return 0;
        if (target_kosaba) {
            /* KosabaOS では U モードから UART のレジスタを
             * 触れないので、putc システムコール (番号 1) で
             * 出す。a0 = 文字、a7 = 1、ecall */
            asm_push(OP_ADDI, REG_A0, REG_T1, 0, 0);
            asm_push(OP_ADDI, REG_A7, REG_ZERO, 0, 1);
            asm_push(OP_ECALL, 0, 0, 0, 0);
            return 1;
        }
        /* sb t1, 0(t0) -- 式の値の下1バイトを UART へ */
        asm_push(OP_SB, 0, REG_T0, REG_T1, 0);
        return 1;
    }
    if (kind == NODE_STORE) {
        /* メモリ書き込み: 先に「書く値」を t1 で計算し、
         * 次に「書き込み先の住所」を t2 に作って書き込む。
         * この順番なのは、住所作りが t1 を壊さないため
         * (値の計算は t2 を下書きに使うことがある)。
         * 書く幅は指す先の大きさで決める (v27: 以前は
         * 常に sb で、int* への書き込みが1バイトに切れて
         * いた。セルフコンパイルの append_file で発覚)。
         * 絶対アドレス (デバイスの窓口) は従来どおり
         * 1バイト */
        /* ポインタ越しの構造体の丸ごと代入 (v45):
         * 指す先が構造体なら、右辺の構造体の値を複写する */
        if (node_kind[node_lhs[node]] == NODE_VAR &&
            var_plevel[node_val[node_lhs[node]]] == 1 &&
            var_struct[node_val[node_lhs[node]]] > 0) {
            int tag = var_struct[node_val[node_lhs[node]]];
            gen_load_var(REG_T2, node_val[node_lhs[node]]);
            return gen_struct_copy_rhs(node_rhs[node], tag);
        }
        if (value_struct(node_rhs[node]) > 0) {
            errmsg = "構造体の値は構造体にだけ代入できます";
            return 0;
        }
        int addr = node_lhs[node];
        /* 値は指す先の型に合わせて生成する (v46:
         * double の指す先なら ft0 に残る) */
        int dst_et = deref_etype(addr);
        if (!gen_value_as(node_rhs[node], dst_et))
            return 0;
        if (node_kind[addr] == NODE_NUM)
            gen_load_addr_sym(REG_T2, 0, node_val[addr]);
        else /* NODE_VAR: 変数の値そのものが住所 (ポインタ) */
            gen_load_var(REG_T2, node_val[addr]);
        if (dst_et & 64)
            asm_push(OP_FSD, 0, REG_T2, REG_FT0, 0);
        else
            asm_push(mem_store_op(dst_et),
                     0, REG_T2, REG_T1, 0);
        return 1;
    }
    if (kind == NODE_CALL || kind == NODE_ICALL) {
        int bsig = kind == NODE_CALL ? func_sig[node_val[node]]
                                     : node_val[node];
        if (call_b_mode(bsig) && sig_ret_struct[bsig] > 0) {
            /* 構造体を返す呼び出しを文として (v45):
             * 戻り値は一時置き場に受けて捨てる */
            int n = struct_slots(sig_ret_struct[bsig]);
            asm_push(OP_ADDI, REG_SP, REG_SP, 0, -n * 16);
            if (!gen_b_call(node, 1))
                return 0;
            asm_push(OP_ADDI, REG_SP, REG_SP, 0, n * 16);
            return 1;
        }
    }
    if (kind == NODE_CALL || kind == NODE_SYSCALL3 ||
        kind == NODE_ICALL) {
        /* 呼び出し文: 式として生成し、戻り値 (t1) は
         * 捨てる (v22 で呼び出しが式になったための形。
         * syscall3 の文も同じ、v27。間接呼び出しも
         * 同じ、v44) */
        return gen_expr(node);
    }
    if (kind == NODE_IF) {
        /* if の作り方:
         *   条件を分岐として生成 (偽なら L_else
         *   または L_end へ、v53)
         *   成り立つときの文...
         *   (else がある場合) jal zero, L_end
         * L_else:
         *   else の文...
         * L_end:
         * 飛び先の書き直しは while と同じやり方 */
        int c_head = -1;
        if (!gen_cond_branch(node_lhs[node], &c_head, 0))
            return 0;
        int then_body = node_rhs[node];
        for (int s = then_body; s != -1; s = node_next[s]) {
            if (!gen_stmt(s))
                return 0;
        }
        int else_body = node_val[node];
        if (else_body < 0) {
            /* else 無し: ここが L_end */
            apply_patches(c_head, nasm);
            return 1;
        }
        /* else あり: 本体を飛び越えるジャンプを置き、
         * 条件の飛び先 (else の頭) を確定させる */
        int jal_index = nasm;
        asm_push(OP_JAL, 0, 0, 0, 0);
        apply_patches(c_head, nasm);
        for (int s = else_body; s != -1; s = node_next[s]) {
            if (!gen_stmt(s))
                return 0;
        }
        asm_imm[jal_index] = nasm;
        return 1;
    }
    if (kind == NODE_WHILE) {
        /* ループ最適化 (v57): 歩くポインタの初期値を
         * 条件判定の前に作る */
        int xmark = loop_xform_begin(node);
        int l_cond = nasm;  /* 条件判定の命令番号 */
        /* 条件を分岐として生成 (偽なら L_end、v53)。
         * 飛び先は末尾で書き直す */
        int c_head = -1;
        if (!gen_cond_branch(node_lhs[node], &c_head, 0))
            return 0;
        if (loop_depth >= 32) {
            errmsg = "ループの入れ子が深すぎます";
            return 0;
        }
        loop_depth++;
        loop_break_head[loop_depth] = -1;
        loop_cont_head[loop_depth] = -1;
        ctrl_depth++;
        ctrl_is_switch[ctrl_depth] = 0;
        int body = node_rhs[node];
        for (int s = body; s != -1; s = node_next[s]) {
            if (!gen_stmt(s))
                return 0;
        }
        /* continue の飛び先は条件判定。jal の直前が現在の
         * 末尾なので、continue は「末尾へ飛ぶ = そのまま
         * jal で戻る」の形でも同じだが、番号で揃える */
        apply_patches(loop_cont_head[loop_depth], l_cond);
        /* jal zero, L_cond -- 条件の判定に戻る */
        asm_push(OP_JAL, 0, 0, 0, l_cond);
        /* ここ (末尾) が L_end。条件と break の飛び先を
         * 書き直す */
        apply_patches(c_head, nasm);
        apply_patches(loop_break_head[loop_depth], nasm);
        ctrl_depth--;
        loop_depth--;
        loop_xform_end(xmark);
        return 1;
    }
    if (kind == NODE_DOWHILE) {
        /* ループ最適化 (v57): 歩くポインタの初期値を
         * 本体の前に作る */
        int xmark = loop_xform_begin(node);
        /* do-while (v48): 本体を先に置き、条件は後ろ。
         * 「真なら戻る」は「偽なら落ちる + jal で戻る」
         * に分解する (条件の分岐の飛び先は前にしか
         * 向けないので、この形が素直) */
        int l_top = nasm;  /* 本体の先頭の命令番号 */
        if (loop_depth >= 32) {
            errmsg = "ループの入れ子が深すぎます";
            return 0;
        }
        loop_depth++;
        loop_break_head[loop_depth] = -1;
        loop_cont_head[loop_depth] = -1;
        ctrl_depth++;
        ctrl_is_switch[ctrl_depth] = 0;
        int body = node_rhs[node];
        for (int s = body; s != -1; s = node_next[s]) {
            if (!gen_stmt(s))
                return 0;
        }
        /* continue の飛び先は条件判定 (本体の直後) */
        apply_patches(loop_cont_head[loop_depth], nasm);
        /* 条件を分岐として生成 (偽なら L_end、v53) */
        int c_head = -1;
        if (!gen_cond_branch(node_lhs[node], &c_head, 0))
            return 0;
        /* jal zero, L_top -- 条件が真なら本体へ戻る */
        asm_push(OP_JAL, 0, 0, 0, l_top);
        /* ここ (末尾) が L_end。条件と break の飛び先 */
        apply_patches(c_head, nasm);
        apply_patches(loop_break_head[loop_depth], nasm);
        ctrl_depth--;
        loop_depth--;
        loop_xform_end(xmark);
        return 1;
    }
    if (kind == NODE_FOR) {
        /* for の作り方 (while への読み替え):
         *   初期化の文 (あれば)
         * L_cond: 条件を分岐として生成 (偽なら L_end)
         *         本体の文...
         * L_step: 更新の文 (あれば。本体の並びの最後に
         *         NODE_FORSTEP で包んで繋いである)
         *         jal zero, L_cond
         * L_end:
         * continue の飛び先は L_step (更新を飛ばすと
         * 終わらないループになるため) */
        if (node_val[node] >= 0) {
            /* 初期化は文の鎖のこともある (構造体の
             * 初期化子の展開、v45) */
            for (int s = node_val[node]; s != -1; s = node_next[s]) {
                if (!gen_stmt(s))
                    return 0;
            }
        }
        /* ループ最適化 (v57): 初期化が済んだ所で、
         * 歩くポインタの初期値を条件判定の前に作る */
        int xmark = loop_xform_begin(node);
        int l_cond = nasm;
        int c_head = -1;
        if (!gen_cond_branch(node_lhs[node], &c_head, 0))
            return 0;
        if (loop_depth >= 32) {
            errmsg = "ループの入れ子が深すぎます";
            return 0;
        }
        loop_depth++;
        loop_break_head[loop_depth] = -1;
        loop_cont_head[loop_depth] = -1;
        ctrl_depth++;
        ctrl_is_switch[ctrl_depth] = 0;
        int cont_done = 0;
        int body = node_rhs[node];
        for (int s = body; s != -1; s = node_next[s]) {
            if (node_kind[s] == NODE_FORSTEP) {
                apply_patches(loop_cont_head[loop_depth], nasm);
                cont_done = 1;
                if (!gen_stmt(node_val[s]))
                    return 0;
            } else {
                if (!gen_stmt(s))
                    return 0;
            }
        }
        if (!cont_done)
            /* 更新が無い for: continue は条件判定へ直行 */
            apply_patches(loop_cont_head[loop_depth], l_cond);
        asm_push(OP_JAL, 0, 0, 0, l_cond);
        apply_patches(c_head, nasm);
        apply_patches(loop_break_head[loop_depth], nasm);
        ctrl_depth--;
        loop_depth--;
        loop_xform_end(xmark);
        return 1;
    }
    if (kind == NODE_SWITCH) {
        return gen_switch(node);
    }
    if (kind == NODE_BREAK) {
        /* 脱出: 最も内側の制御構造 (ループか switch) を
         * 抜ける。飛び先はその構造の生成の最後に書き直す */
        if (ctrl_depth > 0 && ctrl_is_switch[ctrl_depth]) {
            emit_patch_jump(&sw_break_head[sw_depth]);
            return 1;
        }
        if (loop_depth == 0) {
            errmsg = "break はループか switch の中でだけ使えます";
            return 0;
        }
        emit_patch_jump(&loop_break_head[loop_depth]);
        return 1;
    }
    if (kind == NODE_CONTINUE) {
        if (loop_depth == 0) {
            errmsg = "continue はループの中でだけ使えます";
            return 0;
        }
        emit_patch_jump(&loop_cont_head[loop_depth]);
        return 1;
    }
    if (kind == NODE_RETURN) {
        /* return: 値があれば式を計算して a0 (戻り値の
         * 場所) へ置き、関数の出口 (後始末) へ跳ぶ。
         * 出口の番号は関数の生成の最後に書き直す (v20)。
         * 戻り値つきは v22 */
        if (!gen_in_func) {
            errmsg = "return は関数の中でだけ使えます";
            return 0;
        }
        if (inl_depth > 0) {
            /* インライン展開の中の return (v58):
             * 値を戻り値変数へ書き、展開の終端へ
             * 跳ぶ (gen_inline_call が後始末する) */
            if (node_lhs[node] >= 0) {
                int rv = inl_ret_var[inl_depth - 1];
                if (!gen_value_as(node_lhs[node],
                                  var_etype[rv]))
                    return 0;
                gen_store_var(rv, REG_T1,
                              node_etype[node_lhs[node]]);
            }
            emit_patch_jump(&inl_patch_head[inl_depth - 1]);
            return 1;
        }
        if (gen_cur_func >= 0 &&
            func_ret_struct[gen_cur_func] > 0) {
            /* 構造体の値返し (v45): sret (fp の真上、
             * 引数領域の先頭スロット) の指す先へ値を
             * 複写し、sret を a0 に入れて戻る */
            int tag = func_ret_struct[gen_cur_func];
            if (node_lhs[node] < 0) {
                errmsg = "構造体を返す関数には return の値が要ります";
                return 0;
            }
            int expr = node_lhs[node];
            if (value_struct(expr) == 0) {
                errmsg = "構造体を返す関数からは構造体を返します";
                return 0;
            }
            if (value_struct(expr) != tag) {
                errmsg = "構造体の型 (タグ) が合いません";
                return 0;
            }
            if (node_kind[expr] == NODE_CALL ||
                node_kind[expr] == NODE_ICALL) {
                /* 自分の sret をそのまま相手へ渡す
                 * (複写が1回省ける) */
                asm_push(OP_LD, REG_T2, REG_FP, 0, 0);
                asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
                asm_push(OP_SD, 0, REG_SP, REG_T2, 0);
                if (!gen_b_call(expr, 0))
                    return 0;
            } else {
                if (!gen_lvalue_addr(expr))
                    return 0;
                asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
                asm_push(OP_SD, 0, REG_SP, REG_T2, 0);
                asm_push(OP_LD, REG_A0, REG_FP, 0, 0);
                asm_push(OP_LD, REG_T2, REG_SP, 0, 0);
                asm_push(OP_ADDI, REG_SP, REG_SP, 0, 16);
                gen_copy_slots(struct_slots(tag));
                asm_push(OP_LD, REG_A0, REG_FP, 0, 0);
            }
            emit_patch_jump(&return_patch_head);
            return 1;
        }
        if (node_lhs[node] >= 0 &&
            value_struct(node_lhs[node]) > 0) {
            errmsg = "構造体の値は構造体を返す関数からだけ返せます";
            return 0;
        }
        if (node_lhs[node] >= 0) {
            /* 値は関数の戻り値の型に合わせて生成する
             * (v46: double を返すなら ft0 に残るので、
             * ビットとして a0 へ戻す) */
            int ret_et = gen_cur_func >= 0
                ? func_ret_etype[gen_cur_func] : 8;
            if (!gen_value_as(node_lhs[node], ret_et))
                return 0;
            if (ret_et & 64)
                asm_push(OP_FMV_X_D, REG_T1, REG_FT0, 0, 0);
            asm_push(OP_ADDI, REG_A0, REG_T1, 0, 0);
        }
        emit_patch_jump(&return_patch_head);
        return 1;
    }
    errmsg = "内部エラー: 知らない種類の文です";
    return 0;
}

/* switch の生成 (v43、設計は doc/switch.md):
 *   制御式を計算 (値は t1)
 *   制御式の値をスタックへ退避 (比べる並びが t1 / t2 を
 *     使うため。二項演算の退避と同じ形)
 *   case ごとに: 値を t1 に戻し、定数を t2 に作って引き、
 *     0 なら (emit_beq_t1z) その case 群へ飛ぶ
 *   jal で default 群 (あれば) か末尾へ (外れたとき)
 *   退避を戻す
 *   case 群を順に並べる (fallthrough は自然に起きる)
 * 比べる分岐には条件分岐の伸長 (v27) と同じ
 * emit_beq_t1z を使う。飛び先は patch_next で繋いでおき、
 * case 群を並べるときに順に確定させる。
 * 定数は制御式の型の正規形に直してから比べる: 幅に
 * 切り詰め、符号なしは 0 埋め、符号つきは符号拡張
 * (v35 の正規形の約束に合わせる)。定数の作り方は
 * gen_load_u32 (32ビットのパターンを 0 埋めで広げる)
 * に、負なら slli / srai 32 の符号拡張を足す形で、
 * 式の中の数リテラルと同じ流儀に揃える */
static int gen_switch(int node) {
    if (node_etype[node_lhs[node]] & 64) {
        /* switch の制御式と case は整数の世界 (v46、
         * doc/float.md の切り捨て) */
        errmsg = "switch の制御式に浮動小数点は使えません";
        return 0;
    }
    if (!gen_expr(node_lhs[node]))
        return 0;
    int et = node_etype[node_lhs[node]];
    int esz = et & 15;
    int euns = (et & 16) != 0;
    asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
    asm_push(OP_SD, 0, REG_SP, REG_T1, 0);
    int head = -1;
    int tail = -1;
    for (int g = node_rhs[node]; g != -1; g = node_next[g]) {
        if (node_rhs[g] != 0)
            continue;  /* default は比べない */
        int v = node_val[g];
        long cv = v;
        if (esz == 1) {
            cv = v & 0xFF;
            if (!euns && cv >= 128)
                cv = cv - 256;
        } else if (esz == 2) {
            cv = v & 0xFFFF;
            if (!euns && cv >= 32768)
                cv = cv - 65536;
        } else if (esz == 4 && euns) {
            cv = v & 0xFFFFFFFFL;
        }
        asm_push(OP_LD, REG_T1, REG_SP, 0, 0);
        gen_load_u32(REG_T2, (int)cv);
        if (cv < 0) {
            /* 32ビットのパターンを符号拡張する */
            asm_push(OP_SLLI, REG_T2, REG_T2, 0, 32);
            asm_push(OP_SRAI, REG_T2, REG_T2, 0, 32);
        }
        asm_push(OP_SUB, REG_T1, REG_T1, REG_T2, 0);
        int s = emit_beq_t1z();
        patch_next[s] = -1;
        if (head < 0)
            head = s;
        else
            patch_next[tail] = s;
        tail = s;
    }
    /* どれにも合わなければ default 群 (あれば) か末尾へ */
    int miss = nasm;
    asm_push(OP_JAL, 0, 0, 0, 0);
    asm_push(OP_ADDI, REG_SP, REG_SP, 0, 16);
    if (sw_depth >= 32) {
        errmsg = "switch の入れ子が深すぎます";
        return 0;
    }
    sw_depth++;
    sw_break_head[sw_depth] = -1;
    ctrl_depth++;
    ctrl_is_switch[ctrl_depth] = 1;
    int site = head;
    int def_target = -1;
    for (int g = node_rhs[node]; g != -1; g = node_next[g]) {
        if (node_rhs[g] == 0) {
            /* この case への分岐の飛び先をここに確定 */
            asm_imm[site] = nasm;
            site = patch_next[site];
        } else {
            def_target = nasm;  /* default 群の頭を控える */
        }
        for (int s = node_lhs[g]; s != -1; s = node_next[s]) {
            if (!gen_stmt(s))
                return 0;
        }
    }
    if (def_target >= 0)
        asm_imm[miss] = def_target;
    else
        asm_imm[miss] = nasm;
    apply_patches(sw_break_head[sw_depth], nasm);
    sw_depth--;
    ctrl_depth--;
    return 1;
}

/* スタックの天井の住所 (第2層が2パス間で確定させる)。
 * スタックはバイナリの後ろの空き地に置き、下へ伸ばす */
static unsigned int stack_top_addr;

/* main の argc / argv (v29): KosabaOS が入口の約束で
 * a0 = argc、a1 = argv を渡すようになった (OS 側
 * 2aa149b) ので、入口はレジスタをそのまま main へ
 * 渡す。v22 の決め打ち argv (静的領域に組み立てる
 * 方式) は廃止し、argv のための静的領域も無くなった */

/* 関数1つ分の命令列を作る: 入口の決まり文句 (プロローグ)
 * → 本体 → 出口の決まり文句 (エピローグ)。
 *   プロローグ: スタックに場所を取り、戻り先 ra を退避。
 *               引数があれば a0 から引数の変数へ写す
 *   エピローグ: ra をスタックから戻し、場所を返して、
 *               jalr で呼び出し側へ戻る
 * ra を退避するのが肝心で、これがあるから関数の中で
 * さらに別の関数を呼んでも (ra が上書きされても)
 * 自分の戻り先を失わない。失敗したら 0 */
/* fp 相対の住所をレジスタに作る (v45): ずれが即値に
 * 収まらないときは定数を経由する */
static void emit_fp_addr(int reg, int off) {
    if (off >= -2048 && off < 2048) {
        asm_push(OP_ADDI, reg, REG_FP, 0, off);
    } else {
        gen_load_const(reg, off);
        asm_push(OP_ADD, reg, REG_FP, reg, 0);
    }
}

/* 引数領域 (fp 相対、16バイト間隔) から枠 (fp 相対、
 * 8バイト間隔) へ1スロット写す (v45) */
static void emit_copy_fp(int dst_off, int src_off) {
    if (src_off >= -2048 && src_off < 2048) {
        asm_push(OP_LD, REG_T1, REG_FP, 0, src_off);
    } else {
        emit_fp_addr(REG_T2, src_off);
        asm_push(OP_LD, REG_T1, REG_T2, 0, 0);
    }
    asm_push(OP_SD, 0, REG_FP, REG_T1, dst_off);
}

/* 引数領域 (fp 相対) からレジスタへ1スロット写す (v56)。
 * レジスタ割り付けされた B方式の引数の受け取り用 */
static void emit_copy_fp_reg(int reg, int src_off) {
    if (src_off >= -2048 && src_off < 2048) {
        asm_push(OP_LD, reg, REG_FP, 0, src_off);
    } else {
        emit_fp_addr(REG_T2, src_off);
        asm_push(OP_LD, reg, REG_T2, 0, 0);
    }
}

static int gen_func(int f) {
    /* オブジェクト出力 (v31) では、宣言だけの関数に
     * 中身を作らない (一括では空の体ができるが、
     * オブジェクトでは「定義は別ファイル」が普通で、
     * 空の体が場所を取ると配置がずれる) */
    if (obj_mode && !func_has_body[f]) {
        func_entry[f] = nasm;
        return 1;
    }
    func_entry[f] = nasm;
    int frame = func_frame_size(f);
    /* 序文 (v21): フレームを取り、ra と呼び出し側の fp を
     * 退避し、fp を張る。ローカルと引数は fp 相対で触る
     * ので、式の評価で sp が上下しても住所が変わらない。
     *   addi sp, sp, -frame
     *   sd ra, frame-8(sp) / sd fp, frame-16(sp)
     *   addi fp, sp, frame */
    asm_push(OP_ADDI, REG_SP, REG_SP, 0, -frame);
    asm_push(OP_SD, 0, REG_SP, REG_RA, frame - 8);
    asm_push(OP_SD, 0, REG_SP, REG_FP, frame - 16);
    asm_push(OP_ADDI, REG_FP, REG_SP, 0, frame);
    /* レジスタ割り付け (v56): 使う s レジスタを
     * fp-24 から下向きに退避する (呼び出し側の
     * 値を守る callee-saved の約束) */
    for (int i = 0; i < func_saved_regs[f]; i++)
        asm_push(OP_SD, 0, REG_FP, saved_reg_no(i),
                 -(24 + i * 8));
    if (call_b_mode(func_sig[f])) {
        /* B 方式の引数の受け取り (v45): 引数は呼び側が
         * スタックに積んであり、fp の真上 (引数領域) に
         * 16バイト間隔で並ぶ。自分の枠へ写す。構造体の
         * 戻り値があるとき、先頭のスロットは sret
         * (戻り値の置き場の住所) で、ここでは触らない
         * (return のときに fp 相対で読む) */
        int sig = func_sig[f];
        int slot = sig_ret_struct[sig] > 0 ? 1 : 0;
        for (int k = 0; k < func_nparams[f]; k++) {
            int var = func_param_var[f][k];
            if (sig_pstruct[sig][k] > 0 &&
                sig_pplevel[sig][k] == 0) {
                /* 構造体: 引数領域 (16バイト間隔) から
                 * 枠 (8バイト間隔) へスロットごとに写す */
                int ns = struct_slots(sig_pstruct[sig][k]);
                for (int j = 0; j < ns; j++)
                    emit_copy_fp(var_frame_off(var) + j * 8,
                                 (slot + j) * 16);
            } else if (var_reg[var] != 0) {
                /* レジスタ割り付け (v56): 枠でなく
                 * レジスタへ写す */
                emit_copy_fp_reg(var_reg[var], slot * 16);
            } else {
                emit_copy_fp(var_frame_off(var), slot * 16);
            }
            slot += arg_slots(sig, k);
        }
    } else {
        /* 引数を受け取る: a0〜a3 を引数の枠へ順に書く (v22)。
         * レジスタ割り付けの引数 (v56) は枠でなく
         * レジスタへ移す */
        for (int k = 0; k < func_nparams[f]; k++) {
            int var = func_param_var[f][k];
            if (var_reg[var] != 0)
                asm_push(OP_ADDI, var_reg[var],
                         REG_A0 + k, 0, 0);
            else
                asm_push(OP_SD, 0, REG_FP, REG_A0 + k,
                         var_frame_off(var));
        }
    }
    return_patch_head = -1;
    gen_in_func = 1;
    gen_cur_func = f;
    type_list(func_body[f]);
    for (int s = func_body[f]; s != -1; s = node_next[s]) {
        if (!gen_stmt(s))
            return 0;
    }
    gen_in_func = 0;
    gen_cur_func = -1;
    /* 早期 return の飛び先はここ (後始末の頭) */
    apply_patches(return_patch_head, nasm);
    /* レジスタ割り付け (v56): 退避した s レジスタを
     * 戻す。fp がまだ自分のうちに読む */
    for (int i = 0; i < func_saved_regs[f]; i++)
        asm_push(OP_LD, saved_reg_no(i), REG_FP, 0,
                 -(24 + i * 8));
    /* 後始末: ra と fp を戻し (fp の読みは古い fp で
     * 行う)、フレームを返して、呼び出し側へ戻る
     *   ld ra, -8(fp) / ld fp, -16(fp) / addi sp, sp, frame
     *   jalr zero, ra, 0 */
    asm_push(OP_LD, REG_RA, REG_FP, 0, -8);
    asm_push(OP_LD, REG_FP, REG_FP, 0, -16);
    asm_push(OP_ADDI, REG_SP, REG_SP, 0, frame);
    asm_push(OP_JALR, 0, REG_RA, 0, 0);
    return 1;
}

/* スタックが要るかどうかを、AST を歩いて先に調べる。
 * 要るのは「関数がある」か「右の子が葉でない二項演算が
 * ある (評価に退避が要る)」場合。その場合だけ gen_all が
 * sp の用意を出すので、要らないプログラムの出力は
 * 従来と1バイトも変わらない */
static int stack_needed;

/* プログラムが浮動小数点を使うか (v46)。名簿と
 * ノードを先に眺めて決める (stack_needed と同じ流儀)。
 * ベアメタルの入口は、使うプログラムだけ FPU の
 * 有効化 (sstatus の FS) を出す: QEMU + OpenSBI の
 * S モードでは FS が立っていないと浮動小数点命令が
 * 不正命令になる (fmv だけは通る)。KosabaOS 向けは
 * OS が有効化するので出さない */
static int prog_uses_fp;

static int scan_uses_fp(void) {
    for (int v = 0; v < nvar; v++)
        if (var_etype[v] & 64)
            return 1;
    for (int a = 0; a < narr; a++)
        if (arr_etype[a] & 64)
            return 1;
    for (int m = 0; m < nsmem; m++)
        if (smem_etype[m] & 64)
            return 1;
    for (int f = 0; f < nfunc; f++)
        if (func_ret_etype[f] & 64)
            return 1;
    for (int s = 0; s < nsig; s++)
        if (sig_ret_etype[s] & 64)
            return 1;
    for (int n = 0; n < nnode; n++)
        if (node_kind[n] == NODE_NUMFP)
            return 1;
    return 0;
}

static int expr_needs_stack(int node) {
    int k = node_kind[node];
    if (k == NODE_ADD || k == NODE_SUB || k == NODE_AND ||
        k == NODE_EQ || k == NODE_NE || k == NODE_MUL ||
        k == NODE_DIV || k == NODE_LT || k == NODE_GT ||
        k == NODE_LE || k == NODE_GE || k == NODE_MOD ||
        k == NODE_SHL || k == NODE_SHR || k == NODE_OR ||
        k == NODE_XOR) {
        if (!is_leaf(node_rhs[node]))
            return 1;
        return expr_needs_stack(node_lhs[node]) ||
               expr_needs_stack(node_rhs[node]);
    }
    if (k == NODE_LAND || k == NODE_LOR)
        /* 短絡評価は左を退避しないが、中の式が退避を
         * 要することはある */
        return expr_needs_stack(node_lhs[node]) ||
               expr_needs_stack(node_rhs[node]);
    if (k == NODE_NOT || k == NODE_NEG || k == NODE_ADDR ||
        k == NODE_BITNOT || k == NODE_CAST)
        return expr_needs_stack(node_lhs[node]);
    if (k == NODE_COND)
        /* 三項 (v25): 条件と両枝のどこかで退避が要るか */
        return expr_needs_stack(node_lhs[node]) ||
               expr_needs_stack(node_val[node]) ||
               expr_needs_stack(node_rhs[node]);
    if (k == NODE_EPRE || k == NODE_EPOST)
        /* 要素の ++ / -- (v25): 住所作りの中の式を見る */
        return expr_needs_stack(node_lhs[node]);
    if (k == NODE_CALL || k == NODE_SYSCALL3) {
        /* 呼び出しの引数の式が退避を要するか (v22)。
         * syscall3 も引数の並び方は同じ (v27) */
        for (int a = node_lhs[node]; a != -1; a = node_next[a]) {
            if (expr_needs_stack(a))
                return 1;
        }
        return 0;
    }
    if (k == NODE_ICALL) {
        /* 間接呼び出し (v44): 呼び先の式と引数の並び */
        if (expr_needs_stack(node_lhs[node]))
            return 1;
        for (int a = node_rhs[node]; a != -1; a = node_next[a]) {
            if (expr_needs_stack(a))
                return 1;
        }
        return 0;
    }
    if (k == NODE_FADDR)
        return 0;
    if (k == NODE_DEREF)
        return expr_needs_stack(node_lhs[node]);
    if (k == NODE_INDEX)
        return expr_needs_stack(node_rhs[node]);
    if (k == NODE_INDEX2)
        /* 2次元の住所作りは行をスタックに退避する (v23) */
        return 1;
    if (k == NODE_ROWADDR || k == NODE_PINDEX)
        return expr_needs_stack(node_rhs[node]);
    if (k == NODE_MEMBER)
        return expr_needs_stack(node_lhs[node]);
    if (k == NODE_MELEM)
        /* 住所作りがスタックへ退避する (2次元と同じ) */
        return 1;
    return 0;
}

static int list_needs_stack(int head);  /* 相互に使うので先行宣言 */

static int stmt_needs_stack(int node) {
    int k = node_kind[node];
    if (k == NODE_ASSIGN || k == NODE_PUTC) {
        /* 構造体の丸ごと代入は住所の退避にスタックを使う
         * (v45) */
        if (k == NODE_ASSIGN &&
            ((var_struct[node_val[node]] > 0 &&
              var_plevel[node_val[node]] == 0) ||
             value_struct(node_lhs[node]) > 0))
            return 1;
        return expr_needs_stack(node_lhs[node]);
    }
    if (k == NODE_WHILE || k == NODE_DOWHILE)
        return expr_needs_stack(node_lhs[node]) ||
               list_needs_stack(node_rhs[node]);
    if (k == NODE_IF)
        return expr_needs_stack(node_lhs[node]) ||
               list_needs_stack(node_rhs[node]) ||
               (node_val[node] >= 0 &&
                list_needs_stack(node_val[node]));
    if (k == NODE_CALL) {
        /* 引数の式の並びのどれかが退避を要するか (v22) */
        for (int a = node_lhs[node]; a != -1; a = node_next[a]) {
            if (expr_needs_stack(a))
                return 1;
        }
        return 0;
    }
    if (k == NODE_ICALL) {
        /* 間接呼び出しの文 (v44) */
        if (expr_needs_stack(node_lhs[node]))
            return 1;
        for (int a = node_rhs[node]; a != -1; a = node_next[a]) {
            if (expr_needs_stack(a))
                return 1;
        }
        return 0;
    }
    if (k == NODE_FOR)
        /* 初期化・更新は文、本体は文の並び、条件は式。
         * 初期化は文の鎖のこともある (v45) */
        return (node_val[node] >= 0 &&
                list_needs_stack(node_val[node])) ||
               expr_needs_stack(node_lhs[node]) ||
               list_needs_stack(node_rhs[node]);
    if (k == NODE_FORSTEP)
        return stmt_needs_stack(node_val[node]);
    if (k == NODE_SWITCH) {
        /* switch (v43): 制御式と各 case 群の文を見る。
         * 比べる並びは制御式の値をスタックへ退避するので、
         * default でない case が1つでもあれば要る */
        int need = expr_needs_stack(node_lhs[node]);
        for (int g = node_rhs[node]; g != -1; g = node_next[g]) {
            if (node_rhs[g] == 0)
                need = 1;
            if (list_needs_stack(node_lhs[g]))
                need = 1;
        }
        return need;
    }
    if (k == NODE_STORE) {
        /* ポインタ越しの構造体の複写も退避を使う (v45) */
        if ((node_kind[node_lhs[node]] == NODE_VAR &&
             var_plevel[node_val[node_lhs[node]]] == 1 &&
             var_struct[node_val[node_lhs[node]]] > 0) ||
            value_struct(node_rhs[node]) > 0)
            return 1;
        return expr_needs_stack(node_rhs[node]);
    }
    if (k == NODE_INDEX_STORE)
        /* 値が葉でない書き込みは、住所の退避にスタックを使う。
         * 構造体の要素への複写も同じ (v45) */
        return (arr_plevel[node_val[node]] == 0 &&
                arr_struct[node_val[node]] > 0) ||
               value_struct(node_rhs[node]) > 0 ||
               expr_needs_stack(node_lhs[node]) ||
               !is_leaf(node_rhs[node]) ||
               expr_needs_stack(node_rhs[node]);
    if (k == NODE_ELEM_STORE)
        /* lhs は要素を表す式ノード (式として数える)。
         * 値が葉でない書き込みは住所の退避にスタックを
         * 使う。2次元なら住所作り自体も退避する (v23)。
         * 構造体のメンバへの複写も退避を使う (v45) */
        return value_struct(node_lhs[node]) > 0 ||
               value_struct(node_rhs[node]) > 0 ||
               expr_needs_stack(node_lhs[node]) ||
               !is_leaf(node_rhs[node]) ||
               expr_needs_stack(node_rhs[node]);
    return 0;
}

static int list_needs_stack(int head) {
    for (int s = head; s != -1; s = node_next[s]) {
        if (stmt_needs_stack(s))
            return 1;
    }
    return 0;
}

/* ================================================================
 * peephole 最適化 (v52): gen が並べた命令列を、呼び出しの
 * 飛び先を書き直す前に局所的に磨く。設計は doc/peephole.md。
 *
 * やること (窓の中だけを見る局所的な書き換え):
 *   (1) nop move (addi rd, rd, 0) を消す
 *   (2) store の直後の同じスロットへの load が、store した
 *       レジスタ宛なら load を消す (dead reload)
 *   (3) 別レジスタ宛なら load を mv に替える (forwarding)
 *   (4) load した値をそのまま同じスロットへ戻す store を消す
 * 消す命令は asm_op を -1 にして印を付け、最後に圧縮する。
 * 分岐・ジャンプの飛び先は絶対命令番号なので、圧縮の前に
 * 「消えた命令の数」を差し引いて付け替える。命令番号を
 * 握る者は飛び先の他に func_entry[]、dfaddr_site[]、
 * obj_reloc_site[]、obj_abs_site[]、obj_flush_site、
 * obj_prefix_insts があり、全部付け替える。
 * 飛び先になっている命令と、住所の組 (auipc + addi) は
 * 消さない (peep_bits に印を付けて守る)。住所の組は
 * dfaddr (関数の住所) と obj_reloc / obj_abs (データの
 * 住所とオブジェクト出力の穴) が控えている。
 * ================================================================ */
static char *peep_bits;  /* 飛び先・保護位置の印 (ビット列)。
                          * 置き場は patch_next の先頭を借りる:
                          * パッチの鎖は生成が終われば用済みで、
                          * peep は生成の最後にしか動かない。
                          * 専用の表を持つと断片プロファイルの
                          * 1MB 枠を超えるための苦肉の策 */

/* 印の読み書き (位置 j のビット) */
static int peep_istgt(int j) {
    return (peep_bits[j >> 3] >> (j & 7)) & 1;
}

static void peep_settgt(int j) {
    peep_bits[j >> 3] = peep_bits[j >> 3] | (1 << (j & 7));
}

/* 命令 i がレジスタ r を書き換えるか。ストアと分岐は
 * asm_rd を行き先に使わないので除く (呼び出しと ecall は
 * 呼び出し側で障壁として先に止める) */
static int peep_writes(int i, int r) {
    int op = asm_op[i];
    if (op == OP_SD || op == OP_SW || op == OP_SB ||
        op == OP_SH || op == OP_FSD || op == OP_BEQ ||
        op == OP_BNE || op == OP_BLT || op == OP_BGE ||
        op == OP_BLTU || op == OP_BGEU ||
        op == OP_ECALL || op == OP_WFI)
        return 0;
    return asm_rd[i] == r;
}

static int peep_is_store(int op) {
    return op == OP_SD || op == OP_SW || op == OP_SB ||
           op == OP_SH || op == OP_FSD;
}

/* 呼び出しと ecall はメモリをどう触るか分からないので
 * パターンを跨がない */
static int peep_is_barrier(int op) {
    return op == OP_JAL || op == OP_JALR || op == OP_ECALL;
}

/* 位置 pos より前に消えた (asm_op が -1 の) 命令の数 */
static int peep_deleted_before(int pos) {
    int c = 0;
    int i;
    for (i = 0; i < pos; i++)
        if (asm_op[i] < 0)
            c = c + 1;
    return c;
}

static void peep_run(void) {
    int i;
    int j;
    int f;
    /* 住所の控えが溢れた生成では、守るべき組を
     * 特定できないので何もしない (安全側)。
     * obj_nreloc は一括ではデータ住所の控えの借り物 */
    if (dfaddr_n < 0 || obj_nreloc < 0 || obj_nabs < 0)
        return;
    /* 1. 飛び先と保護位置に印を付ける */
    peep_bits = (char *)patch_next;
    for (i = 0; i < 3072; i++)
        peep_bits[i] = 0;
    peep_settgt(0);  /* 入口は落とさない */
    for (i = 0; i < nasm; i++) {
        int op = asm_op[i];
        if ((is_cbranch(op) || op == OP_JAL) &&
            asm_imm[i] >= 0 && asm_imm[i] < nasm)
            peep_settgt(asm_imm[i]);
    }
    for (f = 0; f < nfunc; f++)
        if (func_entry[f] >= 0 && func_entry[f] < nasm)
            peep_settgt(func_entry[f]);
    for (i = 0; i < dfaddr_n; i++) {
        peep_settgt(dfaddr_site[i]);
        if (dfaddr_site[i] + 1 < nasm)
            peep_settgt(dfaddr_site[i] + 1);
    }
    for (i = 0; i < obj_nreloc; i++) {
        peep_settgt(obj_reloc_site[i]);
        if (obj_reloc_site[i] + 1 < nasm)
            peep_settgt(obj_reloc_site[i] + 1);
    }
    for (i = 0; i < obj_nabs; i++) {
        peep_settgt(obj_abs_site[i]);
        if (obj_abs_site[i] + 1 < nasm)
            peep_settgt(obj_abs_site[i] + 1);
    }
    if (obj_flush_site >= 0)
        peep_settgt(obj_flush_site);
    /* 2. パターンの適用 (削除は asm_op = -1 の印で) */
    for (i = 0; i < nasm; i++) {
        int op = asm_op[i];
        if (op < 0)
            continue;
        /* (1) nop move */
        if (op == OP_ADDI && asm_rd[i] == asm_rs1[i] &&
            asm_imm[i] == 0 && !peep_istgt(i)) {
            asm_op[i] = -1;
            continue;
        }
        /* (2)(3) store の後の同じスロットへの load */
        if (op == OP_SD || op == OP_SW) {
            int base = asm_rs1[i];
            int src = asm_rs2[i];
            int off = asm_imm[i];
            int wop = OP_LD;
            if (op == OP_SW)
                wop = OP_LW;
            for (j = i + 1; j < nasm && j <= i + 6; j++) {
                int jop = asm_op[j];
                if (jop < 0)
                    continue;
                if (peep_istgt(j))
                    break;
                if (peep_is_barrier(jop) || peep_is_store(jop))
                    break;
                if (jop == wop && asm_rs1[j] == base &&
                    asm_imm[j] == off) {
                    if (asm_rd[j] == src) {
                        asm_op[j] = -1;  /* dead reload */
                    } else {
                        /* forwarding: load を mv に替える */
                        asm_op[j] = OP_ADDI;
                        asm_rs1[j] = src;
                        asm_rs2[j] = 0;
                        asm_imm[j] = 0;
                    }
                    break;
                }
                if (peep_writes(j, base) || peep_writes(j, src))
                    break;
            }
            continue;
        }
        /* (4) load した値の store-back */
        if ((op == OP_LD || op == OP_LW) &&
            asm_rd[i] != asm_rs1[i]) {
            int rd = asm_rd[i];
            int base = asm_rs1[i];
            int off = asm_imm[i];
            int wop = OP_SD;
            if (op == OP_LW)
                wop = OP_SW;
            for (j = i + 1; j < nasm && j <= i + 4; j++) {
                int jop = asm_op[j];
                if (jop < 0)
                    continue;
                if (peep_istgt(j))
                    break;
                if (peep_is_barrier(jop))
                    break;
                if (jop == wop && asm_rs1[j] == base &&
                    asm_rs2[j] == rd && asm_imm[j] == off) {
                    asm_op[j] = -1;
                    break;
                }
                if (peep_is_store(jop))
                    break;
                if (peep_writes(j, rd) || peep_writes(j, base))
                    break;
            }
        }
    }
    /* 3. 命令番号の付け替えと圧縮 */
    for (i = 0; i < nasm; i++) {
        int op = asm_op[i];
        if ((is_cbranch(op) || op == OP_JAL) &&
            asm_imm[i] >= 0)
            asm_imm[i] = asm_imm[i] -
                         peep_deleted_before(asm_imm[i]);
    }
    for (f = 0; f < nfunc; f++)
        if (func_entry[f] >= 0)
            func_entry[f] = func_entry[f] -
                            peep_deleted_before(func_entry[f]);
    for (i = 0; i < dfaddr_n; i++)
        dfaddr_site[i] = dfaddr_site[i] -
                         peep_deleted_before(dfaddr_site[i]);
    for (i = 0; i < obj_nreloc; i++)
        obj_reloc_site[i] = obj_reloc_site[i] -
                            peep_deleted_before(obj_reloc_site[i]);
    for (i = 0; i < obj_nabs; i++)
        obj_abs_site[i] = obj_abs_site[i] -
                          peep_deleted_before(obj_abs_site[i]);
    if (obj_flush_site >= 0)
        obj_flush_site = obj_flush_site -
                         peep_deleted_before(obj_flush_site);
    obj_prefix_insts = obj_prefix_insts -
                       peep_deleted_before(obj_prefix_insts);
    {
        int w = 0;
        for (i = 0; i < nasm; i++) {
            if (asm_op[i] < 0)
                continue;
            if (w != i) {
                asm_op[w] = asm_op[i];
                asm_rd[w] = asm_rd[i];
                asm_rs1[w] = asm_rs1[i];
                asm_rs2[w] = asm_rs2[i];
                asm_imm[w] = asm_imm[i];
            }
            w = w + 1;
        }
        nasm = w;
    }
}

/* 命令列を全部作る (1回分): 土台 (UART の住所) → (関数が
 * あれば) スタックの用意 → 文の並び → 最後の式 (あれば) →
 * UART へ書く → 止まる → その後に関数の本体を並べる。
 * 関数の本体は「止まる」の後ろに置くので、上から実行して
 * 関数の中に落ちてくることはない (関数は呼び出しでのみ入る)。
 * 最後に、呼び出しの飛び先 (負数の印) を関数の入口番号へ
 * 書き直す。失敗したら 0 */
static int gen_all(void) {
    nasm = 0;
    obj_nreloc = 0;    /* 穴の控えも数え直す (v31) */
    obj_nabs = 0;
    dfaddr_n = 0;      /* 関数の住所の控えも同じく (v44) */
    obj_flush_site = -1;
    loop_depth = 0;    /* 2パス生成のたびに数え直す (v20) */
    sw_depth = 0;      /* switch の深さも同じく数え直す (v43) */
    ctrl_depth = 0;
    branch_ord = 0;    /* 条件分岐の通し番号も数え直す (v27) */
    gen_in_func = 0;
    if (!target_kosaba) {
        /* lui t0, 0x10000 -- UART ベース 0x10000000 を t0 に作る
         * (KosabaOS ターゲットでは UART を直接触らないので不要) */
        asm_push(OP_LUI, REG_T0, 0, 0, 0x10000);
        if (prog_uses_fp) {
            /* FPU の有効化 (v46): sstatus の FS (13ビット
             * 目) を立てる。値の作り方は gen_load_const、
             * 立てる命令は csrs。浮動小数点を使う
             * プログラムの入口だけに出る */
            gen_load_const(REG_T1, 8192);
            asm_push(OP_CSRS, 0, REG_T1, 0, 0x100);
        }
    }
    if (stack_needed && !target_kosaba) {
        /* スタックの用意: 天井の住所を sp に作る
         * (要らないプログラムでは出さない。昔のプログラムの
         * 出力が変わらないようにするため。KosabaOS では
         * スタックは OS が用意するので、これも出さない) */
        gen_load_addr(REG_SP, stack_top_addr);
    }
    type_list(prog_stmts);
    if (prog_has_final)
        type_node(prog_final);
    for (int s = prog_stmts; s != -1; s = node_next[s]) {
        if (!gen_stmt(s))
            return 0;
    }
    if (prog_has_final) {
        if (!gen_expr(prog_final))
            return 0;
        if (node_etype[prog_final] & 64)
            /* 最後の式が double なら、出力の道具が
             * 読む t1 へビットを戻す (v46) */
            asm_push(OP_FMV_X_D, REG_T1, REG_FT0, 0, 0);
        if (target_kosaba) {
            /* 最後の式の出力も putc システムコールで */
            asm_push(OP_ADDI, REG_A0, REG_T1, 0, 0);
            asm_push(OP_ADDI, REG_A7, REG_ZERO, 0, 1);
            asm_push(OP_ECALL, 0, 0, 0, 0);
        } else {
            /* sb t1, 0(t0) -- 最後の式の下1バイトを UART へ */
            asm_push(OP_SB, 0, REG_T0, REG_T1, 0);
        }
    }
    if (prog_main >= 0) {
        /* main の形 (v12): 入口として main を呼ぶ。
         * 飛び先は通常の呼び出しと同じ印 (-(番号+1)) で置き、
         * この後の一括書き直しで確定する。main から戻ると
         * ra の指す次の命令 = 終わりの命令に落ちる。
         * main の入口では、戻り先が ra に入っているのを
         * 通常の関数と同じく退避するので、呼び出しの作法
         * そのままで動く */
        /* main の引数 (v29): KosabaOS では入口の a0 に
         * argc、a1 に argv が入って届く (OS の約束)。
         * 関数の作法でも引数は a0 から順なので、触らずに
         * main を呼べば main のプロローグが枠へ写す。
         * ベアメタルでは引数の届け先が無いので、main に
         * 引数があっても値は不定 (従来どおり) */
        asm_push(OP_JAL, REG_RA, 0, 0, -(prog_main + 1));
        if (target_kosaba) {
            /* main から戻った後 (v29): 終了ステータス
             * (OS 側 d28c7b8 で exit の a0 = ステータスに
             * なった) として、main の戻り値 (t1) を
             * exit の a0 へ渡す。
             * kosaba_flush の約束 (v27): プログラムが
             * kosaba_flush という名前の関数を持っていれば、
             * exit の前に1回呼ぶ。kosaba/ の crt が GCC 版
             * の iwac で呼んでいるのと同じ約束で、
             * ランタイム側の「終了時にやること」(出力
             * バッファの書き切りなど) の置き場所にする。
             * 呼ぶ場合は、戻り値が呼び出しで消えないよう
             * 先にスタックへ退避してから呼ぶ */
            int flush_f = -1;
            for (int f = 0; f < nfunc; f++) {
                if (!func_has_body[f])
                    continue;
                if (func_name_len[f] != 12)
                    continue;
                /* "kosaba_flush" と1文字ずつ比べる
                 * (文字列リテラルの添字は iwac の言語に
                 * 無いので、この形で書く) */
                if (func_names[f][0] == 'k' &&
                    func_names[f][1] == 'o' &&
                    func_names[f][2] == 's' &&
                    func_names[f][3] == 'a' &&
                    func_names[f][4] == 'b' &&
                    func_names[f][5] == 'a' &&
                    func_names[f][6] == '_' &&
                    func_names[f][7] == 'f' &&
                    func_names[f][8] == 'l' &&
                    func_names[f][9] == 'u' &&
                    func_names[f][10] == 's' &&
                    func_names[f][11] == 'h') {
                    flush_f = f;
                    break;
                }
            }
            /* オブジェクト出力 (v31) では、flush が
             * 「このファイルで宣言されている」場合も
             * 呼び出しの形を残す (定義は別ファイルが
             * 持つ。宣言も無いファイルは、flush の
             * ない一括版と出力が一致するよう、従来
             * どおり mv だけにする) */
            int flush_decl = -1;
            if (obj_mode && flush_f < 0) {
                char *want = "kosaba_flush";
                for (int f = 0; f < nfunc && flush_decl < 0; f++) {
                    if (func_name_len[f] != 12)
                        continue;
                    int same = 1;
                    for (int k = 0; k < 12; k++)
                        if (func_names[f][k] != want[k])
                            same = 0;
                    if (same)
                        flush_decl = f;
                }
            }
            if (flush_f >= 0 || flush_decl >= 0) {
                asm_push(OP_ADDI, REG_SP, REG_SP, 0, -16);
                asm_push(OP_SD, 0, REG_SP, REG_T1, 0);
                /* オブジェクト出力 (v31) では、この
                 * ファイルに flush が無くても呼び出し
                 * の形を残す (相手は別ファイルかも
                 * しれない)。行き先はダミーの印
                 * -1000003 で置き、リンカへのメモは
                 * callopt になる */
                asm_push(OP_JAL, REG_RA, 0, 0,
                         flush_f >= 0 ? -(flush_f + 1)
                                      : -1000003);
                if (obj_mode)
                    obj_flush_site = nasm - 1;
                asm_push(OP_LD, REG_A0, REG_SP, 0, 0);
                asm_push(OP_ADDI, REG_SP, REG_SP, 0, 16);
            } else {
                /* mv a0, t1 -- main の戻り値がステータス */
                asm_push(OP_ADDI, REG_A0, REG_T1, 0, 0);
            }
        }
    }
    if (target_kosaba) {
        /* KosabaOS: exit システムコール (番号 3) で終わる。
         * 戻ってくることはない。シェルから起動された場合は
         * シェルへ、'u' で起動された場合はカーネルへ戻る。
         * ステータス (a0) は、main のあるプログラムでは
         * main の戻り値が入れてある (上の v29 の処理)。
         * main の無いプログラムは 0 で終わる */
        if (prog_main < 0)
            asm_push(OP_ADDI, REG_A0, REG_ZERO, 0, 0);
        asm_push(OP_ADDI, REG_A7, REG_ZERO, 0, 3);
        asm_push(OP_ECALL, 0, 0, 0, 0);
    } else {
        /* wfi -- 止まる */
        asm_push(OP_WFI, 0, 0, 0, 0);
    }
    /* 入口の断片はここまで (オブジェクト出力が
     * 先頭の長さとして使う。v31) */
    obj_prefix_insts = nasm;
    for (int f = 0; f < nfunc; f++) {
        if (!gen_func(f))
            return 0;
    }
    /* peephole 最適化 (v52): 飛び先の書き直しの前に、
     * 命令列を局所的に磨く。番号の付け替えまで済ませて
     * あるので、この後の fixup は従来どおりでよい */
    peep_run();
    /* 呼び出しの飛び先を書き直す (印: -(関数番号+1))。
     * オブジェクト出力では書き直さず、印のまま
     * リンカへのメモ (call) にする */
    if (!obj_mode) {
        /* データ住所の組の即値を埋める (v52)。peephole
         * で確定した位置から pc を数え直す */
        bake_daddr();
        for (int i = 0; i < nasm; i++) {
            if (asm_op[i] == OP_JAL && asm_imm[i] < 0)
                asm_imm[i] = func_entry[-asm_imm[i] - 1];
        }
        /* 関数の住所の組 (auipc + addi) の即値を埋める
         * (v44): 相手の住所 = LOAD_BASE + 入口の命令
         * 番号 × 4。分け方は gen_load_addr と同じ */
        for (int i = 0; i < dfaddr_n; i++) {
            int site = dfaddr_site[i];
            unsigned int pc = LOAD_BASE +
                              (unsigned int)site * 4u;
            unsigned int addr = LOAD_BASE +
                (unsigned int)func_entry[dfaddr_func[i]] * 4u;
            long diff = (long)addr - (long)pc;
            long hi = (diff + 0x800) >> 12;
            long lo = diff - (hi << 12);
            asm_imm[site] = (int)hi;
            asm_imm[site + 1] = (int)lo;
        }
    }
    return 1;
}

/* プログラム全体の生成。文字列の住所を確定させるため、
 * 生成を2回行う (2パス生成):
 *   1回目: 命令の個数を数えることが目的。この時点では
 *          文字列の住所は仮の値だが、住所を作る命令の
 *          個数 (auipc + addi) は本物と同じになる
 *   住所の確定: 静的データの先頭 (data_base) を決める。
 *          ベアメタルではコードの後ろ (LOAD_BASE +
 *          命令数 x 4)、KosabaOS ではデータ領域
 *          0x40200000 (v24)。文字列・配列・グローバルは
 *          data_base からの並びで住所が決まる
 *   2回目: 確定した住所で作り直す。これが最終成果になる */
static int gen_program(void) {
    code_insts = 0;
    /* インライン化 (v58): 生成の前に一括で計画する
     * (複製変数でフレームが増えるため、どの関数の
     * 生成よりも先に済ませる) */
    inline_plan_all();
    /* 宣言だけで定義のない関数が呼ばれていたら、ここで
     * 止める (v24)。行き先のない呼び出しは作れない。
     * 外部の関数は、ソースの連結で定義が一緒に入って
     * くる形でのみ解決する (v26 のランタイム結合)。
     * オブジェクト出力 (v31) では止めない: 定義は
     * 別ファイルが持っているのが普通で、点検は
     * リンカの仕事になる (メモは undef として出す) */
    if (!obj_mode) {
        for (int f = 0; f < nfunc; f++) {
            if (func_called[f] && !func_has_body[f]) {
                errmsg = "定義のない関数が呼ばれています (宣言だけの関数です)";
                return 0;
            }
        }
    }
    /* スタックが要るかは生成の前に AST から決める */
    stack_needed = nfunc > 0 || list_needs_stack(prog_stmts) ||
                   (prog_has_final && expr_needs_stack(prog_final));
    /* 浮動小数点を使うかも生成の前に決める (v46) */
    prog_uses_fp = scan_uses_fp();
    for (int f = 0; f < nfunc && !stack_needed; f++)
        stack_needed = list_needs_stack(func_body[f]);
    stack_top_addr = LOAD_BASE;  /* 1回目は仮の値 (個数は同じ) */
    globals_base = LOAD_BASE;    /* 同上 (グローバルの住所) */
    for (int i = 0; i < narr; i++)
        arr_addr[i] = LOAD_BASE;  /* 同上 (配列の住所も仮置き) */
    /* 条件分岐の範囲の点検 (v27): 届かない分岐は長い形に
     * 伸ばす。伸ばすと別の分岐が届かなくなることがある
     * ので、印が増えなくなるまで1回目の生成を繰り返す
     * (emit_beq_t1z の説明参照) */
    for (int i = 0; i < 24576; i++)
        branch_long[i] = 0;
    for (;;) {
        if (!gen_all())
            return 0;
        if (!relax_scan())
            break;
    }
    if (nasm >= 24576) {
        /* asm_push が上限で止めた分 (v39) */
        errmsg = "命令が多すぎます";
        return 0;
    }
    code_insts = nasm;
    if (obj_mode && (obj_nreloc < 0 || obj_nabs < 0)) {
        errmsg = "穴のメモが多すぎます (オブジェクト出力の上限)";
        return 0;
    }
    if (!obj_mode && dfaddr_n < 0) {
        errmsg = "関数の住所を作る場所が多すぎます";
        return 0;
    }
    /* 静的データ (文字列 → 配列 → グローバル) の
     * 置き場所の先頭 (v24 でターゲット別になった):
     *   ベアメタル: コードの後ろ (従来どおり連続配置)
     *   KosabaOS  : OS のデータ領域 0x40200000。コード
     *               領域 (0x40000000、読み・実行のみ) と
     *               分けることで、静的データへの書き
     *               込みが OS 上でもできるようになる
     *               (ELF は PT_LOAD 3本で申告する) */
    unsigned int data_base;
    if (target_kosaba)
        data_base = 0x40200000u;
    else
        data_base = LOAD_BASE + (unsigned int)nasm * 4u;
    for (int i = 0; i < nstr; i++)
        str_addr[i] = data_base + (unsigned int)str_off[i];
    /* グローバル配列の領域は文字列の後ろに置く。
     * ローカル配列はフレームの中なので住所は持たない
     * (gen_load_arr_base が fp 相対で作る、v26) */
    for (int i = 0; i < narr; i++)
        arr_addr[i] = arr_owner[i] < 0
            ? data_base + (unsigned int)str_used +
              (unsigned int)arr_off[i]
            : 0u;
    /* グローバル変数の領域は配列の後ろ (v21)。
     * 8 バイトずつ。初期値のあるものは encode が初期
     * イメージとして埋め、無いものは 0 で始まる
     * (kosaba では memsz の差分としてゼロ埋めされる。
     * ベアメタルでは初期イメージが無い場合、RAM の
     * 更地なので 0 とは限らない。プログラムは代入
     * してから使う約束) */
    globals_base = data_base + (unsigned int)str_used +
                   (unsigned int)arr_used;
    {
        /* スタックは静的領域の後ろ 4KB 先から下へ伸ばす。
         * 16 バイト単位に切り上げておく (置き場所の約束)。
         * 静的領域の末尾はグローバルの後ろ (v29 で argv
         * の領域が無くなった) */
        unsigned int tail = globals_base +
                            (unsigned int)var_nglobal * 8u;
        stack_top_addr = tail + 4096u;
        stack_top_addr = (stack_top_addr + 15u) & ~15u;
    }
    if (!gen_all())
        return 0;
    return 1;
}

