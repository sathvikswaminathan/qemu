/*
 * RISC-V translation routines for the RVXI Base Integer Instruction Set.
 *
 * Copyright (c) 2016-2017 Sagar Karandikar, sagark@eecs.berkeley.edu
 * Copyright (c) 2018 Peer Adelt, peer.adelt@hni.uni-paderborn.de
 *                    Bastian Koppelmann, kbastian@mail.uni-paderborn.de
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

static bool trans_illegal(DisasContext *ctx, arg_empty *a)
{
    gen_exception_illegal(ctx);
    return true;
}

static bool trans_lui(DisasContext *ctx, arg_lui *a)
{
    gen_set_gpr_const(a->rd, a->imm);
    return true;
}

static bool trans_auipc(DisasContext *ctx, arg_auipc *a)
{
#ifdef TARGET_CHERI
    if (ctx->capmode) {
        TCGv_i32 dst = tcg_const_i32(a->rd);
        TCGv new_cursor = tcg_const_tl(a->imm + ctx->base.pc_next);
        gen_helper_auipcc(cpu_env, dst, new_cursor);
        tcg_temp_free(new_cursor);
        tcg_temp_free_i32(dst);
        return true;
    }
#endif
    // AUIPC returns a value relative to PCC.base
    gen_set_gpr_const(a->rd, a->imm + ctx->base.pc_next - pcc_base(ctx));
    return true;
}

static bool trans_jal(DisasContext *ctx, arg_jal *a)
{
#ifdef TARGET_CHERI
    if (ctx->capmode) {
        gen_cjal(ctx, a->rd, a->imm);
        return true;
    }
#endif
    gen_jal(ctx, a->rd, a->imm);
    return true;
}

static bool trans_jalr(DisasContext *ctx, arg_jalr *a)
{
#ifdef TARGET_CHERI
    if (ctx->capmode) {
        gen_cjalr(ctx, a->rd, a->rs1, a->imm);
        return true;
    }
#endif
    gen_jalr(ctx, a->rd, a->rs1, a->imm);
    return true;
}

static bool gen_branch(DisasContext *ctx, arg_b *a, TCGCond cond)
{
    TCGLabel *l = gen_new_label();
    TCGv source1, source2;
    source1 = tcg_temp_new();
    source2 = tcg_temp_new();
    gen_get_gpr(source1, a->rs1);
    gen_get_gpr(source2, a->rs2);

    tcg_gen_brcond_tl(cond, source1, source2, l);
    /* Branch not taken, CHERI PCC bounds check done on next ifetch. */
    gen_goto_tb(ctx, 1, ctx->pc_succ_insn, /*bounds_check=*/false);
    gen_set_label(l); /* branch taken */

    if (!has_ext(ctx, RVC) && ((ctx->base.pc_next + a->imm) & 0x3)) {
        /* misaligned */
        gen_exception_inst_addr_mis(ctx);
    } else {
        /* Branch taken -> check if PCC bounds allow for this jump. */
        gen_goto_tb(ctx, 0, ctx->base.pc_next + a->imm, /*bounds_check=*/true);
    }
    ctx->base.is_jmp = DISAS_NORETURN;

    tcg_temp_free(source1);
    tcg_temp_free(source2);

    return true;
}

static bool trans_beq(DisasContext *ctx, arg_beq *a)
{
    return gen_branch(ctx, a, TCG_COND_EQ);
}

static bool trans_bne(DisasContext *ctx, arg_bne *a)
{
    return gen_branch(ctx, a, TCG_COND_NE);
}

static bool trans_blt(DisasContext *ctx, arg_blt *a)
{
    return gen_branch(ctx, a, TCG_COND_LT);
}

static bool trans_bge(DisasContext *ctx, arg_bge *a)
{
    return gen_branch(ctx, a, TCG_COND_GE);
}

static bool trans_bltu(DisasContext *ctx, arg_bltu *a)
{
    return gen_branch(ctx, a, TCG_COND_LTU);
}

static bool trans_bgeu(DisasContext *ctx, arg_bgeu *a)
{
    return gen_branch(ctx, a, TCG_COND_GEU);
}

