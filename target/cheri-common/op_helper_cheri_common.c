/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2015-2016 Stacey Son <sson@FreeBSD.org>
 * Copyright (c) 2016-2018 Alfredo Mazzinghi <am2419@cl.cam.ac.uk>
 * Copyright (c) 2016-2020 Alex Richardson <Alexander.Richardson@cl.cam.ac.uk>
 * All rights reserved.
 *
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory under DARPA/AFRL contract FA8750-10-C-0237
 * ("CTSRD"), as part of the DARPA CRASH research programme.
 *
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory (Department of Computer Science and
 * Technology) under DARPA contract HR0011-18-C-0016 ("ECATS"), as part of the
 * DARPA SSITH research programme.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "exec/memop.h"

#include "cheri-lazy-capregs.h"
#include "cheri-bounds-stats.h"
#include "cheri_tagmem.h"

#ifndef TARGET_CHERI
#error "This file should only be compiled for CHERI"
#endif

#ifdef __clang__
#pragma clang diagnostic error "-Wdeprecated-declarations"
#else
#pragma GCC diagnostic error "-Wdeprecated-declarations"
#endif
#define CHERI_HELPER_IMPL(name)                                                \
    __attribute__(                                                             \
        (deprecated("Do not call the helper directly, it will crash at "       \
                    "runtime. Call the _impl variant instead"))) helper_##name

static inline bool is_cap_sealed(const cap_register_t *cp)
{
    // TODO: remove this function and update all callers to use the correct
    // function
    return !cap_is_unsealed(cp);
}

#ifndef TARGET_MIPS
static inline /* Currently needed for other helpers */
#endif
    target_ulong
    check_ddc(CPUArchState *env, uint32_t perm, uint64_t ddc_offset,
              uint32_t len, uintptr_t retpc)
{
    const cap_register_t *ddc = cheri_get_ddc(env);
    target_ulong addr = ddc_offset + cap_get_cursor(ddc);
    check_cap(env, ddc, perm, addr, CHERI_EXC_REGNUM_DDC, len,
              /*instavail=*/true, retpc);
    return addr;
}

target_ulong CHERI_HELPER_IMPL(ddc_check_load(CPUArchState *env,
                                              target_ulong offset, MemOp op))
{
    return check_ddc(env, CAP_PERM_LOAD, offset, memop_size(op), GETPC());
}

target_ulong CHERI_HELPER_IMPL(ddc_check_store(CPUArchState *env,
                                               target_ulong offset, MemOp op))
{
    return check_ddc(env, CAP_PERM_STORE, offset, memop_size(op), GETPC());
}

target_ulong CHERI_HELPER_IMPL(ddc_check_rmw(CPUArchState *env,
                                             target_ulong offset, MemOp op))
{
    return check_ddc(env, CAP_PERM_LOAD | CAP_PERM_STORE, offset,
                     memop_size(op), GETPC());
}

target_ulong CHERI_HELPER_IMPL(pcc_check_load(CPUArchState *env,
                                              target_ulong pcc_offset,
                                              MemOp op))
{
    const cap_register_t *pcc = cheri_get_pcc(env);
    target_ulong addr = pcc_offset + cap_get_cursor(pcc);
    check_cap(env, pcc, CAP_PERM_LOAD, addr, CHERI_EXC_REGNUM_PCC,
              memop_size(op), /*instavail=*/true, GETPC());
    return addr;
}

void CHERI_HELPER_IMPL(cheri_invalidate_tags(CPUArchState *env,
                                             target_ulong vaddr, MemOp op))
{
    cheri_tag_invalidate(env, vaddr, memop_size(op), GETPC());
}

/// Implementations of individual instructions start here

/// Two operand inspection instructions:

target_ulong CHERI_HELPER_IMPL(cgetaddr(CPUArchState *env, uint32_t cb))
{
    /*
     * CGetAddr: Move Virtual Address to a General-Purpose Register
     * TODO: could do this directly from TCG now.
     */
    return (target_ulong)get_capreg_cursor(env, cb);
}

target_ulong CHERI_HELPER_IMPL(cgetbase(CPUArchState *env, uint32_t cb))
{
    /*
     * CGetBase: Move Base to a General-Purpose Register.
     */
    return (target_ulong)cap_get_base(get_readonly_capreg(env, cb));
}

target_ulong CHERI_HELPER_IMPL(cgetflags(CPUArchState *env, uint32_t cb))
{
    /*
     * CGetBase: Move Base to a General-Purpose Register.
     */
    return (target_ulong)get_readonly_capreg(env, cb)->cr_flags;
}

