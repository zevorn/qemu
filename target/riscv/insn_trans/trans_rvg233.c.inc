static bool trans_dma(DisasContext *ctx, arg_dma *a)
{
    TCGv dst = get_gpr(ctx, a->rd, EXT_NONE);
    TCGv src = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv gran = get_gpr(ctx, a->rs2, EXT_NONE);
    gen_helper_dma(tcg_env, dst, src, gran);
    return true;
}

static bool trans_sort(DisasContext *ctx, arg_sort *a)
{
    TCGv addr = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv sort_num = get_gpr(ctx, a->rd, EXT_NONE);
    TCGv array_num = get_gpr(ctx, a->rs2, EXT_NONE);
    gen_helper_sort(tcg_env, addr, array_num, sort_num);
    return true;
}

static bool trans_crush(DisasContext *ctx, arg_crush *a)
{
    TCGv dst = get_gpr(ctx, a->rd, EXT_NONE);
    TCGv src = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv num = get_gpr(ctx, a->rs2, EXT_NONE);
    gen_helper_crush(tcg_env, dst, src, num);
    return true;
}

static bool trans_expand(DisasContext *ctx, arg_expand *a)
{
    TCGv dst = get_gpr(ctx, a->rd, EXT_NONE);
    TCGv src = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv num = get_gpr(ctx, a->rs2, EXT_NONE);
    gen_helper_expand(tcg_env, dst, src, num);
    return true;
}