static bool gen_load(DisasContext *ctx, arg_lb *a, MemOp memop)
{
#ifdef TARGET_CHERI
    if (ctx->capmode) {
        // TODO: LD is LC for RV32
        return gen_cap_load(ctx, a->rd, a->rs1, a->imm, memop);
    }
#endif
    TCGv t0 = tcg_temp_new();
    TCGv t1 = tcg_temp_new();
    gen_get_gpr(t0, a->rs1);
    tcg_gen_addi_tl(t0, t0, a->imm);

    gen_ddc_interposed_ld_tl(ctx, t1, /* Update addr in-place */ NULL, t0,
                             ctx->mem_idx, memop);
    gen_set_gpr(a->rd, t1);
    tcg_temp_free(t0);
    tcg_temp_free(t1);
    return true;
}

static bool trans_lb(DisasContext *ctx, arg_lb *a)
{
    return gen_load(ctx, a, MO_SB);
}

static bool trans_lh(DisasContext *ctx, arg_lh *a)
{
    return gen_load(ctx, a, MO_TESW);
}

static bool trans_lw(DisasContext *ctx, arg_lw *a)
{
    return gen_load(ctx, a, MO_TESL);
}

static bool trans_lbu(DisasContext *ctx, arg_lbu *a)
{
    return gen_load(ctx, a, MO_UB);
}

static bool trans_lhu(DisasContext *ctx, arg_lhu *a)
{
    return gen_load(ctx, a, MO_TEUW);
}

static bool gen_store(DisasContext *ctx, arg_sb *a, MemOp memop)
{
#ifdef TARGET_CHERI
    if (ctx->capmode) {
        // TODO: SD is SC for RV32
        return gen_cap_store(ctx, a->rs1, a->rs2, a->imm, memop);
    }
#endif
    TCGv t0 = tcg_temp_new();
    TCGv dat = tcg_temp_new();
    gen_get_gpr(t0, a->rs1);
    tcg_gen_addi_tl(t0, t0, a->imm);
    gen_get_gpr(dat, a->rs2);

    gen_ddc_interposed_st_tl(ctx, dat, /* Update addr in-place */ NULL, t0,
                             ctx->mem_idx, memop);
    tcg_temp_free(t0);
    tcg_temp_free(dat);
    return true;
}


static bool gen_ustore(DisasContext *ctx, arg_usb *a, MemOp memop)
{
#ifdef TARGET_CHERI
if (ctx->capmode) {
return gen_cap_ustore(ctx, a->rs1, a->rs2, a->rd, memop);
}
#endif
return true;
}

static bool trans_sb(DisasContext *ctx, arg_sb *a)
{
    return gen_store(ctx, a, MO_SB);
}

static bool trans_sh(DisasContext *ctx, arg_sh *a)
{
    return gen_store(ctx, a, MO_TESW);
}

static bool trans_sw(DisasContext *ctx, arg_sw *a)
{
    return gen_store(ctx, a, MO_TESL);
}

#ifdef TARGET_RISCV64
static bool trans_lwu(DisasContext *ctx, arg_lwu *a)
{
    return gen_load(ctx, a, MO_TEUL);
}

static bool trans_ld(DisasContext *ctx, arg_ld *a)
{
    return gen_load(ctx, a, MO_TEQ);
}

static bool trans_sd(DisasContext *ctx, arg_sd *a)
{
    return gen_store(ctx, a, MO_TEQ);
}
#endif

static bool trans_usb(DisasContext *ctx, arg_usb *a)
{
return gen_ustore(ctx, a, MO_SB);
}

static bool trans_ush(DisasContext *ctx, arg_ush *a)
{
return gen_ustore(ctx, a, MO_TESW);
}

static bool trans_usw(DisasContext *ctx, arg_usw *a)
{
return gen_ustore(ctx, a, MO_TESL);
}

#ifdef TARGET_RISCV64
static bool trans_usd(DisasContext *ctx, arg_usd *a)
{
return gen_ustore(ctx, a, MO_TEQ);
}
#endif



static bool trans_addi(DisasContext *ctx, arg_addi *a)
{
    return gen_arith_imm_fn(ctx, a, &tcg_gen_addi_tl);
}

static void gen_slt(TCGv ret, TCGv s1, TCGv s2)
{
    tcg_gen_setcond_tl(TCG_COND_LT, ret, s1, s2);
}

static void gen_sltu(TCGv ret, TCGv s1, TCGv s2)
{
    tcg_gen_setcond_tl(TCG_COND_LTU, ret, s1, s2);
}