target_ulong CHERI_HELPER_IMPL(cgetlen(CPUArchState *env, uint32_t cb))
{
    /*
     * CGetLen: Move Length to a General-Purpose Register.
     *
     * Note: For 128-bit Capabilities we must handle len >= 2^64:
     * cap_get_length64() converts 1 << 64 to UINT64_MAX
     */
    return (target_ulong)cap_get_length64(get_readonly_capreg(env, cb));
}

target_ulong CHERI_HELPER_IMPL(cgetperm(CPUArchState *env, uint32_t cb))
{
    /*
     * CGetPerm: Move Memory Permissions Field to a General-Purpose
     * Register.
     */
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    cheri_debug_assert((cbp->cr_perms & CAP_PERMS_ALL) == cbp->cr_perms &&
                       "Unknown HW perms bits set!");
    cheri_debug_assert((cbp->cr_uperms & CAP_UPERMS_ALL) == cbp->cr_uperms &&
                       "Unknown SW perms bits set!");

    return (target_ulong)cbp->cr_perms |
           ((target_ulong)cbp->cr_uperms << CAP_UPERMS_SHFT);
}

target_ulong CHERI_HELPER_IMPL(cgetoffset(CPUArchState *env, uint32_t cb))
{
    /*
     * CGetOffset: Move Offset to a General-Purpose Register
     */
    return (target_ulong)cap_get_offset(get_readonly_capreg(env, cb));
}

target_ulong CHERI_HELPER_IMPL(cgetsealed(CPUArchState *env, uint32_t cb))
{
    /*
     * CGetSealed: Move sealed bit to a General-Purpose Register
     */
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    if (cap_is_sealed_with_type(cbp) || cap_is_sealed_entry(cbp))
        return (target_ulong)1;
    assert(cap_is_unsealed(cbp) && "Unknown reserved otype?");
    return (target_ulong)0;
}

target_ulong CHERI_HELPER_IMPL(cgettag(CPUArchState *env, uint32_t cb))
{
    /*
     * CGetTag: Move Tag to a General-Purpose Register
     */
    return (target_ulong)get_capreg_tag(env, cb);
}

target_ulong CHERI_HELPER_IMPL(cgettype(CPUArchState *env, uint32_t cb))
{
    /*
     * CGetType: Move Object Type Field to a General-Purpose Register.
     */
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    const int64_t otype = cap_get_otype(cbp);
    // Must be either a valid positive type < maximum or one of the special
    // hardware-interpreted otypes
    if (otype < 0) {
        cheri_debug_assert(otype <= CAP_FIRST_SPECIAL_OTYPE_SIGNED);
        cheri_debug_assert(otype >= CAP_LAST_SPECIAL_OTYPE_SIGNED);
    } else {
        cheri_debug_assert(otype <= CAP_LAST_NONRESERVED_OTYPE);
    }
    return otype;
}

/// Two operands (both capabilities)

void CHERI_HELPER_IMPL(ccleartag(CPUArchState *env, uint32_t cd, uint32_t cb))
{
    /*
     * CClearTag: Clear the tag bit
     */
    // TODO: could do this without decompressing.
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    cap_register_t result = *cbp;
    result.cr_tag = 0;
    update_capreg(env, cd, &result);
}

void CHERI_HELPER_IMPL(cmove(CPUArchState *env, uint32_t cd, uint32_t cb))
{
    /*
     * CMove: Move Capability to another Register
     */
    // TODO: could do this without decompressing.
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    update_capreg(env, cd, cbp);
}

void CHERI_HELPER_IMPL(cchecktype(CPUArchState *env, uint32_t cs, uint32_t cb))
{
    GET_HOST_RETPC();
    const cap_register_t *csp = get_readonly_capreg(env, cs);
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    /*
     * CCheckType: Raise exception if otypes don't match
     */
    if (!csp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cs);
    } else if (!cbp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cb);
    } else if (cap_is_unsealed(csp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cs);
    } else if (cap_is_unsealed(cbp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cb);
    } else if (csp->cr_otype != cbp->cr_otype ||
               csp->cr_otype > CAP_LAST_NONRESERVED_OTYPE) {
        raise_cheri_exception(env, CapEx_TypeViolation, cs);
    }
}