static bool trans_slti(DisasContext *ctx, arg_slti *a)
{
#ifdef CONFIG_TCG_LOG_INSTR
    /*
     * If instruction tracing is enabled, we use slti zero, zero, <magic>
     * to perform magic-nop tracing control operations.
     * These will trigger a flush of the TCG buffer, so prepare to resume
     * from next instruction.
     */
    if (unlikely(a->rd == 0)) {
        TCGv tpc = tcg_const_tl(ctx->base.pc_next);
        TCGv_i32 ttmp;

        switch (a->imm) {
        case 0x01: case 0x02:
            ttmp = tcg_const_i32(a->imm == 0x01);
            gen_helper_qemu_log_instr_buffered_mode(cpu_env, ttmp);
            tcg_temp_free_i32(ttmp);
            break;
        case 0x03:
            gen_helper_qemu_log_instr_buffer_flush(cpu_env);
            break;
        case 0x1b:
            gen_helper_qemu_log_instr_start(cpu_env, tpc);
            ctx->base.is_jmp = DISAS_NORETURN;
            break;
        case 0x1e:
            gen_helper_qemu_log_instr_stop(cpu_env, tpc);
            ctx->base.is_jmp = DISAS_NORETURN;
            break;
        case 0x2b:
            gen_helper_qemu_log_instr_user_start(cpu_env, tpc);
            ctx->base.is_jmp = DISAS_NORETURN;
            break;
        case 0x2e:
            gen_helper_qemu_log_instr_stop(cpu_env, tpc);
            ctx->base.is_jmp = DISAS_NORETURN;
            break;
        }
        tcg_temp_free(tpc);

        if (ctx->base.is_jmp != DISAS_NEXT) {
            gen_update_cpu_pc(ctx->pc_succ_insn);
            exit_tb(ctx);
            return true;
        }
    }
#endif
    return gen_arith_imm_tl(ctx, a, &gen_slt);
}

static bool trans_sltiu(DisasContext *ctx, arg_sltiu *a)
{
    return gen_arith_imm_tl(ctx, a, &gen_sltu);
}

static bool trans_xori(DisasContext *ctx, arg_xori *a)
{
    return gen_arith_imm_fn(ctx, a, &tcg_gen_xori_tl);
}
static bool trans_ori(DisasContext *ctx, arg_ori *a)
{
    return gen_arith_imm_fn(ctx, a, &tcg_gen_ori_tl);
}
static bool trans_andi(DisasContext *ctx, arg_andi *a)
{
    return gen_arith_imm_fn(ctx, a, &tcg_gen_andi_tl);
}
static bool trans_slli(DisasContext *ctx, arg_slli *a)
{
    if (a->shamt >= TARGET_LONG_BITS) {
        return false;
    }

    if (a->rd != 0) {
        TCGv t = tcg_temp_new();
        gen_get_gpr(t, a->rs1);

        tcg_gen_shli_tl(t, t, a->shamt);

        gen_set_gpr(a->rd, t);
        tcg_temp_free(t);
    } /* NOP otherwise */
    return true;
}

static bool trans_srli(DisasContext *ctx, arg_srli *a)
{
    if (a->shamt >= TARGET_LONG_BITS) {
        return false;
    }

    if (a->rd != 0) {
        TCGv t = tcg_temp_new();
        gen_get_gpr(t, a->rs1);

        tcg_gen_shri_tl(t, t, a->shamt);
        gen_set_gpr(a->rd, t);
        tcg_temp_free(t);
    } /* NOP otherwise */
    return true;
}

static bool trans_srai(DisasContext *ctx, arg_srai *a)
{
    if (a->shamt >= TARGET_LONG_BITS) {
        return false;
    }

    if (a->rd != 0) {
        TCGv t = tcg_temp_new();
        gen_get_gpr(t, a->rs1);

        tcg_gen_sari_tl(t, t, a->shamt);
        gen_set_gpr(a->rd, t);
        tcg_temp_free(t);
    } /* NOP otherwise */
    return true;
}

static bool trans_add(DisasContext *ctx, arg_add *a)
{
    return gen_arith(ctx, a, &tcg_gen_add_tl);
}

static bool trans_sub(DisasContext *ctx, arg_sub *a)
{
    return gen_arith(ctx, a, &tcg_gen_sub_tl);
}

static bool trans_sll(DisasContext *ctx, arg_sll *a)
{
    return gen_shift(ctx, a, &tcg_gen_shl_tl);
}

static bool trans_slt(DisasContext *ctx, arg_slt *a)
{
    return gen_arith(ctx, a, &gen_slt);
}

static bool trans_sltu(DisasContext *ctx, arg_sltu *a)
{
    return gen_arith(ctx, a, &gen_sltu);
}

static bool trans_xor(DisasContext *ctx, arg_xor *a)
{
    return gen_arith(ctx, a, &tcg_gen_xor_tl);
}

static bool trans_srl(DisasContext *ctx, arg_srl *a)
{
    return gen_shift(ctx, a, &tcg_gen_shr_tl);
}

static bool trans_sra(DisasContext *ctx, arg_sra *a)
{
    return gen_shift(ctx, a, &tcg_gen_sar_tl);
}

static bool trans_or(DisasContext *ctx, arg_or *a)
{
    return gen_arith(ctx, a, &tcg_gen_or_tl);
}

static bool trans_and(DisasContext *ctx, arg_and *a)
{
    return gen_arith(ctx, a, &tcg_gen_and_tl);
}

#ifdef TARGET_RISCV64
static bool trans_addiw(DisasContext *ctx, arg_addiw *a)
{
    return gen_arith_imm_tl(ctx, a, &gen_addw);
}

static bool trans_slliw(DisasContext *ctx, arg_slliw *a)
{
    TCGv source1;
    source1 = tcg_temp_new();
    gen_get_gpr(source1, a->rs1);

    tcg_gen_shli_tl(source1, source1, a->shamt);
    tcg_gen_ext32s_tl(source1, source1);
    gen_set_gpr(a->rd, source1);

    tcg_temp_free(source1);
    return true;
}

static bool trans_srliw(DisasContext *ctx, arg_srliw *a)
{
    TCGv t = tcg_temp_new();
    gen_get_gpr(t, a->rs1);
    tcg_gen_extract_tl(t, t, a->shamt, 32 - a->shamt);
    /* sign-extend for W instructions */
    tcg_gen_ext32s_tl(t, t);
    gen_set_gpr(a->rd, t);
    tcg_temp_free(t);
    return true;
}

static bool trans_sraiw(DisasContext *ctx, arg_sraiw *a)
{
    TCGv t = tcg_temp_new();
    gen_get_gpr(t, a->rs1);
    tcg_gen_sextract_tl(t, t, a->shamt, 32 - a->shamt);
    gen_set_gpr(a->rd, t);
    tcg_temp_free(t);
    return true;
}

static bool trans_addw(DisasContext *ctx, arg_addw *a)
{
    return gen_arith(ctx, a, &gen_addw);
}

static bool trans_subw(DisasContext *ctx, arg_subw *a)
{
    return gen_arith(ctx, a, &gen_subw);
}

static bool trans_sllw(DisasContext *ctx, arg_sllw *a)
{
    TCGv source1 = tcg_temp_new();
    TCGv source2 = tcg_temp_new();

    gen_get_gpr(source1, a->rs1);
    gen_get_gpr(source2, a->rs2);

    tcg_gen_andi_tl(source2, source2, 0x1F);
    tcg_gen_shl_tl(source1, source1, source2);

    tcg_gen_ext32s_tl(source1, source1);
    gen_set_gpr(a->rd, source1);
    tcg_temp_free(source1);
    tcg_temp_free(source2);
    return true;
}

static bool trans_srlw(DisasContext *ctx, arg_srlw *a)
{
    TCGv source1 = tcg_temp_new();
    TCGv source2 = tcg_temp_new();

    gen_get_gpr(source1, a->rs1);
    gen_get_gpr(source2, a->rs2);

    /* clear upper 32 */
    tcg_gen_ext32u_tl(source1, source1);
    tcg_gen_andi_tl(source2, source2, 0x1F);
    tcg_gen_shr_tl(source1, source1, source2);

    tcg_gen_ext32s_tl(source1, source1);
    gen_set_gpr(a->rd, source1);
    tcg_temp_free(source1);
    tcg_temp_free(source2);
    return true;
}