/// Two operands (capability and int)
void CHERI_HELPER_IMPL(ccheckperm(CPUArchState *env, uint32_t cs,
                                  target_ulong rt))
{
    GET_HOST_RETPC();
    const cap_register_t *csp = get_readonly_capreg(env, cs);
    uint32_t rt_perms = (uint32_t)rt & (CAP_PERMS_ALL);
    uint32_t rt_uperms = ((uint32_t)rt >> CAP_UPERMS_SHFT) & CAP_UPERMS_ALL;
    /*
     * CCheckPerm: Raise exception if don't have permission
     */
    if (!csp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cs);
    } else if ((csp->cr_perms & rt_perms) != rt_perms) {
        raise_cheri_exception(env, CapEx_UserDefViolation, cs);
    } else if ((csp->cr_uperms & rt_uperms) != rt_uperms) {
        raise_cheri_exception(env, CapEx_UserDefViolation, cs);
    } else if ((rt >> (16 + CAP_MAX_UPERM)) != 0UL) {
        raise_cheri_exception(env, CapEx_UserDefViolation, cs);
    }
}

/// Three operands (capability capability capability)
static void cseal_common(CPUArchState *env, uint32_t cd, uint32_t cs,
                         uint32_t ct, bool conditional,
                         uintptr_t _host_return_address)
{
    const cap_register_t *csp = get_readonly_capreg(env, cs);
    const cap_register_t *ctp = get_readonly_capreg(env, ct);
    uint64_t ct_base_plus_offset = cap_get_cursor(ctp);
    /*
     * CSeal: Seal a capability
     */
    if (!csp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cs);
    } else if (!ctp->cr_tag) {
        if (conditional)
            update_capreg(env, cd, csp);
        else
            raise_cheri_exception(env, CapEx_TagViolation, ct);
    } else if (conditional && cap_get_cursor(ctp) == -1) {
        update_capreg(env, cd, csp);
    } else if (!cap_is_unsealed(csp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cs);
    } else if (!cap_is_unsealed(ctp)) {
        raise_cheri_exception(env, CapEx_SealViolation, ct);
    } else if (!(ctp->cr_perms & CAP_PERM_SEAL)) {
        raise_cheri_exception(env, CapEx_PermitSealViolation, ct);
    } else if (!cap_is_in_bounds(ctp, ct_base_plus_offset, /*num_bytes=*/1)) {
        // Must be within bounds -> num_bytes=1
        raise_cheri_exception(env, CapEx_LengthViolation, ct);
    } else if (ct_base_plus_offset > (uint64_t)CAP_LAST_NONRESERVED_OTYPE) {
        raise_cheri_exception(env, CapEx_LengthViolation, ct);
    } else if (!is_representable_cap_when_sealed_with_addr(
                   csp, cap_get_cursor(csp))) {
        raise_cheri_exception(env, CapEx_InexactBounds, cs);
    } else {
        cap_register_t result = *csp;
        cap_set_sealed(&result, (uint32_t)ct_base_plus_offset);
        update_capreg(env, cd, &result);
    }
}

void CHERI_HELPER_IMPL(ccseal(CPUArchState *env, uint32_t cd, uint32_t cs,
                              uint32_t ct))
{
    /*
     * CCSeal: Conditionally seal a capability.
     */
    cseal_common(env, cd, cs, ct, true, GETPC());
}

void CHERI_HELPER_IMPL(cseal(CPUArchState *env, uint32_t cd, uint32_t cs,
                             uint32_t ct))
{
    /*
     * CSeal: Seal a capability
     */
    cseal_common(env, cd, cs, ct, false, GETPC());
}