static bool trans_sraw(DisasContext *ctx, arg_sraw *a)
{
    TCGv source1 = tcg_temp_new();
    TCGv source2 = tcg_temp_new();

    gen_get_gpr(source1, a->rs1);
    gen_get_gpr(source2, a->rs2);

    /*
     * first, trick to get it to act like working on 32 bits (get rid of
     * upper 32, sign extend to fill space)
     */
    tcg_gen_ext32s_tl(source1, source1);
    tcg_gen_andi_tl(source2, source2, 0x1F);
    tcg_gen_sar_tl(source1, source1, source2);

    gen_set_gpr(a->rd, source1);
    tcg_temp_free(source1);
    tcg_temp_free(source2);

    return true;
}
#endif

static bool trans_fence(DisasContext *ctx, arg_fence *a)
{
    /* FENCE is a full memory barrier. */
    tcg_gen_mb(TCG_MO_ALL | TCG_BAR_SC);
    return true;
}

static bool trans_fence_i(DisasContext *ctx, arg_fence_i *a)
{
    if (!ctx->ext_ifencei) {
        return false;
    }

    /*
     * FENCE_I is a no-op in QEMU,
     * however we need to end the translation block
     */
    gen_update_cpu_pc(ctx->pc_succ_insn);
    exit_tb(ctx);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

#define RISCV_OP_CSR_PRE do {\
    source1 = tcg_temp_new(); \
    csr_store = tcg_temp_new(); \
    dest = tcg_temp_new(); \
    rs1_pass = tcg_temp_new(); \
    gen_get_gpr(source1, a->rs1); \
    gen_update_cpu_pc(ctx->base.pc_next); \
    tcg_gen_movi_tl(rs1_pass, a->rs1); \
    tcg_gen_movi_tl(csr_store, a->csr); \
    gen_io_start();\
} while (0)

#define RISCV_OP_CSR_POST do {\
    gen_set_gpr(a->rd, dest); \
    gen_update_cpu_pc(ctx->pc_succ_insn); \
    exit_tb(ctx); \
    ctx->base.is_jmp = DISAS_NORETURN; \
    tcg_temp_free(source1); \
    tcg_temp_free(csr_store); \
    tcg_temp_free(dest); \
    tcg_temp_free(rs1_pass); \
} while (0)


static bool trans_csrrw(DisasContext *ctx, arg_csrrw *a)
{
    TCGv source1, csr_store, dest, rs1_pass;
    RISCV_OP_CSR_PRE;
    gen_helper_csrrw(dest, cpu_env, source1, csr_store);
    RISCV_OP_CSR_POST;
    return true;
}

static bool trans_csrrs(DisasContext *ctx, arg_csrrs *a)
{
    TCGv source1, csr_store, dest, rs1_pass;
    RISCV_OP_CSR_PRE;
    gen_helper_csrrs(dest, cpu_env, source1, csr_store, rs1_pass);
    RISCV_OP_CSR_POST;
    return true;
}

static bool trans_csrrc(DisasContext *ctx, arg_csrrc *a)
{
    TCGv source1, csr_store, dest, rs1_pass;
    RISCV_OP_CSR_PRE;
    gen_helper_csrrc(dest, cpu_env, source1, csr_store, rs1_pass);
    RISCV_OP_CSR_POST;
    return true;
}

static bool trans_csrrwi(DisasContext *ctx, arg_csrrwi *a)
{
    TCGv source1, csr_store, dest, rs1_pass;
    RISCV_OP_CSR_PRE;
    gen_helper_csrrw(dest, cpu_env, rs1_pass, csr_store);
    RISCV_OP_CSR_POST;
    return true;
}

static bool trans_csrrsi(DisasContext *ctx, arg_csrrsi *a)
{
    TCGv source1, csr_store, dest, rs1_pass;
    RISCV_OP_CSR_PRE;
    gen_helper_csrrs(dest, cpu_env, rs1_pass, csr_store, rs1_pass);
    RISCV_OP_CSR_POST;
    return true;
}

static bool trans_csrrci(DisasContext *ctx, arg_csrrci *a)
{
    TCGv source1, csr_store, dest, rs1_pass;
    RISCV_OP_CSR_PRE;
    gen_helper_csrrc(dest, cpu_env, rs1_pass, csr_store, rs1_pass);
    RISCV_OP_CSR_POST;
    return true;
}