void CHERI_HELPER_IMPL(cunseal(CPUArchState *env, uint32_t cd, uint32_t cs,
                               uint32_t ct))
{
    GET_HOST_RETPC();
    const cap_register_t *csp = get_readonly_capreg(env, cs);
    const cap_register_t *ctp = get_readonly_capreg(env, ct);
    const uint64_t ct_cursor = cap_get_cursor(ctp);
    /*
     * CUnseal: Unseal a sealed capability
     */
    if (!csp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cs);
    } else if (!ctp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, ct);
    } else if (cap_is_unsealed(csp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cs);
    } else if (!cap_is_unsealed(ctp)) {
        raise_cheri_exception(env, CapEx_SealViolation, ct);
    } else if (!cap_is_sealed_with_type(csp)) {
        raise_cheri_exception(env, CapEx_TypeViolation,
                              cs); /* Reserved otypes */
    } else if (ct_cursor != csp->cr_otype) {
        raise_cheri_exception(env, CapEx_TypeViolation, ct);
    } else if (!(ctp->cr_perms & CAP_PERM_UNSEAL)) {
        raise_cheri_exception(env, CapEx_PermitUnsealViolation, ct);
    } else if (!cap_is_in_bounds(ctp, ct_cursor, /*num_bytes=1*/ 1)) {
        // Must be within bounds and not one past end (i.e. not equal to top ->
        // num_bytes=1)
        raise_cheri_exception(env, CapEx_LengthViolation, ct);
    } else if (ct_cursor >= CAP_LAST_NONRESERVED_OTYPE) {
        // This should never happen due to the ct_cursor != csp->cr_otype check
        // above that should never succeed for
        raise_cheri_exception(env, CapEx_LengthViolation, ct);
    } else {
        cap_register_t result = *csp;
        if ((csp->cr_perms & CAP_PERM_GLOBAL) &&
            (ctp->cr_perms & CAP_PERM_GLOBAL)) {
            result.cr_perms |= CAP_PERM_GLOBAL;
        } else {
            result.cr_perms &= ~CAP_PERM_GLOBAL;
        }
        cap_set_unsealed(&result);
        update_capreg(env, cd, &result);
    }
}

/// Three operands (capability capability int)

#ifdef DO_CHERI_STATISTICS
struct bounds_bucket bounds_buckets[NUM_BOUNDS_BUCKETS] = {
    {1, "1  "}, // 1
    {2, "2  "}, // 2
    {4, "4  "}, // 3
    {8, "8  "}, // 4
    {16, "16 "},
    {32, "32 "},
    {64, "64 "},
    {256, "256"},
    {1024, "1K "},
    {4096, "4K "},
    {64 * 1024, "64K"},
    {1024 * 1024, "1M "},
    {64 * 1024 * 1024, "64M"},
};

DEFINE_CHERI_STAT(cincoffset);
DEFINE_CHERI_STAT(csetoffset);
DEFINE_CHERI_STAT(csetaddr);
DEFINE_CHERI_STAT(candaddr);

static void cincoffset_impl(CPUArchState *env, uint32_t cd, uint32_t cb,
                            target_ulong rt, uintptr_t retpc,
                            struct oob_stats_info *oob_info)
{
    oob_info->num_uses++;
#else
static void cincoffset_impl(CPUArchState *env, uint32_t cd, uint32_t cb,
                            target_ulong rt, uintptr_t retpc, void *dummy_arg)
{
    (void)dummy_arg;
#endif
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    /*
     * CIncOffset: Increase Offset
     */
    if (cbp->cr_tag && is_cap_sealed(cbp)) {
        raise_cheri_exception_impl(env, CapEx_SealViolation, cb, retpc);
    } else {
        uint64_t new_addr = cap_get_cursor(cbp) + rt;
        cap_register_t result = *cbp;
        if (unlikely(!is_representable_cap_with_addr(cbp, new_addr))) {
            if (cbp->cr_tag) {
                became_unrepresentable(env, cd, oob_info, retpc);
            }
            cap_mark_unrepresentable(new_addr, &result);
        } else {
            result._cr_cursor = new_addr;
            check_out_of_bounds_stat(env, oob_info, &result);
        }
        update_capreg(env, cd, &result);
    }
}

void CHERI_HELPER_IMPL(candperm(CPUArchState *env, uint32_t cd, uint32_t cb,
                                target_ulong rt))
{
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    GET_HOST_RETPC();
    /*
     * CAndPerm: Restrict Permissions
     */
    if (!cbp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cb);
    } else if (!cap_is_unsealed(cbp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cb);
    } else {
        uint32_t rt_perms = (uint32_t)rt & (CAP_PERMS_ALL);
        uint32_t rt_uperms = ((uint32_t)rt >> CAP_UPERMS_SHFT) & CAP_UPERMS_ALL;

        cap_register_t result = *cbp;
        result.cr_perms = cbp->cr_perms & rt_perms;
        result.cr_uperms = cbp->cr_uperms & rt_uperms;
        update_capreg(env, cd, &result);
    }
}

void CHERI_HELPER_IMPL(cincoffset(CPUArchState *env, uint32_t cd, uint32_t cb,
                                  target_ulong rt))
{
    return cincoffset_impl(env, cd, cb, rt, GETPC(), OOB_INFO(cincoffset));
}

void CHERI_HELPER_IMPL(candaddr(CPUArchState *env, uint32_t cd, uint32_t cb,
    target_ulong rt))
{
    target_ulong cursor = get_capreg_cursor(env, cb);
    target_ulong target_addr = cursor & rt;
    target_ulong diff = target_addr - cursor;
    cincoffset_impl(env, cd, cb, diff, GETPC(), OOB_INFO(candaddr));
}

void CHERI_HELPER_IMPL(csetaddr(CPUArchState *env, uint32_t cd, uint32_t cb,
                                target_ulong target_addr))
{
    target_ulong cursor = get_capreg_cursor(env, cb);
    target_ulong diff = target_addr - cursor;
    cincoffset_impl(env, cd, cb, diff, GETPC(), OOB_INFO(csetaddr));
}

void CHERI_HELPER_IMPL(csetoffset(CPUArchState *env, uint32_t cd, uint32_t cb,
    target_ulong target_offset))
{
    target_ulong offset = cap_get_offset(get_readonly_capreg(env, cb));
    target_ulong diff = target_offset - offset;
    cincoffset_impl(env, cd, cb, diff, GETPC(), OOB_INFO(csetoffset));
}

static void do_setbounds(bool must_be_exact, CPUArchState *env, uint32_t cd,
                         uint32_t cb, target_ulong length, uintptr_t _host_return_address) {
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    uint64_t cursor = cap_get_cursor(cbp);
    unsigned __int128 new_top = (unsigned __int128)cursor + length; // 65 bits
    /*
     * CSetBounds: Set Bounds
     */
    if (!cbp->cr_tag) {
        raise_cheri_exception(env, CapEx_TagViolation, cb);
    } else if (is_cap_sealed(cbp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cb);
    } else if (cursor < cbp->cr_base) {
        raise_cheri_exception(env, CapEx_LengthViolation, cb);
    } else if (new_top > cap_get_top65(cbp)) {
        raise_cheri_exception(env, CapEx_LengthViolation, cb);
    } else {
        cap_register_t result = *cbp;
#if QEMU_USE_COMPRESSED_CHERI_CAPS
        _Static_assert(CHERI_CAP_SIZE == 16, "");
        /*
         * With compressed capabilities we may need to increase the range of
         * memory addresses to be wider than requested so it is
         * representable.
         */
        const bool exact = cc128_setbounds(&result, cursor, new_top);
        if (!exact)
            env->statcounters_imprecise_setbounds++;
        if (must_be_exact && !exact) {
            raise_cheri_exception(env, CapEx_InexactBounds, cb);
            return;
        }
        assert(cc128_is_representable_cap_exact(&result) && "CSetBounds must create a representable capability");
#else
        (void)must_be_exact;
        /* Capabilities are precise -> can just set the values here */
        result.cr_base = cursor;
        result._cr_top = new_top;
        result._cr_cursor = cursor;
#endif
        assert(result.cr_base >= cbp->cr_base && "CSetBounds broke monotonicity (base)");
        assert(cap_get_length65(&result) <= cap_get_length65(cbp) && "CSetBounds broke monotonicity (length)");
        assert(cap_get_top65(&result) <= cap_get_top65(cbp) && "CSetBounds broke monotonicity (top)");
        update_capreg(env, cd, &result);
    }
}

void CHERI_HELPER_IMPL(csetbounds(CPUArchState *env, uint32_t cd, uint32_t cb,
    target_ulong rt))
{
    do_setbounds(false, env, cd, cb, rt, GETPC());
}

void CHERI_HELPER_IMPL(csetboundsexact(CPUArchState *env, uint32_t cd, uint32_t cb,
    target_ulong rt))
{
    do_setbounds(true, env, cd, cb, rt, GETPC());
}

void CHERI_HELPER_IMPL(csetflags(CPUArchState *env, uint32_t cd, uint32_t cb,
                                 target_ulong flags))
{
    const cap_register_t *cbp = get_readonly_capreg(env, cb);
    GET_HOST_RETPC();
    /*
     * CSetFlags: Set Flags
     */
    if (!cap_is_unsealed(cbp)) {
        raise_cheri_exception(env, CapEx_SealViolation, cb);
    }
    // FIXME: should we trap instead of masking?
    cap_register_t result = *cbp;
    flags &= CAP_FLAGS_ALL_BITS;
    _Static_assert(CAP_FLAGS_ALL_BITS == 1, "Only one flag should exist");
    result.cr_flags = flags;
    update_capreg(env, cd, &result);
}
