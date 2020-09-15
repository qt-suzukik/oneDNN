/*******************************************************************************
* Copyright 2019-2020 FUJITSU LIMITED
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

/*******************************************************************************
* Copyright 2016-2020 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "common/c_types_map.hpp"
#include "common/nstl.hpp"
#include "common/type_helpers.hpp"
#include "common/utils.hpp"

#include "cpu/platform.hpp"
#include "cpu/aarch64/cpu_barrier.hpp"

#include "cpu/aarch64/jit_aarch64_sve_512_conv_kernel.hpp"

#define GET_OFF(field) static_cast<int32_t>(offsetof(jit_conv_call_s, field))
#define KNx_L2_EFFECTIVE_CAPACITY ((512 - 64) * 1024)
#define A64FX_L2_EFFECTIVE_CAPACITY ((666-128)*1024)

// intel L2 1024KiB L1 32KiB
// l2 :7MiB / 12core -  l1 : 64 * 2(inclusive cache) ?

namespace dnnl {
namespace impl {
namespace cpu {
namespace aarch64 {

using namespace dnnl::impl::format_tag;
using namespace dnnl::impl::memory_tracking::names;
using namespace dnnl::impl::utils;
using namespace Xbyak;

#define CGA64 CodeGeneratorAArch64
namespace xa = Xbyak::Xbyak_aarch64;

namespace {

constexpr auto small_spatial = 14;
#if 1
unsigned int L1_cache_size = platform::get_per_core_cache_size(1);
unsigned int L2_cache_size = platform::get_per_core_cache_size(2);
#else
//[info]記述形式が変わっているが以下でよい？
unsigned int L1_cache_size = get_A64FX_cache_size(1, true, 1);
unsigned int L2_cache_size = get_A64FX_cache_size(2, true, 1);
#endif

inline void pick_loop_order(jit_conv_conf_t &jcp) {
    using namespace prop_kind;
    assert(one_of(
            jcp.prop_kind, forward_training, forward_inference, backward_data));
    auto w = (jcp.prop_kind == backward_data) ? jcp.iw : jcp.ow;
    auto h = (jcp.prop_kind == backward_data) ? jcp.ih : jcp.oh;

    // The w in the loop order is currently ignored by 3D BWD_D
    jcp.loop_order = (w <= small_spatial && h <= small_spatial) ? loop_cwgn
                                                                : loop_gncw;
    if (utils::one_of(jcp.src_tag, format_tag::ndhwc, format_tag::nhwc,
                format_tag::nwc)
            && jcp.ngroups > 1 && jcp.oc < 16)
        jcp.loop_order = loop_nhwcg;
}

inline status_t init_tag(format_tag_t &tag, memory_desc_t &md,
        const memory_desc_wrapper &mdw, const format_tag_t tag_value) {
    if (mdw.format_kind() == format_kind::any) {
        CHECK(memory_desc_init_by_tag(md, tag_value));
        tag = tag_value;
    } else {
        tag = mdw.matches_one_of_tag(tag_value);
    }

    if (tag != tag_value) return status::unimplemented;

    return status::success;
}

inline bool is_1stconv(const jit_conv_conf_t &jcp) {
    if (mayiuse(sve))
        return (jcp.ic < 16 && jcp.ngroups == 1);
    else
        return one_of(jcp.ic, 1, 3);
}

inline bool is_ow_threading_on(const jit_conv_conf_t &jcp) {
    return (jcp.nb_ow > 1);
}

inline bool is_iw_threading_on(const jit_conv_conf_t &jcp) {
    return (jcp.nb_iw > 1);
}
#if 0
inline bool is_owb_prefetching(const jit_conv_conf_t &jcp) {
    return (jcp.ver == ver_4fma && is_ow_threading_on(jcp));
}
#else
inline bool is_owb_prefetching(const jit_conv_conf_t &jcp) {
    return (is_ow_threading_on(jcp));
}
#endif

} // namespace

template <typename Vmm>
void _jit_aarch64_sve_512_conv_fwd_kernel<Vmm>::prepare_output(int ur_w) {

    auto zreg_out_s = [=](int i_ur, int i_oc){
        int idx = i_ur + i_oc * jcp.ur_w;
        assert(idx < ker_reg_base_idx);
        return xa::ZRegS(idx);
    };

    for (int k = 0; k < jcp.nb_oc_blocking; k++)
        for (int j = 0; j < ur_w; j++) {
            CGA64::fmov(zreg_out_s(j, k));
            if (!is_owb_prefetching(jcp)) {
                size_t aux_output_offset = get_output_offset(j, k);
                std::string op = "LD";
                prefetch(op, 2, reg_out_prf, aux_output_offset);
//              mic_prefetcht1(EVEX_compress_addr_safe(reg_out_prf, aux_output_offset, reg_out_long_offt));
            }
        }
}

template <typename Vmm>
void _jit_aarch64_sve_512_conv_fwd_kernel<Vmm>::store_output(int ur_w) {

    xa::LabelAArch64 no_update_label, store_label, eltwise_label;

#if 0
//[info]取り敢えずコメント化
    // Note 1: the following code has conditions that fix a regression
    // in Densenet

    assert(IMPLICATION(
            mayiuse(avx512_mic), !is_src_layout_nxc() && !is_dst_layout_nxc()));
#endif

    auto _test = [&](const int cond) {
        // *Note 1
        return CGA64::tst(reg_channel, cond);
    };

    // *Note 1
#if 0
    mov(reg_channel,
            ptr[param1
                    + (mayiuse(avx512_mic) ? GET_OFF(channel)
                                           : GET_OFF(flags))]);
#else
//[info]v0.21と同じcodeを展開
    auto zreg_tmp = [=](int idx){
        return xa::ZReg(idx);
    };
    auto zreg_tmp_s = [=](int idx){
        return xa::ZRegS(idx);
    };

    auto zreg_out = [=](int i_ur, int i_oc){
        int idx = i_ur + i_oc * jcp.ur_w;
        assert(idx < ker_reg_base_idx);
        return xa::ZReg(idx);
    };
    auto zreg_out_s = [=](int i_ur, int i_oc){
        int idx = i_ur + i_oc * jcp.ur_w;
        assert(idx < ker_reg_base_idx);
        return xa::ZRegS(idx);
    };

    CGA64::ldr(reg_channel, xa::ptr(abi_param1_aarch64, GET_OFF(flags)));
#endif

    if (jcp.with_bias) { CGA64::ldr(reg_bias, xa::ptr(abi_param1_aarch64, GET_OFF(bias))); }

    const int oc_tail = jcp.oc_tail;

    if (!jcp.with_sum) {
        auto _jmp = [&](const xa::LabelAArch64 &l) {
            return CGA64::b(xa::NE, l);
        };

        // *Note 1
        _test(FLAG_IC_FIRST);
        _jmp(no_update_label);
    }

#if 1
//[info]取り敢えずv0.21と同一codeを展開
    int reg_ofs = jcp.ur_w * jcp.nb_oc_blocking;
    int num_regs = 32 - reg_ofs;
#endif

    for (int k = 0; k < jcp.nb_oc_blocking; k++)
        for (int j = 0; j < ur_w; j++) {
#if 0
            Vmm vmm = vmm_out(j, k);
            // mask only needed for last oc_block
            if (oc_tail && k + 1 == jcp.nb_oc_blocking)
                vmm = vmm | k_oc_tail_mask | T_z;
            size_t aux_output_offset = get_output_offset(j, k);
            vaddps(vmm,
                    make_safe_addr(
                            reg_out, aux_output_offset, reg_out_long_offt));
#else
//[info]取り敢えずv0.21を参考に展開
            size_t aux_output_offset = get_output_offset(j, k);
            int idx = reg_ofs + ((j + k * ur_w)%num_regs);
            CGA64::add_imm(reg_out_long_offt, reg_out, aux_output_offset, reg_tmp_imm);
            CGA64::ldr(zreg_tmp(idx), xa::ptr(reg_out_long_offt));
            CGA64::fadd(zreg_out_s(j, k), zreg_out_s(j, k), zreg_tmp_s(idx));
#endif
        }

    if (!jcp.with_sum) {
        CGA64::b(eltwise_label);
    } else {
        auto _jmp = [&](const xa::LabelAArch64 &l) {
            return CGA64::b(xa::EQ, l);
        };

        // *Note 1
        _test(FLAG_IC_FIRST);
        _jmp(eltwise_label);
    }

#if 0
    if (jcp.with_bias) {
        for (int k = 0; k < jcp.nb_oc_blocking; k++) {
            int bias_offset = jcp.typesize_out * k * jcp.oc_block;
            for (int j = 0; j < ur_w; j++) {
                Vmm vmm = vmm_out(j, k);
                // mask only needed for last oc_block
                if (oc_tail && k + 1 == jcp.nb_oc_blocking)
                    vmm = vmm | k_oc_tail_mask | T_z;
                vaddps(vmm, EVEX_compress_addr(reg_bias, bias_offset));
            }
            mic_prefetcht1(EVEX_compress_addr(reg_bias, bias_offset + 64));
        }
    }
#else
//[info]v0.21と同一codeを展開
    auto bias_load = [=] (int bias_offset, int idx){
        int ofs = bias_offset;

        if( (VL_OFS(ofs) < LDRMAX) &&
            (VL_OFS(ofs) >= (-1 * LDRMAX)) &&
            ((ofs&0x3f) == 0)){
            CGA64::ldr(zreg_tmp(idx), xa::ptr(reg_bias, static_cast<int32_t>(VL_OFS(ofs))));
        }else{
            CGA64::add_imm(reg_tmp_addr, reg_bias, ofs, reg_tmp_imm);
            CGA64::ldr(zreg_tmp(idx), xa::ptr(reg_tmp_addr));
        }
    };

    CGA64::L_aarch64(no_update_label);
    if (jcp.with_bias) {
        for (int k = 0; k < jcp.nb_oc_blocking; k++) {
            int bias_offset = jcp.typesize_out * k * jcp.oc_block;
            for (int j = 0; j < ur_w; j++) {
                int idx = reg_ofs + ((j + k * ur_w)%num_regs);
                bias_load(bias_offset, idx);
                CGA64::fadd(zreg_out_s(j,k), zreg_out_s(j,k), zreg_tmp_s(idx));
            }
           int ofs = bias_offset + 256; // cache line size ?
           std::string op = "LD";
           prefetch(op, 2, reg_bias, ofs);
           // mic_prefetcht1(EVEX_compress_addr(reg_bias, bias_offset + 64));
        }
    }
#endif

    CGA64::L_aarch64(eltwise_label);
#if 0
    if (jcp.with_eltwise) {
        auto _jmp = [&](const Label &l) {
            return mayiuse(avx512_mic) ? jl(l, T_NEAR) : jz(l, T_NEAR);
        };

        // *Note 1
        _test(mayiuse(avx512_mic) ? jcp.nb_ic - 1 : FLAG_IC_LAST);
        _jmp(store_label);

        if (ur_w == jcp.ur_w) {
            eltwise_injector_->compute_vector_range(
                    0, jcp.nb_oc_blocking * jcp.ur_w);
        } else {
            for (int k = 0; k < jcp.nb_oc_blocking; k++)
                eltwise_injector_->compute_vector_range(
                        k * jcp.ur_w, k * jcp.ur_w + ur_w);
        }
    }
#else
//[info]v0.21と同一codeを展開
//[info]eltwise処理を一時的に削除
    if (jcp.with_eltwise) {
        CGA64::tst(reg_channel, FLAG_IC_LAST);
        CGA64::b(xa::EQ, store_label);

        {
            if (ur_w == jcp.ur_w) {
                eltwise_injector_->compute_vector_range(0,
                        jcp.nb_oc_blocking * jcp.ur_w);
            } else {
                for (int k = 0; k < jcp.nb_oc_blocking; k++)
                    eltwise_injector_->compute_vector_range(k * jcp.ur_w,
                            k * jcp.ur_w + ur_w);
            }
        }
    }
    auto out_str = [=](int j, int k, int aux_output_offset){
        int ofs = aux_output_offset;

        if( (VL_OFS(ofs) < LDRMAX) &&
            (VL_OFS(ofs) >= (-1 * LDRMAX)) &&
            ((ofs&0x3f) == 0)){
            CGA64::str(zreg_out(j, k), xa::ptr(reg_out, static_cast<int32_t>(VL_OFS(ofs))));
        }else{
            CGA64::add_imm(reg_tmp_addr, reg_out, ofs, reg_tmp_imm);
            CGA64::str(zreg_out(j, k), xa::ptr(reg_tmp_addr));
        }
    };

#endif

    CGA64::L_aarch64(store_label);
#if 0
    for (int k = 0; k < jcp.nb_oc_blocking; k++)
        for (int j = 0; j < ur_w; j++) {
            Vmm vmm = vmm_out(j, k);
            // mask only needed for last oc_block
            if (oc_tail && k + 1 == jcp.nb_oc_blocking)
                vmm = vmm | k_oc_tail_mask;
            size_t aux_output_offset = get_output_offset(j, k);

            vmovups(EVEX_compress_addr_safe(
                            reg_out, aux_output_offset, reg_out_long_offt),
                    vmm);
            if (!is_owb_prefetching(jcp))
                mic_prefetcht0(EVEX_compress_addr_safe(
                        reg_out_prf, aux_output_offset, reg_out_long_offt));
        }
#else
//[info]v0.21と同一codeを展開
    for (int k = 0; k < jcp.nb_oc_blocking; k++){
        for (int j = 0; j < ur_w; j++) {
            size_t aux_output_offset = (size_t)typesize *
                ((size_t)k * jcp.od * jcp.oh * jcp.ow + j) * jcp.oc_block;

            out_str(j, k, aux_output_offset);

            if (!is_owb_prefetching(jcp)) {
              std::string op = "ST";
              prefetch(op, 1, reg_out_prf, aux_output_offset);
            }
            // mic_prefetcht0(EVEX_compress_addr_safe(reg_out_prf,
            //             aux_output_offset, reg_out_long_offt));
        }
    }
#endif

}

#if 0
template <typename Vmm>
void _jit_avx512_conv_fwd_kernel<Vmm>::compute_loop_4fma_1st(
        int ur_w, int pad_l, int pad_r) {}

template <>
void _jit_avx512_conv_fwd_kernel<Zmm>::compute_loop_4fma_1st(
        int ur_w, int pad_l, int pad_r) {
    assert(!is_src_layout_nxc() && !is_dst_layout_nxc());
    assert(jcp.dilate_d == 0 && jcp.dilate_h == 0 && jcp.dilate_w == 0);

    /* XXX: BUGBUGBUG - this call does not work when pad_r > 0 || pad_l > 0.
     * However, JIT execution is currently protected within init_conf(). */
    assert(pad_l == 0 && pad_r == 0);

    int iw = jcp.iw;
    int ih = jcp.ih;
    int kw = jcp.kw;
    int stride_w = jcp.stride_w;
    int ic_block = jcp.ic_block;
    int oc_block = jcp.oc_block;

    Label kh_label, kd_label;

    if (one_of(jcp.ndims, 3, 4)) {
        mov(aux_reg_inp, reg_inp);
        mov(aux_reg_ker, reg_ker);
        mov(aux_reg_inp_prf, reg_inp_prf);
    }

    size_t max_input_offset = (size_t)jcp.typesize_in
            * ((size_t)(kw + ur_w * stride_w - pad_l)
                    + (size_t)ic_block * iw * ih * jcp.id);
    assert(reg_inp_prf == reg_long_offt);
    if (max_input_offset > INT_MAX) push(reg_inp_prf);

    if (jcp.ndims == 5) {
        push(reg_out_prf);
        push(reg_out);

        mov(reg_ki, ptr[param1 + GET_OFF(kd_padding)]);
        mov(aux_reg_ker_d, ptr[param1 + GET_OFF(filt)]);
        mov(aux_reg_inp_d, reg_inp);
        mov(aux_reg_inp_d_prf, reg_inp_prf);

        L(kd_label);
    }
    mov(reg_kj, reg_kh);
    if (jcp.ndims == 5) {
        mov(aux_reg_inp, aux_reg_inp_d);
        mov(aux_reg_ker, aux_reg_ker_d);
        mov(aux_reg_inp_prf, aux_reg_inp_d_prf);
    }

    L(kh_label);
    for (int ki = 0; ki < kw; ki += unroll_4fma) {
        for (int ic = 0; ic < ic_block; ic++) {
            for (int i = 0; i < unroll_4fma; i++) {
                int aux_ker_offset = jcp.typesize_in
                        * ((ki + i) * oc_block
                                + ic * kw * jcp.kh * jcp.kd * oc_block);
                if (ki + i < kw)
                    vmovups(vmm_ker(i),
                            EVEX_compress_addr(aux_reg_ker, aux_ker_offset));
                else
                    vpxord(vmm_ker(i), vmm_ker(i), vmm_ker(i));
            }

            int j_start = get_ow_start(ki, pad_l);
            int j_end = get_ow_end(ur_w, ki, pad_r);

            for (int j = j_start, prf_count = 0; j < j_end; j++) {
                size_t kw_unroll
                        = ki + j * static_cast<size_t>(stride_w) - pad_l;
                /* Note: protect against potential illegal memory addressing due
                 * to 4fma overflow in source. */
                assert(kw_unroll + unroll_4fma <= (size_t)iw);
                size_t aux_input_offset = (size_t)jcp.typesize_in
                        * (kw_unroll + (size_t)ic * iw * ih * jcp.id);
                v4fmaddps(vmm_out(j, 0), vmm_ker(0),
                        EVEX_compress_addr_safe(
                                aux_reg_inp, aux_input_offset, reg_long_offt));
                if (ki + prf_count < kw && prf_count < 4
                        && ((ki < 2 && j % 4) || j % 2)) {
                    int aux_ker_offset = jcp.typesize_in
                            * ((ki + prf_count) * oc_block
                                    + ic * kw * jcp.kh * jcp.kd * oc_block
                                    + kw * oc_block);
                    mic_prefetcht0(
                            EVEX_compress_addr(aux_reg_ker, aux_ker_offset));
                    prf_count++;
                }
                if (ki == 0 && j % (64 / (stride_w * jcp.typesize_in)) == 0) {
                    mic_prefetcht0(EVEX_compress_addr_safe(
                            aux_reg_inp_prf, aux_input_offset, reg_long_offt));
                }
                if (ki == 1 && j % (64 / (stride_w * jcp.typesize_in)) == 0) {
                    mic_prefetcht0(EVEX_compress_addr_safe(aux_reg_inp,
                            aux_input_offset + jcp.typesize_in * iw,
                            reg_long_offt));
                }
            }
        }
    }
    add(aux_reg_ker, jcp.typesize_in * kw * oc_block);
    add(aux_reg_inp, jcp.typesize_in * iw);
    add(aux_reg_inp_prf, jcp.typesize_in * iw);

    dec(reg_kj);
    cmp(reg_kj, 0);
    jg(kh_label, T_NEAR);

    if (jcp.ndims == 5) {
        add(aux_reg_inp_d, typesize * jcp.ih * jcp.iw);
        add(aux_reg_ker_d, typesize * jcp.kw * jcp.kh * oc_block);
        add(aux_reg_inp_d_prf, typesize * jcp.ih * jcp.iw);

        dec(reg_ki);
        cmp(reg_ki, 0);
        jg(kd_label, T_NEAR);

        pop(reg_out);
        pop(reg_out_prf);
    }

    if (max_input_offset > INT_MAX) pop(reg_inp_prf);
}

template <typename Vmm>
void _jit_avx512_conv_fwd_kernel<Vmm>::compute_loop_4fma(
        int ur_w, int pad_l, int pad_r) {}

template <>
void _jit_avx512_common_conv_fwd_kernel<Zmm>::compute_loop_4fma(
        int ur_w, int pad_l, int pad_r) {
    assert(!is_src_layout_nxc() && !is_dst_layout_nxc());
    int stride_w = jcp.stride_w;
    int ic_block = jcp.ic_block;
    int oc_block = jcp.oc_block;
    Label kh_label, last_iter_label, loop_end_label, kd_label;
    int ker_load_number = 4;
    int shift_kernel_ptr = typesize * jcp.kw * jcp.oc_block * jcp.ic_block;
    int shift_input_ptr = typesize * (jcp.dilate_h + 1) * jcp.iw * jcp.ic_block;

    bool check_last_kh = (jcp.kh > 3);
    bool pref_current_inp = (jcp.iw < 14 || jcp.iw > 28);

    int oi_ipref_t0 = get_ow_start(0, pad_l);
    int ow_end_ipref = get_ow_end(ur_w, 0, pad_r);

    assert(jcp.oc % jcp.nb_oc_blocking == 0);

    auto kernel_offset = [=](int ocb, int ic, int ki) {
        int blk_idx = ocb * jcp.nb_ic * jcp.kh * jcp.kw * jcp.kd + ki;
        int blk_offset = blk_idx * jcp.oc_block * jcp.ic_block;
        int ic_offset = ic * jcp.oc_block;
        return typesize * (blk_offset + ic_offset);
    };
    auto kernel_loads = [=](int ki, int ic, int kk) {
        for (int ii = 0; ii < ker_load_number; ii++) {
            int aux_kernel_offset = kernel_offset(kk, ic + ii, ki);
            vmovups(vmm_ker(ii),
                    EVEX_compress_addr(aux_reg_ker, aux_kernel_offset));
        }
    };
    auto prefetch_inp_next_kh = [&](int ki, int ki_start, int cnt0, int cnt1) {
        if (cnt1 >= ker_load_number && cnt0 >= ker_load_number && ki >= ki_start
                && oi_ipref_t0 < ow_end_ipref) {
            int aux_inp_offset = typesize
                    * ((oi_ipref_t0 * stride_w - pad_l) * ic_block
                            + (jcp.dilate_h + 1) * jcp.iw * ic_block);
            prefetcht0(EVEX_compress_addr(aux_reg_inp, aux_inp_offset));
            oi_ipref_t0++;
        }
    };

    if (one_of(jcp.ndims, 3, 4)) {
        mov(aux_reg_inp, reg_inp);
        mov(aux_reg_ker, reg_ker);
        mov(aux_reg_ker_prf, reg_ker_prf);
        mov(aux_reg_inp_prf, reg_inp_prf);
    }

    if (jcp.ndims == 5) {
        push(reg_out_prf);
        push(reg_out);

        mov(reg_ki, ptr[param1 + GET_OFF(kd_padding)]);
        mov(aux_reg_ker_d, ptr[param1 + GET_OFF(filt)]);
        mov(aux_reg_inp_d, reg_inp);
        mov(aux_reg_inp_d_prf, reg_inp_prf);
        mov(aux_reg_ker_d_prf, reg_ker_prf);
        L(kd_label);
        mov(reg_kj, ptr[param1 + GET_OFF(kh_padding)]);
    } else {
        mov(reg_kj, reg_kh);
    }
    if (jcp.ndims == 5) {
        mov(aux_reg_inp, aux_reg_inp_d);
        mov(aux_reg_ker, aux_reg_ker_d);
        mov(aux_reg_ker_prf, aux_reg_ker_d_prf);
        mov(aux_reg_inp_prf, aux_reg_inp_d_prf);
    }

    align(16);
    L(kh_label);
    int kw = jcp.kw;
    if (check_last_kh) {
        for (int ki = 0; ki < kw; ki++)
            for (int ic = 0; ic < ic_block; ic += 4)
                for (int kk = 0; kk < jcp.nb_oc_blocking; kk++) {
                    bool last_kernel_loads = (kk == jcp.nb_oc_blocking - 1
                            && ki == kw - 1 && (ic + 4) == ic_block);

                    if (last_kernel_loads) {
                        cmp(reg_kj, 1);
                        je(last_iter_label, T_NEAR);
                    }

                    kernel_loads(ki, ic, kk);
                    for (int oi = get_ow_start(ki, pad_l), prf_count_t1 = 0,
                             prf_count_t0 = 0;
                            oi < get_ow_end(ur_w, ki, pad_r); oi++) {
                        int aux_input_offset = typesize
                                * ((ki * (jcp.dilate_w + 1) + oi * stride_w
                                           - pad_l)
                                                * ic_block
                                        + ic);
                        v4fmaddps(vmm_out(oi, kk), vmm_ker(0),
                                EVEX_compress_addr(
                                        aux_reg_inp, aux_input_offset));

                        if (oi % 2) {
                            if (prf_count_t0 < 4) {
                                int aux_kernel_prf;
                                if (last_kernel_loads)
                                    aux_kernel_prf
                                            = kernel_offset(0,
                                                      prf_count_t0 + ic + 4
                                                              - ic_block,
                                                      0)
                                            + typesize * kw * oc_block
                                                    * ic_block;
                                else
                                    aux_kernel_prf = kernel_offset(
                                            kk, ic + 4 + prf_count_t0, ki);
                                mic_prefetcht0(EVEX_compress_addr(
                                        aux_reg_ker, aux_kernel_prf));
                                prf_count_t0++;
                            } else if (prf_count_t1 < 4) {
                                mic_prefetcht1(EVEX_compress_addr(
                                        aux_reg_ker_prf,
                                        kernel_offset(
                                                kk, ic + prf_count_t1, ki)));
                                prf_count_t1++;
                            }
                        } else
                            prefetch_inp_next_kh(
                                    ki, 2, prf_count_t0, prf_count_t1);
                    }

                    if (last_kernel_loads) {
                        jmp(loop_end_label, T_NEAR);

                        L(last_iter_label);

                        kernel_loads(ki, ic, kk);
                        for (int oi = get_ow_start(ki, pad_l), prf_count_t1 = 0,
                                 prf_count_t0 = 0;
                                oi < get_ow_end(ur_w, ki, pad_r); oi++) {
                            int aux_input_offset = typesize
                                    * ((ki * (jcp.dilate_w + 1) + oi * stride_w
                                               - pad_l)
                                                    * ic_block
                                            + ic);
                            v4fmaddps(vmm_out(oi, kk), vmm_ker(0),
                                    EVEX_compress_addr(
                                            aux_reg_inp, aux_input_offset));
                            if (oi % 2) {
                                if (prf_count_t0 < 4) {
                                    mic_prefetcht0(EVEX_compress_addr(
                                            aux_reg_ker_prf,
                                            kernel_offset(0, prf_count_t0, 0)));
                                    prf_count_t0++;
                                } else if (prf_count_t1 < 4) {
                                    mic_prefetcht1(EVEX_compress_addr(
                                            aux_reg_ker_prf,
                                            kernel_offset(kk, ic + prf_count_t1,
                                                    ki)));
                                    prf_count_t1++;
                                }
                            }
                        }
                        L(loop_end_label);
                    }
                }
    } else {
        for (int ki = 0; ki < kw; ki++)
            for (int ic = 0; ic < ic_block; ic += 4)
                for (int kk = 0; kk < jcp.nb_oc_blocking; kk++) {
                    kernel_loads(ki, ic, kk);
                    for (int oi = get_ow_start(ki, pad_l), prf_count_t1 = 0,
                             prf_count_t0 = 0;
                            oi < get_ow_end(ur_w, ki, pad_r); oi++) {
                        int aux_input_offset = typesize
                                * ((ki * (jcp.dilate_w + 1) + oi * stride_w
                                           - pad_l)
                                                * ic_block
                                        + ic);
                        v4fmaddps(vmm_out(oi, kk), vmm_ker(0),
                                EVEX_compress_addr(
                                        aux_reg_inp, aux_input_offset));

                        if (!is_owb_prefetching(jcp)) {
                            if ((oi % 2) && (prf_count_t1 < 4)) {
                                mic_prefetcht1(EVEX_compress_addr(
                                        aux_reg_ker_prf,
                                        kernel_offset(
                                                kk, ic + prf_count_t1, ki)));
                                prf_count_t1++;
                            }
                        } else {
                            if (!(ki == 0 && ic == 0)
                                    && !(ki == kw - 1 && ic == 0) && (oi % 2)
                                    && (prf_count_t1 < 4)) {
                                mic_prefetcht0(EVEX_compress_addr(aux_reg_ker,
                                        kernel_offset(kk, ic + 4 + prf_count_t0,
                                                ki)));
                                prf_count_t0++;
                            }
                        }
                        if (!is_owb_prefetching(jcp)) {
                            if (pref_current_inp) {
                                if (ki == 0 && ic == 0 && kk == 0)
                                    mic_prefetcht0(
                                            EVEX_compress_addr(aux_reg_inp,
                                                    aux_input_offset
                                                            + shift_input_ptr));
                            } else {
                                if (ki == 1 && ic == 0 && kk == 0)
                                    mic_prefetcht1(EVEX_compress_addr(
                                            aux_reg_inp_prf, aux_input_offset));
                            }
                        } else {
                            int inp_mult = jcp.is_1stconv ? 1 : jcp.ic_block;
                            int inp_shift = jcp.typesize_in * ur_w * stride_w
                                    * inp_mult;
                            bool kk_pref_slot = kk ? oi % 2 : !(oi % 2);
                            if (ki == 0 && ic == 0 && kk_pref_slot)
                                mic_prefetcht1(EVEX_compress_addr(aux_reg_inp,
                                        aux_input_offset + inp_shift));

                            if (ki == kw - 1 && ic == 0 && kk_pref_slot)
                                mic_prefetcht0(EVEX_compress_addr(aux_reg_inp,
                                        aux_input_offset + inp_shift));
                        }
                    }
                }
    }

    add(aux_reg_ker, shift_kernel_ptr);
    add(aux_reg_inp, shift_input_ptr);
    add(aux_reg_ker_prf, shift_kernel_ptr);
    add(aux_reg_inp_prf, shift_input_ptr);

    dec(reg_kj);
    cmp(reg_kj, 0);
    jg(kh_label, T_NEAR);

    if (jcp.ndims == 5) {
        add(aux_reg_inp_d,
                typesize * (jcp.dilate_d + 1) * jcp.ih * jcp.iw * jcp.ic_block);
        add(aux_reg_ker_d,
                typesize * jcp.kw * jcp.kh * jcp.oc_block * jcp.ic_block);
        add(aux_reg_inp_d_prf,
                typesize * (jcp.dilate_d + 1) * jcp.ih * jcp.iw * jcp.ic_block);
        add(aux_reg_ker_d_prf,
                typesize * jcp.kw * jcp.kh * jcp.oc_block * jcp.ic_block);

        dec(reg_ki);
        cmp(reg_ki, 0);
        jg(kd_label, T_NEAR);

        pop(reg_out);
        pop(reg_out_prf);
    }
}

template <typename Vmm>
void _jit_aarch64_sve_512_common_conv_fwd_kernel<Vmm>::compute_loop_fma(
        int ur_w, int pad_l, int pad_r) {
    const bool prf_ker = mayiuse(avx512_mic);
    const bool prf_inp = mayiuse(avx512_mic);
    const bool prf_out = mayiuse(avx512_mic);
    const bool is_source_layout_nxc = is_src_layout_nxc();
    const bool icb_loop_in_compute_function = is_source_layout_nxc;
    const int ic_tail = jcp.ic_tail;
    const int oc_tail = jcp.oc_tail;
    // reg_channel in icb loop is the same as aux_reg_inp_prf
    assert(IMPLICATION(prf_inp, !icb_loop_in_compute_function));
    int ih = jcp.ih;
    int stride_w = jcp.stride_w;
    int id = jcp.id;
    int iw = jcp.iw;
    int kw = jcp.kw;
    int ic_block = jcp.ic_block;
    int oc_block = jcp.oc_block;
    int nb_oc_block = jcp.nb_oc_blocking;
    Label kh_label, kd_label;
    std::vector<Label> ic_tail_jmp(kw);

    // It seems that this compute_loop currently only handles one block of oc.
    // assert if it is extended in future to catch unpadded_oc_tail.
    assert(IMPLICATION(oc_tail, nb_oc_block == 1));

    int num_ker_loads = ic_block * nb_oc_block * kw;
    int ker_pipeline_depth
            = oc_tail || ic_tail ? 1 : nstl::min(4, num_ker_loads);
    assert(ker_reg_base_idx + ker_pipeline_depth <= 32);
    assert(oc_block >= ker_pipeline_depth);

    int num_ker_prfs = prf_ker ? num_ker_loads : 0;
    int num_inp_prfs = prf_inp
            ? ur_w * nstl::min(kw, stride_w) + nstl::max(0, kw - stride_w)
            : 0;
    if (jcp.is_1stconv && prf_inp) {
        num_inp_prfs = div_up(num_inp_prfs, jcp.simd_w) * ic_block;
    }
    int num_prfs = num_ker_prfs + num_inp_prfs;
    int num_fmas = num_ker_loads * ur_w;
    int prf_inst_spacing
            = (prf_ker || prf_inp) ? nstl::max(1, num_fmas / num_prfs) : 1;
    int prf_inst_trigger = (num_fmas % prf_inst_spacing) / 2;
    int inp_mul = is_source_layout_nxc ? jcp.ngroups * jcp.ic
                                       : (!jcp.is_1stconv ? ic_block : 1);

    if (one_of(jcp.ndims, 3, 4)) {
        mov(aux_reg_inp, reg_inp);
        mov(aux_reg_ker, reg_ker);
        if (prf_inp) mov(aux_reg_inp_prf, reg_inp_prf);
        if (prf_ker) mov(aux_reg_ker_prf, reg_ker_prf);
    }

    size_t max_input_offset = (size_t)jcp.typesize_in * ic_block * iw * ih * id;
    assert(IMPLICATION(prf_inp, reg_inp_prf == reg_long_offt));
    if (max_input_offset > INT_MAX && prf_inp) push(reg_inp_prf);

    if (jcp.ndims == 5) {
        if (prf_out) push(reg_out_prf);
        push(reg_out);

        mov(reg_ki, ptr[param1 + GET_OFF(kd_padding)]);
        if (icb_loop_in_compute_function) {
            // need to continue with the same kernel pointer, but as
            // aux_reg_ker_d == reg_ker we need to save its value and restore
            // it after kd loop
            assert(aux_reg_ker_d == reg_ker);
            push(aux_reg_ker_d);
        } else
            mov(aux_reg_ker_d, ptr[param1 + GET_OFF(filt)]);
        mov(aux_reg_inp_d, reg_inp);
        if (prf_inp) mov(aux_reg_inp_d_prf, reg_inp_prf);
        if (prf_ker) mov(aux_reg_ker_d_prf, reg_ker_prf);

        L(kd_label);
        mov(reg_kj, ptr[param1 + GET_OFF(kh_padding)]);
    } else {
        mov(reg_kj, reg_kh);
    }

    if (jcp.ndims == 5) {
        mov(aux_reg_inp, aux_reg_inp_d);
        mov(aux_reg_ker, aux_reg_ker_d);
        if (prf_ker) mov(aux_reg_ker_prf, aux_reg_ker_d_prf);
        if (prf_inp) mov(aux_reg_inp_prf, aux_reg_inp_d_prf);
    }

    align(16);
    L(kh_label);
    {
        int step = 0;
        int ker_prfs = 0;
        for (int ki = 0; ki < kw; ki++) {
            for (int ic = 0; ic < ic_block; ic++) {
                if (ic_tail && ic >= ic_tail) {
                    // if src has only tails to compute, skip early
                    if (jcp.ic == ic_tail)
                        break;
                    else if (ic == ic_tail) {
                        cmp(reg_channel, ic_tail);
                        je(ic_tail_jmp[ki], T_NEAR);
                    }
                }
                int aux_kernel_offset = 0;
                if (step == 0) {
                    for (int i = 0; i < ker_pipeline_depth; i++) {
                        aux_kernel_offset = get_kernel_offset(ki, ic, 0, i);
                        vmovups(vmm_ker(i),
                                EVEX_compress_addr(
                                        aux_reg_ker, aux_kernel_offset));
                    }
                } else if (step < num_ker_loads - ker_pipeline_depth + 1) {
                    int load_offset = ker_pipeline_depth - 1;
                    int ker_load_reg_idx
                            = (step + load_offset) % ker_pipeline_depth;
                    aux_kernel_offset
                            = get_kernel_offset(ki, ic, 0, load_offset);
                    vmovups(vmm_ker(ker_load_reg_idx),
                            EVEX_compress_addr(aux_reg_ker, aux_kernel_offset));
                }

                bool ker_prf_inserted = false;
                Vmm vmm_kernel = vmm_ker(step % ker_pipeline_depth);
                int j_start = get_ow_start(ki, pad_l);
                int j_end = get_ow_end(ur_w, ki, pad_r);
                for (int j = j_start; j < j_end; j++) {
                    size_t aux_input_offset
                            = get_input_offset(ki, ic, j, pad_l);
                    auto addr = EVEX_compress_addr_safe(
                            aux_reg_inp, aux_input_offset, reg_long_offt, true);
                    vfmadd231ps(vmm_out(j, 0), vmm_kernel, addr);
                    int fma_idx = step * ur_w + j;
                    int prf_slot_idx = fma_idx / prf_inst_spacing;
                    if (fma_idx % prf_inst_spacing == prf_inst_trigger) {
                        if (prf_ker && !ker_prf_inserted
                                && ker_prfs < num_ker_prfs) {
                            int ker_prf_offset
                                    = jcp.typesize_in * ker_prfs * jcp.oc_block;
                            mic_prefetcht2(EVEX_compress_addr(
                                    aux_reg_ker_prf, ker_prf_offset));
                            ker_prf_inserted = true;
                            ker_prfs++;
                        } else if (prf_inp) {
                            int inp_prf_idx = prf_slot_idx - ker_prfs;
                            if (inp_prf_idx < num_inp_prfs) {
                                size_t inp_prf_stride = nstl::max(kw, stride_w);
                                size_t inp_prf_offset;
                                if (!jcp.is_1stconv) {
                                    inp_prf_offset = ic_block * jcp.typesize_in
                                            * ((inp_prf_idx / kw)
                                                            * inp_prf_stride
                                                    + (inp_prf_idx % kw));
                                } else {
                                    size_t ic_prf_stride
                                            = (size_t)jcp.typesize_in * iw * ih
                                            * id;
                                    size_t iw_prf_stride
                                            = jcp.typesize_in * jcp.simd_w;
                                    inp_prf_offset = ((inp_prf_idx / ic_block)
                                                    * iw_prf_stride
                                            + (inp_prf_idx % ic_block)
                                                    * ic_prf_stride);
                                }
                                mic_prefetcht0(
                                        EVEX_compress_addr_safe(aux_reg_inp_prf,
                                                inp_prf_offset, reg_long_offt));
                            }
                        }
                    }
                }
                step++;
            }
            L(ic_tail_jmp[ki]);
        }
        int ker_shift = jcp.typesize_in * kw * oc_block * ic_block;
        add(aux_reg_ker, ker_shift);
        if (prf_ker) add(aux_reg_ker_prf, ker_shift);
        int inp_shift = jcp.typesize_in * (jcp.dilate_h + 1) * iw * inp_mul;
        add(aux_reg_inp, inp_shift);
        if (prf_inp) add(aux_reg_inp_prf, inp_shift);
        dec(reg_kj);
        cmp(reg_kj, 0);
        jg(kh_label, T_NEAR);
    }

    if (jcp.ndims == 5) {
        int inp_shift
                = typesize * (jcp.dilate_d + 1) * jcp.ih * jcp.iw * inp_mul;
        add(aux_reg_inp_d, inp_shift);
        int ker_shift
                = typesize * jcp.kw * jcp.kh * jcp.oc_block * jcp.ic_block;
        add(aux_reg_ker_d, ker_shift);
        if (prf_inp) add(aux_reg_inp_d_prf, inp_shift);
        if (prf_ker) add(aux_reg_ker_d_prf, ker_shift);

        dec(reg_ki);
        cmp(reg_ki, 0);
        jg(kd_label, T_NEAR);

        if (icb_loop_in_compute_function) pop(aux_reg_ker_d);
        pop(reg_out);
        if (prf_out) pop(reg_out_prf);
    }
    if (max_input_offset > INT_MAX && prf_inp) pop(reg_inp_prf);
}
#endif

template <typename Vmm>
void _jit_aarch64_sve_512_conv_fwd_kernel<Vmm>::compute_loop_fma_core(
        int ur_w, int pad_l, int pad_r) {
    int kw = jcp.kw;
    int ic_block = jcp.ic_block;
    int oc_block = jcp.oc_block;
    int nb_oc_block = jcp.nb_oc_blocking;
    const bool is_source_layout_nxc = is_src_layout_nxc();
    const bool icb_loop_in_compute_function = is_source_layout_nxc;
    const int ic_tail = jcp.ic_tail;

    xa::LabelAArch64 kh_label, kd_label;

#if 0
    std::vector<Label> ic_tail_jmp(kw);
#else
    std::vector<xa::LabelAArch64> ic_tail_jmp(kw);
#endif
    int shift_kernel_ptr
            = jcp.typesize_in * jcp.kw * jcp.oc_block * jcp.ic_block;
    int inp_mul = is_source_layout_nxc ? jcp.ngroups * jcp.ic
                                       : (!jcp.is_1stconv ? ic_block : 1);

    int shift_input_ptr
            = jcp.typesize_in * (jcp.dilate_h + 1) * jcp.iw * inp_mul;

    if (one_of(jcp.ndims, 3, 4)) {
        CGA64::mov(aux_reg_inp, reg_inp);
        CGA64::mov(aux_reg_ker, reg_ker);
    }

    if (jcp.ndims == 5) {
        CGA64::mov(reg_out_org, reg_out);
        CGA64::ldr(reg_ki, xa::ptr(abi_param1_aarch64, GET_OFF(kd_padding)));
        if (icb_loop_in_compute_function) {
            // need to continue with the same kernel pointer, but as
            // aux_reg_ker_d == reg_ker we need to save its value and restore
            // it after kd loop
#if 0
//[info]取り敢えずコメント化。
//レジスタの比較で、assertは使えない?
            assert(aux_reg_ker_d == reg_ker);
#endif
            CGA64::mov(aux_reg_ker_d_org, aux_reg_ker_d);
        } else
            CGA64::ldr(aux_reg_ker_d, xa::ptr(abi_param1_aarch64, GET_OFF(filt)));

        CGA64::mov(aux_reg_inp_d, reg_inp);

        CGA64::L_aarch64(kd_label);
        CGA64::ldr(reg_kj, xa::ptr(abi_param1_aarch64, GET_OFF(kh_padding)));
    } else {
        CGA64::mov(reg_kj, reg_kh);
    }

    if (jcp.ndims == 5) {
        CGA64::mov(aux_reg_inp, aux_reg_inp_d);
        CGA64::mov(aux_reg_ker, aux_reg_ker_d);
    }

#if 1
//[info]v0.21 codeをそのまま展開
    auto zreg_inp_s = [=](int i_ic, int nb_x_blocking){
        int idx = i_ic + nb_x_blocking * jcp.ur_w;
        assert(idx < 31);
        return xa::ZRegS(idx);
    };
    auto zreg_out_s = [=](int i_ur, int i_oc){
        int idx = i_ur + i_oc * jcp.ur_w;
        assert(idx < ker_reg_base_idx);
        return xa::ZRegS(idx);
    };
    auto zreg_wei = [=](int idx){
        assert(idx < 32);
        return xa::ZReg(idx);
    };
    auto zreg_wei_s = [=](int idx){
        assert(idx < 32);
        return xa::ZRegS(idx);
    };

    auto bcast_load = [&](int jj, int nb_oc_block, int aux_input_offset, int prev_ofs, int jj_end){
        if( ((aux_input_offset & 0x3) ==0) &&
                (aux_input_offset < LDRWMAX) &&
                (aux_input_offset >= 0)){
            CGA64::ld1rw(zreg_inp_s(jj, nb_oc_block), reg_p_all_ones,
                    xa::ptr(aux_reg_inp, static_cast<int32_t>(aux_input_offset)));
        }else{
            if( (prev_ofs != -1) &&
                ((aux_input_offset - prev_ofs)>0) &&
                ((aux_input_offset - prev_ofs) < LDRWMAX) &&
                (((aux_input_offset - prev_ofs)& 0x3) ==0)){

                CGA64::ld1rw(zreg_inp_s(jj, nb_oc_block), reg_p_all_ones,
                        xa::ptr(reg_prev_bcast_addr, static_cast<int32_t>(aux_input_offset - prev_ofs)));
            }else{
                int ofs;
                if((prev_ofs != -1) && ((aux_input_offset - prev_ofs)>0)){
                    ofs = aux_input_offset - prev_ofs;
                    CGA64::add_imm(reg_prev_bcast_addr, reg_prev_bcast_addr, ofs, reg_tmp_imm);
                }else{
                    ofs = aux_input_offset;
                    CGA64::add_imm(reg_prev_bcast_addr, aux_reg_inp, ofs, reg_tmp_imm);
                }

                CGA64::ld1rw(zreg_inp_s(jj, nb_oc_block), reg_p_all_ones, xa::ptr(reg_prev_bcast_addr));
                prev_ofs = aux_input_offset;
            }
        }

        return prev_ofs;

    };

    auto wei_load = [=](int aux_kernel_offset, int reg_idx, int prev_ofs){
        int ofs = aux_kernel_offset;

        if( (VL_OFS(ofs) <= LDRMAX) && (VL_OFS(ofs) >= (-1 * LDRMAX))){
            CGA64::ldr(zreg_wei(reg_idx),
                       xa::ptr(aux_reg_ker, static_cast<int32_t>(VL_OFS(ofs))));
        }else{
            int ofs_tmp = ofs - prev_ofs;
            if( (prev_ofs != -1) && (VL_OFS(ofs_tmp)>=(-1 * LDRMAX)) && (VL_OFS(ofs_tmp) <= LDRMAX) ){
                CGA64::ldr(zreg_wei(reg_idx),
                           xa::ptr(reg_prev_wei_addr, static_cast<int32_t>(VL_OFS(ofs_tmp))));
            }else{
                if((prev_ofs != -1) && (ofs_tmp>0)){
                    ofs_tmp = aux_kernel_offset - prev_ofs;
                    CGA64::add_imm(reg_prev_wei_addr, reg_prev_wei_addr, ofs_tmp, reg_tmp_imm);
                }else{
                    CGA64::add_imm(reg_prev_wei_addr, aux_reg_ker, ofs, reg_tmp_imm);
                }

                CGA64::ldr(zreg_wei(reg_idx), xa::ptr(reg_prev_wei_addr));
                prev_ofs = ofs;
            }
        }
        return prev_ofs;

    };
#endif

#if 0
    L(kh_label);
    {
        for (int ki = 0; ki < kw; ki++) {
            int jj_start = get_ow_start(ki, pad_l);
            int jj_end = get_ow_end(ur_w, ki, pad_r);
            for (int ic = 0; ic < ic_block; ic++) {
                if (ic_tail && ic >= ic_tail) {
                    // if src has only tails to compute, skip early
                    if (jcp.ic == ic_tail)
                        break;
                    else if (ic == ic_tail) {
                        cmp(reg_channel, ic_tail);
                        je(ic_tail_jmp[ki], T_NEAR);
                    }
                }
                if (jcp.kernel_kind == expl_bcast) {
                    for (int jj = jj_start; jj < jj_end; jj++) {
                        size_t aux_input_offset
                                = get_input_offset(ki, ic, jj, pad_l);
                        vbroadcastss(vmm_inp(jj, nb_oc_block),
                                EVEX_compress_addr_safe(aux_reg_inp,
                                        aux_input_offset, reg_long_offt));
                    }
                }
                for (int ii = 0; ii < nb_oc_block; ii++) {
                    int aux_kernel_offset = jcp.typesize_in
                            * (ii * jcp.nb_ic * jcp.kh * jcp.kw * jcp.kd
                                            * ic_block * oc_block
                                    + ki * ic_block * oc_block + ic * oc_block);
                    if (jj_end - jj_start > 0)
                        vmovups(vmm_wei,
                                EVEX_compress_addr(
                                        aux_reg_ker, aux_kernel_offset));
                    for (int jj = jj_start; jj < jj_end; jj++)
                        if (jcp.kernel_kind == expl_bcast)
                            vfmadd231ps(vmm_out(jj, ii),
                                    vmm_inp(jj, nb_oc_block), vmm_wei);
                        else {
                            size_t aux_input_offset
                                    = get_input_offset(ki, ic, jj, pad_l);
                            vfmadd231ps(vmm_out(jj, ii), vmm_wei,
                                    EVEX_compress_addr_safe(aux_reg_inp,
                                            aux_input_offset, reg_long_offt,
                                            true));
                        }
                }
            }
            L(ic_tail_jmp[ki]);
        }
#else
//[info]v0.21を参考に修正。要見直し。
    align(32);
    CGA64::L_aarch64(kh_label);
    {
        int prev_bcast_ofs = -1;
        int prev_wei_ofs = -1;
        for (int ki = 0; ki < kw; ki++) {

            int jj_start = get_ow_start(ki, pad_l);
            int jj_end = get_ow_end(ur_w, ki, pad_r);

            int wei_reg_ofs = nb_oc_block * jcp.ur_w + jj_end;
            int num_regs4wei = 32 - wei_reg_ofs;
            for (int ic = 0; ic < ic_block; ic++) {
                if (ic_tail && ic >= ic_tail) {
                    // if src has only tails to compute, skip early
                    if (jcp.ic == ic_tail) {
                        break;
                    }
                    else if (ic == ic_tail) {
                        CGA64::cmp(reg_channel, ic_tail);
                        CGA64::b(xa::EQ, ic_tail_jmp[ki]);
                    }
                }
                if (jcp.kernel_kind == expl_bcast) {
                    for (int jj = jj_start; jj < jj_end; jj++) {
                        size_t aux_input_offset = get_input_offset(ki, ic, jj, pad_l);
                        prev_bcast_ofs = bcast_load(jj, nb_oc_block, aux_input_offset, prev_bcast_ofs, jj_end);
                    }
                }
                int wei_count = 0;
                for (int ii = 0; ii < nb_oc_block; ii++) {
                    int reg_idx = wei_reg_ofs + ii;
                    if(reg_idx >= 32) break;
                    int aux_kernel_offset = jcp.typesize_in
                        * (ii * jcp.nb_ic * jcp.kh * jcp.kw * jcp.kd * ic_block
                        * oc_block + ki * ic_block * oc_block + ic * oc_block);

                    wei_count ++;
                    if (jj_end - jj_start > 0){
                        prev_wei_ofs =
                            wei_load(aux_kernel_offset,
                                     wei_reg_ofs +(ii%num_regs4wei),
                                     prev_wei_ofs);
                    }
                }

                for (int ii = 0; ii < nb_oc_block; ii++) {
                    int aux_kernel_offset = jcp.typesize_in
                        * ((ii+wei_count) * jcp.nb_ic * jcp.kh * jcp.kw * jcp.kd * ic_block
                        * oc_block + ki * ic_block * oc_block + ic * oc_block);

                    for (int jj = jj_start; jj < jj_end; jj++)
                        if (jcp.kernel_kind == expl_bcast){
                            CGA64::fmla(zreg_out_s(jj, ii), reg_p_all_ones,
                                        zreg_inp_s(jj, nb_oc_block),
                                        zreg_wei_s(wei_reg_ofs +(ii%num_regs4wei)));
                        }
                        else
                        {
                            assert(NULL);
                        }

                    if ((jj_end - jj_start > 0) && ((wei_count + ii) < nb_oc_block)){
                        prev_wei_ofs =
                            wei_load(aux_kernel_offset, 
                                     wei_reg_ofs +((ii+wei_count)%num_regs4wei),
                                     prev_wei_ofs);
                    }
                }
            }
            CGA64:L_aarch64(ic_tail_jmp[ki]);
        }
#endif
        CGA64::add_imm(aux_reg_ker, aux_reg_ker, shift_kernel_ptr, reg_tmp_imm);
        CGA64::add_imm(aux_reg_inp, aux_reg_inp, shift_input_ptr, reg_tmp_imm);
        CGA64::sub(reg_kj, reg_kj, 1); //dec(reg_kj);
        CGA64::cmp(reg_kj, 0);
        CGA64::b(xa::GT, kh_label);
    }

    if (jcp.ndims == 5) {
        CGA64::add_imm(aux_reg_inp_d, aux_reg_inp_d,
                typesize * (jcp.dilate_d + 1) * jcp.ih * jcp.iw * inp_mul, reg_tmp_imm);
        const int ker_shift
                = typesize * jcp.kw * jcp.kh * jcp.oc_block * jcp.ic_block;
        CGA64::add_imm(aux_reg_ker_d, aux_reg_ker_d, ker_shift, reg_tmp_imm);

        CGA64::sub(reg_ki, reg_ki, 1); //dec(reg_ki);
        CGA64::cmp(reg_ki, 0);
        CGA64::b(xa::GT, kd_label);

        if (icb_loop_in_compute_function) CGA64::mov(aux_reg_ker_d, aux_reg_ker_d_org);
        CGA64::mov(reg_out, reg_out_org);
    }
}

template <typename Vmm>
void _jit_aarch64_sve_512_conv_fwd_kernel<Vmm>::compute_loop(
        int ur_w, int pad_l, int pad_r) {

    if (jcp.ndims == 5) CGA64::mov(reg_oi_org, reg_oi);

    prepare_output(ur_w);

    xa::LabelAArch64 skip_compute_loop;
    if (jcp.ndims == 5) {
        if ((jcp.dilate_d >= jcp.id)
                || (jcp.kd - 1) * (jcp.dilate_d + 1)
                        < nstl::max(jcp.f_pad, jcp.back_pad)) {
            CGA64::ldr(reg_kj, xa::ptr(abi_param1_aarch64, GET_OFF(kd_padding)));
            CGA64::cmp(reg_kj, 0);
            CGA64::b(xa::LE, skip_compute_loop);
        }
    }
    if ((jcp.dilate_h >= jcp.ih)
            || (jcp.kh - 1) * (jcp.dilate_h + 1)
                    < nstl::max(jcp.t_pad, jcp.b_pad)) {
        CGA64::ldr(reg_kj, xa::ptr(abi_param1_aarch64, GET_OFF(kh_padding)));
        CGA64::cmp(reg_kj, 0);
        CGA64::b(xa::LE, skip_compute_loop);
    }

    xa::LabelAArch64 ic_loop;
    const bool generate_icb_loop = jcp.nb_ic > 1 && is_src_layout_nxc();
    if (generate_icb_loop) {
        CGA64::mov(reg_inp_org, reg_inp);
        CGA64::mov(reg_ker_org, reg_ker);

        CGA64::ldr(reg_channel, xa::ptr(param, GET_OFF(reduce_work)));
        CGA64::L_aarch64(ic_loop);
    }

#if 0
    if (jcp.ver == ver_4fma)
        if (jcp.is_1stconv)
            compute_loop_4fma_1st(ur_w, pad_l, pad_r);
        else
            compute_loop_4fma(ur_w, pad_l, pad_r);
    else if (jcp.ver == ver_fma)
        if ((jcp.is_1stconv && jcp.kernel_kind != expl_bcast)
                || mayiuse(avx512_mic))
            compute_loop_fma(ur_w, pad_l, pad_r);
        else if (jcp.kernel_kind == embd_bcast && jcp.nb_oc_blocking == 1)
            compute_loop_fma(ur_w, pad_l, pad_r);
        else
            compute_loop_fma_core(ur_w, pad_l, pad_r);
    else
        assert(!"unknown convolution version");
#else
    if (jcp.ver == ver_fma)
        if (jcp.is_1stconv && jcp.kernel_kind != expl_bcast)
                assert(!"STOP:jcp.is_1stconv && jcp.kernel_kind != expl_bcast");
        else if (jcp.kernel_kind == embd_bcast && jcp.nb_oc_blocking == 1)
                assert(!"STOP:jcp.kernel_kind == embd_bcast && jcp.nb_oc_blocking == 1");
        else
        {
            compute_loop_fma_core(ur_w, pad_l, pad_r);
        }
    else
        assert(!"unknown convolution version");
#endif

    if (generate_icb_loop) {
        assert(is_src_layout_nxc());
        const int inp_shift = jcp.ic_block * jcp.typesize_in;
        CGA64:add_imm(reg_inp, reg_inp, inp_shift, reg_tmp_imm);
        const int ker_shift = jcp.kd * jcp.kh * jcp.kw * jcp.ic_block
                * jcp.oc_block * jcp.typesize_in;
        CGA64::add_imm(reg_ker, reg_ker, ker_shift, reg_tmp_imm);
        CGA64::sub_imm(reg_channel, reg_channel, jcp.ic_block, reg_tmp_imm);
        CGA64::b(xa::GT, ic_loop);
        CGA64::mov(reg_ker, reg_ker_org);
        CGA64::mov(reg_inp, reg_inp_org);
    }

    CGA64::L_aarch64(skip_compute_loop);
    store_output(ur_w);
    if (jcp.ndims == 5) CGA64::mov(reg_oi, reg_oi_org);
}

template <typename Vmm>
void _jit_aarch64_sve_512_conv_fwd_kernel<Vmm>::generate() {
    int iw = jcp.iw;
    int ow = jcp.ow;
    int ow_block = jcp.ow_block;
    int nb_ow = jcp.nb_ow;
    int kw = jcp.kw;
    int l_pad = jcp.l_pad;
    int ur_w = jcp.ur_w;
    int ur_w_tail = jcp.ur_w_tail;
    int stride_w = jcp.stride_w;

    int inp_mult = is_src_layout_nxc() ? jcp.ngroups * jcp.ic
                                       : (jcp.is_1stconv ? 1 : jcp.ic_block);
    int inp_shift_pad = jcp.typesize_in * (ur_w * stride_w - l_pad) * inp_mult;
    int inp_shift = jcp.typesize_in * ur_w * stride_w * inp_mult;
    int inp_shift_pad_second_block = -1 * jcp.typesize_in * l_pad * inp_mult;
    int out_shift = jcp.typesize_out * ur_w
            * (is_dst_layout_nxc() ? jcp.ngroups * jcp.oc : jcp.oc_block);

    preamble();
    CGA64::ldr(reg_inp,     xa::ptr(abi_param1_aarch64, GET_OFF(src)));
    CGA64::ldr(reg_out,     xa::ptr(abi_param1_aarch64, GET_OFF(dst)));
    CGA64::ldr(reg_ker,     xa::ptr(abi_param1_aarch64, GET_OFF(filt)));
    CGA64::ldr(reg_ker_prf, xa::ptr(abi_param1_aarch64, GET_OFF(filt_prf)));
    CGA64::ldr(reg_kh,      xa::ptr(abi_param1_aarch64, GET_OFF(kh_padding)));

    const int oc_tail = jcp.oc_tail;
//[info]取り敢えずコメント化。
#if 0
    if (oc_tail) {
        Label done;
        // dummy mask all 1's
        kxnorw(k_oc_tail_mask, k_oc_tail_mask, k_oc_tail_mask);
        mov(reg_load_work, ptr[param1 + GET_OFF(load_work)]);
        cmp(reg_load_work, jcp.nb_oc_blocking * jcp.oc_block);
        je(done, T_NEAR);
        Reg32 reg_tail_32 = reg_tail.cvt32();
        mov(reg_tail_32, (1 << oc_tail) - 1);
        kmovw(k_oc_tail_mask, reg_tail_32);
        L(done);
    }
#endif

    int r_pad = nstl::max(0, jcp.r_pad);
    int n_oi = ow / ur_w;
    int r_pad1 = calculate_end_padding(l_pad, ur_w * n_oi, iw, stride_w,
            calculate_extended_filter_size(kw, jcp.dilate_w));

    CGA64::ptrue( reg_p_all_ones.b );

    if (!is_ow_threading_on(jcp)) { // nb_ow <= 1
        // nb_ow is # of output width blocks ??

        // ow is being processed as a whole - with left and right paddings
        // n_oi is # of output width blocks ??
        if (r_pad1 > 0) n_oi--;

        if (ow == ur_w) {
            CGA64::ldr(reg_inp_prf, xa::ptr(abi_param1_aarch64, GET_OFF(src_prf)));
            CGA64::ldr(reg_out_prf, xa::ptr(abi_param1_aarch64, GET_OFF(dst_prf)));
            compute_loop(ur_w, l_pad, r_pad);
        } else {
            CGA64::mov(reg_inp_prf, reg_inp);
            CGA64::mov(reg_out_prf, reg_out);
            if (n_oi == 0) {
                CGA64::add_imm(reg_inp_prf, reg_inp_prf,inp_shift_pad, reg_tmp_imm);
                CGA64::add_imm(reg_out_prf, reg_out_prf, out_shift, reg_tmp_imm);
                compute_loop(ur_w, l_pad, r_pad1);
                CGA64::add_imm(reg_inp, reg_inp, inp_shift_pad, reg_tmp_imm);
                CGA64::add_imm(reg_out, reg_out, out_shift, reg_tmp_imm);
                if (ur_w_tail != 0) {
                    CGA64::add_imm(reg_inp_prf, reg_inp_prf, inp_shift, reg_tmp_imm);
                    CGA64::add_imm(reg_out_prf, reg_out_prf, out_shift, reg_tmp_imm);
                    compute_loop(ur_w_tail, 0, r_pad);
                }
            } else {
                CGA64::mov(reg_oi, 0);
                if (l_pad > 0) {
                    CGA64::add_imm(reg_inp_prf, reg_inp_prf, inp_shift_pad, reg_tmp_imm);
                    CGA64::add_imm(reg_out_prf, reg_out_prf, out_shift, reg_tmp_imm);
                    compute_loop(ur_w, l_pad, 0);
                    CGA64::add_imm(reg_inp, reg_inp,inp_shift_pad, reg_tmp_imm);
                    CGA64::add_imm(reg_out, reg_out,out_shift, reg_tmp_imm);
                    CGA64::add_imm(reg_oi, reg_oi, 1, reg_tmp_imm); // increment
                }
                if ((l_pad <= 0 && n_oi > 0) || (l_pad > 0 && n_oi > 1)) {
                    xa::LabelAArch64 ow_loop_label;
                    CGA64::L_aarch64(ow_loop_label);
                    {
                        CGA64::add_imm(reg_inp_prf, reg_inp_prf, inp_shift, reg_tmp_imm);
                        CGA64::add_imm(reg_out_prf, reg_out_prf, out_shift, reg_tmp_imm);
                        compute_loop(ur_w, 0, 0);
                        CGA64::add_imm(reg_inp, reg_inp, inp_shift, reg_tmp_imm);
                        CGA64::add_imm(reg_out, reg_out, out_shift, reg_tmp_imm);
                        CGA64::add_imm(reg_oi, reg_oi, 1, reg_tmp_imm); //inc(reg_oi);
                        CGA64::cmp(reg_oi, n_oi);
                        CGA64::b(xa::LT, ow_loop_label);
                    }
                }
                if (r_pad1 > 0) {
                    CGA64::add_imm(reg_inp_prf, reg_inp_prf, inp_shift, reg_tmp_imm);
                    CGA64::add_imm(reg_out_prf, reg_out_prf, out_shift, reg_tmp_imm);
                    compute_loop(ur_w, 0, r_pad1);
                    CGA64::add_imm(reg_inp, reg_inp, inp_shift, reg_tmp_imm);
                    CGA64::add_imm(reg_out, reg_out, out_shift, reg_tmp_imm);
                }
                if (ur_w_tail != 0) {
                    CGA64::add_imm(reg_inp_prf, reg_inp_prf, inp_shift, reg_tmp_imm);
                    CGA64::add_imm(reg_out_prf, reg_out_prf, out_shift, reg_tmp_imm);
                    compute_loop(ur_w_tail, 0, r_pad);
                }
            }
        }
    } else {
        // ow block is only processed.
        // Number of block is passed as parameter owb,
        // and padding processing depends on this number.

        xa::LabelAArch64 end_label, last_oi_label, middle_ow_blocks_label, tail_label;
        xa::LabelAArch64 oi_loop_label, oi_loop_start_label, oi_loop_end_label;

        assert(ow_block % ur_w == 0);
        int n_oi_not_last_ow_block = ow_block / ur_w;
        // to simplify code (and general regs usage),
        // size of ow block must be >= 2 * ur_w
        assert(n_oi_not_last_ow_block > 1);
        int n_oi_next_last_ow_block = n_oi_not_last_ow_block;
        int n_oi_first_ow_block = n_oi_not_last_ow_block;

        int n_oi_last_ow_block = (ow - ow_block * (nb_ow-1)) / ur_w;

        // prepare right padding
        bool next_last_ow_block_padded = r_pad1 > 0 && n_oi_last_ow_block == 0;
        bool first_ow_block_padded = next_last_ow_block_padded && jcp.nb_ow == 2;
        bool last_ow_block_padded = r_pad1 > 0 && n_oi_last_ow_block > 0;

        if (last_ow_block_padded) n_oi_last_ow_block--;
        else if (first_ow_block_padded) n_oi_first_ow_block--;
        else if (next_last_ow_block_padded) n_oi_next_last_ow_block--;

        CGA64::ldr(reg_owb, xa::ptr(abi_param1_aarch64, GET_OFF(owb)));
        CGA64::cmp(reg_owb, 0); // is that the first ow-block ?
        CGA64::b(xa::GT, middle_ow_blocks_label);

        // the first ow block, compute left padding

        CGA64::mov(reg_oi, n_oi_first_ow_block);
        CGA64::mov(reg_inp_prf, reg_inp);
        CGA64::mov(reg_out_prf, reg_out);

        if (l_pad > 0) {
            CGA64::ldr(reg_ker_prf, xa::ptr(abi_param1_aarch64, GET_OFF(filt_prf)));
            CGA64::add_imm(reg_inp_prf, reg_inp_prf, inp_shift_pad, reg_tmp_imm);
            CGA64::add_imm(reg_out_prf, reg_out_prf, out_shift, reg_tmp_imm);
            compute_loop(ur_w, l_pad, 0);
            CGA64::add_imm(reg_inp, reg_inp, inp_shift_pad, reg_tmp_imm);
            CGA64::add_imm(reg_out, reg_out, out_shift, reg_tmp_imm);
            CGA64::sub(reg_oi, reg_oi, 1); // decrement
            CGA64::cmp(reg_oi, 0);
        }
        CGA64::b(oi_loop_label);

        // middle or last ow block entry

        CGA64::L_aarch64(middle_ow_blocks_label);

        if (l_pad > 0) {
            // just to consider left padding, not compute
            CGA64::add_imm(reg_inp, reg_inp, inp_shift_pad_second_block, reg_tmp_imm);
            CGA64::add_imm(reg_inp_prf, reg_inp_prf, inp_shift_pad_second_block, reg_tmp_imm);
        }

        // set number of iteration for oi-loop
        CGA64::cmp(reg_owb, jcp.nb_ow - 1); // last ow-block ?
        CGA64::mov(reg_oi, n_oi_last_ow_block);
        CGA64::b(xa::EQ, oi_loop_label);
        CGA64::cmp(reg_owb, jcp.nb_ow - 2); // next to last ow-block ?
        CGA64::mov(reg_oi, n_oi_next_last_ow_block);
        CGA64::b(xa::EQ, oi_loop_label);
        CGA64::mov(reg_oi, n_oi_not_last_ow_block); // other middle ow-blocks

        // oi loop w/o padding
        CGA64::L_aarch64(oi_loop_label);
        CGA64::ldr(reg_ker_prf, xa::ptr(abi_param1_aarch64, GET_OFF(filt_prf)));
        CGA64::L_aarch64(oi_loop_start_label);
            CGA64::cmp(reg_oi, 0);
            CGA64::b(xa::LE, oi_loop_end_label);

            CGA64::add_imm(reg_inp_prf, reg_inp_prf, inp_shift, reg_tmp_imm);
            CGA64::add_imm(reg_out_prf, reg_out_prf, out_shift, reg_tmp_imm);
            compute_loop(ur_w, 0, 0);
            CGA64::add_imm(reg_inp, reg_inp, inp_shift, reg_tmp_imm);
            CGA64::add_imm(reg_out, reg_out, out_shift, reg_tmp_imm);
            CGA64::sub(reg_oi, reg_oi, 1); // dec(reg_oi);
            CGA64::cmp(reg_oi, 0);
            CGA64::b(oi_loop_start_label);
        CGA64::L_aarch64(oi_loop_end_label);

        CGA64::ldr(reg_owb, xa::ptr(abi_param1_aarch64, GET_OFF(owb)));

        CGA64::cmp(reg_owb, 0); // first ow-block ?
        if (first_ow_block_padded) {
            CGA64::b(xa::EQ, last_oi_label);
        } else {
            CGA64::b(xa::EQ, end_label);
        }
        CGA64::cmp(reg_owb, jcp.nb_ow - 2); // next to last ow-block ?
        CGA64::b(xa::LT, end_label);
        if (next_last_ow_block_padded) {
            CGA64::b(xa::EQ, last_oi_label);
        } else {
            CGA64::b(xa::EQ, end_label);
        }
        // that is last block
        if (!last_ow_block_padded) {
            CGA64::b(tail_label);
        }

        // last oi block with right padding
        CGA64::L_aarch64(last_oi_label);
        CGA64::ldr(reg_ker_prf, xa::ptr(abi_param1_aarch64, GET_OFF(filt_prf)));
        CGA64::add_imm(reg_inp_prf, reg_inp_prf, inp_shift, reg_tmp_imm);
        CGA64::add_imm(reg_out_prf, reg_out_prf, out_shift, reg_tmp_imm);
        compute_loop(ur_w, 0, r_pad1);
        CGA64::add_imm(reg_inp, reg_inp, inp_shift, reg_tmp_imm);
        CGA64::add_imm(reg_out, reg_out, out_shift, reg_tmp_imm);

        CGA64::ldr(reg_owb, xa::ptr(abi_param1_aarch64, GET_OFF(owb)));
        CGA64::cmp(reg_owb, jcp.nb_ow - 1); // last ow_block?
        CGA64::b(xa::LT, end_label);

        CGA64::L_aarch64(tail_label);
        CGA64::ldr(reg_ker_prf, xa::ptr(abi_param1_aarch64, GET_OFF(filt_prf)));
        if (ur_w_tail != 0) {
            CGA64::add_imm(reg_inp_prf, reg_inp_prf, inp_shift, reg_tmp_imm);
            CGA64::add_imm(reg_out_prf, reg_out_prf, out_shift, reg_tmp_imm);
            compute_loop(ur_w_tail, 0, r_pad);
        }
        CGA64::L_aarch64(end_label);
    }
    postamble();

//[info]eltwise処理を一時的に削除
    if (jcp.with_eltwise){
        eltwise_injector_->prepare_table();
        binCommit();
    }
}

bool jit_aarch64_sve_512_conv_fwd_kernel::post_ops_ok(
        jit_conv_conf_t &jcp, const primitive_attr_t &attr) {
    const auto &p = attr.post_ops_;

    auto is_eltwise = [&](int idx) { return p.entry_[idx].is_eltwise(); };
    auto is_sum = [&](int idx) { return p.entry_[idx].is_sum(); };

    switch (p.len()) {
        case 0: return true; // no post_ops
        case 1: return is_eltwise(0) || is_sum(0); // sum OR eltwise
        case 2: return is_sum(0) && is_eltwise(1); // sum -> eltwise
        default: return false;
    }

    return false;
}

status_t jit_aarch64_sve_512_conv_fwd_kernel::init_conf(jit_conv_conf_t &jcp,
        const convolution_desc_t &cd, memory_desc_t &src_md,
        memory_desc_t &weights_md, memory_desc_t &dst_md,
        memory_desc_t &bias_md, const primitive_attr_t &attr, int nthreads) {
    using namespace prop_kind;

    if (!mayiuse(sve)){
        return status::unimplemented;
    }

    const memory_desc_wrapper src_d(&src_md);
    const memory_desc_wrapper weights_d(&weights_md);
    const memory_desc_wrapper dst_d(&dst_md);
    const memory_desc_wrapper bias_d(&bias_md);

    const int unroll_4fma = 4;
    const int regs = 28;
    const bool with_groups = weights_d.ndims() == src_d.ndims() + 1;
    int ndims = src_d.ndims();

    jcp = zero<decltype(jcp)>();
    jcp.nthr = jcp.aligned_threads = nthreads;
    jcp.ndims = ndims;
    jcp.prop_kind = cd.prop_kind;
    jcp.ngroups = with_groups ? weights_d.dims()[0] : 1;
    jcp.mb = src_d.dims()[0];
    jcp.oc = dst_d.dims()[1] / jcp.ngroups;
    jcp.oc_without_padding = jcp.oc;
    jcp.ic = src_d.dims()[1] / jcp.ngroups;
    jcp.ic_without_padding = jcp.ic;
    jcp.id = (ndims == 5) ? src_d.dims()[2] : 1;
    jcp.ih = (ndims == 3) ? 1 : src_d.dims()[ndims - 2];
    jcp.iw = src_d.dims()[ndims - 1];
    jcp.od = (ndims == 5) ? dst_d.dims()[2] : 1;
    jcp.oh = (ndims == 3) ? 1 : dst_d.dims()[ndims - 2];
    jcp.ow = dst_d.dims()[ndims - 1];
    jcp.kd = (ndims == 5) ? weights_d.dims()[with_groups + 2] : 1;
    jcp.kh = (ndims == 3) ? 1 : weights_d.dims()[with_groups + ndims - 2];
    jcp.kw = weights_d.dims()[with_groups + ndims - 1];
    jcp.f_pad = (ndims == 5) ? cd.padding[0][0] : 0;
    jcp.t_pad = (ndims == 3) ? 0 : cd.padding[0][ndims - 4];
    jcp.l_pad = cd.padding[0][ndims - 3];
    jcp.stride_d = (ndims == 5) ? cd.strides[0] : 1;
    jcp.stride_h = (ndims == 3) ? 1 : cd.strides[ndims - 4];
    jcp.stride_w = cd.strides[ndims - 3];

    jcp.dilate_d = (ndims == 5) ? cd.dilates[0] : 0;
    jcp.dilate_h = (ndims == 3) ? 0 : cd.dilates[ndims - 4];
    jcp.dilate_w = cd.dilates[ndims - 3];

    int ext_kw = calculate_extended_filter_size(jcp.kw, jcp.dilate_w);
    int ext_kh = calculate_extended_filter_size(jcp.kh, jcp.dilate_h);
    int ext_kd = calculate_extended_filter_size(jcp.kd, jcp.dilate_d);
    jcp.r_pad = calculate_end_padding(
            jcp.l_pad, jcp.ow, jcp.iw, jcp.stride_w, ext_kw);
    jcp.b_pad = calculate_end_padding(
            jcp.t_pad, jcp.oh, jcp.ih, jcp.stride_h, ext_kh);
    jcp.back_pad = calculate_end_padding(
            jcp.f_pad, jcp.od, jcp.id, jcp.stride_d, ext_kd);
    bool kernel_outside_src = false || ext_kw <= jcp.l_pad
            || ext_kw <= jcp.r_pad || ext_kh <= jcp.t_pad || ext_kh <= jcp.b_pad
            || ext_kd <= jcp.f_pad || ext_kd <= jcp.back_pad;
    if (kernel_outside_src) {
       return status::unimplemented;
    }

    const auto dat_tag_nxc = pick(ndims - 3, nwc, nhwc, ndhwc);
    const auto dat_tag_ncx = pick(ndims - 3, ncw, nchw, ncdhw);
    const auto dat_tag_nCx4c = pick(ndims - 3, nCw4c, nChw4c, nCdhw4c);
    const auto dat_tag_nCx8c = pick(ndims - 3, nCw8c, nChw8c, nCdhw8c);
    const auto dat_tag_nCx16c = pick(ndims - 3, nCw16c, nChw16c, nCdhw16c);
    auto curr_src_tag = src_d.matches_one_of_tag(dat_tag_nxc, dat_tag_nCx16c,
            dat_tag_nCx8c, dat_tag_nCx4c, dat_tag_ncx);
    auto curr_dst_tag = dst_d.matches_one_of_tag(
            dat_tag_nxc, dat_tag_nCx16c, dat_tag_nCx8c, dat_tag_nCx4c);
    bool is_data_layout_nxc
            = utils::everyone_is(dat_tag_nxc, curr_src_tag, curr_dst_tag);
#if 0
//[info]取り敢えずコメント化
    if (mayiuse(avx512_mic) && is_data_layout_nxc) return status::unimplemented;
#endif

    jcp.is_1stconv = is_1stconv(jcp);

    bool ok_to_pad_channels = true && !is_data_layout_nxc && jcp.ngroups == 1
            && src_d.data_type() == data_type::f32;

    const int full_simd_w = cpu_isa_traits<sve>::vlen / typesize;
    jcp.simd_w = full_simd_w;
#if 0
//[info]v1.6にて変更。内容不明なので取り敢えずコメント化
    bool ok_to_try_lower_zmm = true
            && IMPLICATION(is_data_layout_nxc,
                    jcp.oc < full_simd_w && jcp.ic < full_simd_w
                            && jcp.ngroups > 1)
            && mayiuse(avx512_core) && src_d.data_type() == data_type::f32
            && !jcp.is_1stconv && !ok_to_pad_channels
            && (jcp.ic % jcp.simd_w != 0 || jcp.oc % jcp.simd_w != 0);

    if (ok_to_try_lower_zmm) {
        for (auto simd : {8, 4}) {
            if (jcp.ic % simd == 0 && jcp.oc % simd == 0) {
                jcp.simd_w = simd;
                break;
            }
        }
    }
#else
    bool ok_to_try_lower_zmm = true
            && IMPLICATION(is_data_layout_nxc,
                    jcp.oc < full_simd_w && jcp.ic < full_simd_w
                            && jcp.ngroups > 1)
            && mayiuse(sve) && src_d.data_type() == data_type::f32
            && !jcp.is_1stconv && !ok_to_pad_channels
            && (jcp.ic % jcp.simd_w != 0 || jcp.oc % jcp.simd_w != 0);

    if (ok_to_try_lower_zmm) {
        for (auto simd : {8, 4}) {
            if (jcp.ic % simd == 0 && jcp.oc % simd == 0) {
                jcp.simd_w = simd;
                break;
            }
        }
    }
#endif

    jcp.oc_block = jcp.simd_w;
    jcp.ic_block = jcp.is_1stconv ? jcp.ic : jcp.simd_w;

    if (ok_to_pad_channels) {
        jcp.oc = rnd_up(jcp.oc, jcp.oc_block);
        jcp.ic = rnd_up(jcp.ic, jcp.ic_block);
    }
    if (!IMPLICATION(!is_data_layout_nxc,
                jcp.oc % jcp.oc_block == 0 && jcp.ic % jcp.ic_block == 0)) {
        return status::unimplemented;
    }
    jcp.ic_tail = is_data_layout_nxc ? jcp.ic % jcp.simd_w : 0;
    jcp.oc_tail = is_data_layout_nxc ? jcp.oc % jcp.simd_w : 0;

    if (!post_ops_ok(jcp, attr)) {
        return status::unimplemented;
    }

    const auto &p = attr.post_ops_;
    jcp.with_sum = p.find(primitive_kind::sum) != -1;
    const int eltwise_ind = p.find(primitive_kind::eltwise);
    jcp.with_eltwise = eltwise_ind != -1;
    if (jcp.with_eltwise) {
        jcp.eltwise = p.entry_[eltwise_ind].eltwise;
        if (dst_d.data_type() == data_type::s32) return status::unimplemented;
    }

    format_tag_t src_tag, dst_tag, wei_tag;

    if (jcp.simd_w == 8) {
        assert(with_groups);
        src_tag = is_data_layout_nxc ? dat_tag_nxc : dat_tag_nCx8c;
        dst_tag = src_tag;
        wei_tag = pick(ndims - 3, gOIw8i8o, gOIhw8i8o, gOIdhw8i8o);
    } else if (jcp.simd_w == 4) {
        assert(with_groups);
        src_tag = is_data_layout_nxc ? dat_tag_nxc : dat_tag_nCx4c;
        dst_tag = src_tag;
        wei_tag = pick(ndims - 3, gOIw4i4o, gOIhw4i4o, gOIdhw4i4o);
    } else {
        dst_tag = is_data_layout_nxc ? dat_tag_nxc : dat_tag_nCx16c;
        src_tag = is_data_layout_nxc
                ? dat_tag_nxc
                : (jcp.is_1stconv ? dat_tag_ncx : dat_tag_nCx16c);
        wei_tag = pick(2 * ndims - 6 + with_groups, OIw16i16o, gOIw16i16o,
                OIhw16i16o, gOIhw16i16o, OIdhw16i16o, gOIdhw16i16o);
    }

    if (src_md.format_kind == format_kind::any)
        CHECK(memory_desc_init_by_tag(src_md, src_tag));
    else if (curr_src_tag != src_tag)
        return status::unimplemented;
    jcp.src_tag = src_tag;

    if (dst_md.format_kind == format_kind::any)
        CHECK(memory_desc_init_by_tag(dst_md, dst_tag));
    else if (curr_dst_tag != dst_tag)
        return status::unimplemented;
    jcp.dst_tag = dst_tag;

    jcp.with_bias = cd.bias_desc.format_kind != format_kind::undef;
    if (jcp.with_bias) {
        if (bias_d.format_kind() == format_kind::any)
            CHECK(memory_desc_init_by_tag(bias_md, x));
    }

    if (mayiuse(sve) && src_d.data_type() == data_type::f32
            && weights_d.data_type() == data_type::f32
            && dst_d.data_type() == data_type::f32) {
        jcp.ver = ver_fma;
        jcp.typesize_in = typesize;
        jcp.typesize_out = typesize;
#if 0
        if (!is_data_layout_nxc && mayiuse(avx512_mic_4ops)) jcp.ver = ver_4fma;
#endif

        if (jcp.is_1stconv) {
            /* NOTE:
             * 1) When memory-protection is enabled, this guards against a
             * seg-fault from illegal memory access. A potential solution is
             * to enable tail processing within 'compute_loop_4fma_1st'
             * 2) 4FMA Kernel does not support:
             *  `l_pad > 0 || r_pad > 0`; when `kw > 1`
             * from incorrect 'get_ow_start' and 'get_ow_end' calculation, so
             * disable for now. */
            bool not_for_4fma = jcp.l_pad > 0 // needed in case jcp.r_pad < 0
                    || (rnd_up(jcp.kw, unroll_4fma)
                                    + (jcp.ow - 1) * jcp.stride_w
                            > jcp.iw);
            bool is_dilated
                    = !everyone_is(0, jcp.dilate_d, jcp.dilate_h, jcp.dilate_w);
            if (one_of(true, not_for_4fma, is_dilated)) jcp.ver = ver_fma;
#if 0
            if (jcp.ver == ver_4fma) {
                wei_tag = with_groups
                        ? ((jcp.simd_w == 4) ? pick(
                                   ndims - 3, gOiw4o, gOihw4o, gOidhw4o)
                                             : pick(ndims - 3, gOiw16o,
                                                     gOihw16o, gOidhw16o))
                        : pick(ndims - 3, Oiw16o, Oihw16o, Oidhw16o);
            } else {
                wei_tag = with_groups
                        ? ((jcp.simd_w == 4) ? pick(
                                   ndims - 3, gOwi4o, gOhwi4o, gOdhwi4o)
                                             : pick(ndims - 3, gOwi16o,
                                                     gOhwi16o, gOdhwi16o))
                        : pick(ndims - 3, Owi16o, Ohwi16o, Odhwi16o);
            }
#else
                wei_tag = with_groups
                        ? ((jcp.simd_w == 4) ? pick(
                                   ndims - 3, gOwi4o, gOhwi4o, gOdhwi4o)
                                             : pick(ndims - 3, gOwi16o,
                                                     gOhwi16o, gOdhwi16o))
                        : pick(ndims - 3, Owi16o, Ohwi16o, Odhwi16o);
#endif
        }
    } else {
        return status::unimplemented;
    }

    if (init_tag(jcp.wei_tag, weights_md, weights_d, wei_tag)
            != status::success)
        return status::unimplemented;

#if 0
    if (jcp.is_1stconv) {
        jcp.ur_w = nstl::min(jcp.ow, regs);
    } else {
        // avx512_core guard - just to avoid possible regression for other archs
        if (jcp.ver == ver_fma && mayiuse(avx512_core)) {
            jcp.ur_w = nstl::min(jcp.ow, regs);
        } else {
            for (int ur_w = regs; ur_w > 0; --ur_w) {
                if (jcp.ow % ur_w == 0) {
                    jcp.ur_w = ur_w;
                    break;
                }
            }
        }
        if ((ndims == 5 && jcp.ur_w <= 8) || (jcp.ur_w <= 1)) {
            jcp.ur_w = nstl::min(jcp.ow, regs);
        }
    }
#else
    jcp.ur_w = nstl::min(jcp.ow, regs); // ur_w is min(output width, regs=28)
#endif
    // TODO (Tanya): currently applied to Segnet convolutions only.
    // Need to try for other topologies
    if (jcp.ow > 150 && jcp.ur_w < regs / 2) jcp.ur_w = regs;

    int n_oi = (jcp.ow / jcp.ur_w);
    int r_pad = calculate_end_padding(
            jcp.l_pad, jcp.ur_w * n_oi, jcp.iw, jcp.stride_w, ext_kw);
    if (jcp.l_pad > 0 && r_pad > 0) n_oi--;

    // Heuristic to optimize code size on KNX
    bool large_code_size = jcp.ur_w != jcp.ow && jcp.l_pad > 0 && r_pad > 0
            && ((jcp.l_pad <= 0 && n_oi > 0) || (jcp.l_pad > 0 && n_oi > 1));
    if (large_code_size) {
        const int max_code_size = 24 * 1024;
        const int num_ops_per_reg = 6 + jcp.ic_block * jcp.kw;
        int mult = 1;
        if (jcp.l_pad > 0) mult += 1;
        if (r_pad > 0) mult += 1;
        for (int ur_w = jcp.ur_w; ur_w > regs / 2; --ur_w) {
            if (ur_w * mult * num_ops_per_reg * 9 < max_code_size) {
                jcp.ur_w = ur_w;
                break;
            }
        }
    }

    /* Grouped channel offset to support 'non-blocked data' format for
     * convolution sizes with '(input_channel / ngroups) < simd' */
    jcp.nonblk_group_off
            = (jcp.ngroups > 1 && one_of(jcp.src_tag, ncw, nchw, ncdhw))
            ? jcp.ic
            : 1;

    jcp.nb_ic = div_up(jcp.ic, jcp.ic_block);
    jcp.nb_oc = div_up(jcp.oc, jcp.oc_block);
    jcp.nb_ic_blocking = jcp.nb_oc_blocking = 1;

#if 0
    auto is_ow_threading_applicable = [=]() {
        return (true && !jcp.is_1stconv && one_of(jcp.ndims, 3, 4)
                && IMPLICATION(mayiuse(avx512_mic),
                        jcp.ver == ver_4fma
                                && IMPLICATION(jcp.mb != 1,
                                        jcp.ih == 1 && jcp.kh == 1)));
    };

    if (jcp.ver == ver_4fma && !jcp.is_1stconv) {
        if ((jcp.kw <= 5 && jcp.kh <= 5 && jcp.kw == jcp.kh && jcp.ow <= 8
                    && jcp.oh <= 8 && jcp.ow == jcp.oh)
                || (jcp.stride_h != 1 && jcp.ur_w < jcp.ow)) {
            if (jcp.nb_oc % 2 == 0) {
                jcp.nb_oc_blocking = 2;
                jcp.ur_w = nstl::min(jcp.ow, regs / jcp.nb_oc_blocking);
            }
        } else {
            for (int i = jcp.nb_oc; i > 0; i--)
                if (i * jcp.ur_w <= regs && jcp.nb_oc % i == 0) {
                    jcp.nb_oc_blocking = i;
                    break;
                }
        }
        if (jcp.ver == ver_4fma && is_ow_threading_applicable()) {
            if (jcp.nb_oc % 2 == 0 && jcp.ur_w < jcp.ow
                    && jcp.ow != 2 * jcp.ur_w) {
                jcp.nb_oc_blocking = 2;
                jcp.ur_w = nstl::min(jcp.ow, regs / jcp.nb_oc_blocking);
            }
        }
    }
#endif

    jcp.ow_block = jcp.ow;

    auto get_thr_eff = [=](int nb_oc_blocking, int ow_block) {
        int nb_ow = div_up(jcp.ow, ow_block);
        int nb_oc_chunks = div_up(jcp.nb_oc, nb_oc_blocking);
        int work_amount = jcp.mb * jcp.oh * nb_oc_chunks * nb_ow;
        float disbalance = (float)jcp.ow / rnd_up(jcp.ow, ow_block);
        float thr_eff = disbalance * (float)work_amount
                / rnd_up(work_amount, jcp.nthr);
        return thr_eff;
    };

#if 0
    auto is_ow_threading_applicable = [=]() {
        return (true && !jcp.is_1stconv && one_of(jcp.ndims, 3, 4)
                 && IMPLICATION(mayiuse(avx512_mic),
                 jcp.ver == ver_4fma && IMPLICATION(jcp.mb != 1, jcp.ih == 1 && jcp.kh == 1)));
    };
#else
    auto is_ow_threading_applicable = [=]() {
        return (false);
    };
#endif

    auto get_ow_block = [=](int nb_oc_blocking, int ur_w, float &eff) {
        int res_ow_block = jcp.ow;
        eff = get_thr_eff(nb_oc_blocking, res_ow_block);
        if (!is_ow_threading_applicable()) return res_ow_block;

        int L2_part = (platform::get_per_core_cache_size(2) * 7 / 8) / typesize;
//        if (jcp.ver == ver_4fma) L2_part /= 2;
        int size_src_chunk = jcp.ic_block * ur_w * jcp.kh;
        int size_dst_chunk = jcp.oc_block * nb_oc_blocking * ur_w;
        int size_wei_chunk = jcp.oc_block * nb_oc_blocking * jcp.ic_block
                * jcp.kw * jcp.kh;
        int nurw_cache = (L2_part - 2 * size_wei_chunk)
                / (2 * size_dst_chunk + 2 * size_src_chunk);
        // current design of generate() requires ow_block >= 2 * ur_w
        int ow_block_cache = ur_w * nstl::max(2, nurw_cache);

        int ow_block_thr = ow_block_cache;
        eff = get_thr_eff(nb_oc_blocking, ow_block_thr);

        int max_nb_ow = div_up(jcp.ow, 2 * ur_w);
        int start_nb_ow = div_up(jcp.ow, ow_block_thr);
        for (int nb_ow = start_nb_ow; nb_ow <= max_nb_ow; nb_ow++) {
            int ow_block
                    = nstl::min(rnd_up(div_up(jcp.ow, nb_ow), ur_w), jcp.ow);
#if 0
            float eff_threshold = (jcp.ver == ver_4fma) ? 0.8f : 0.9f;
#else
            float eff_threshold = 0.9f;
#endif
            if (ow_block < nb_oc_blocking * jcp.oc_block && eff > eff_threshold)
                break;
            if (div_up(jcp.ow, ow_block) != nb_ow) continue;
            float thr_eff = get_thr_eff(nb_oc_blocking, ow_block);
#if 0
            float eff_step = (jcp.ver == ver_4fma) ? 1.1f : 1.f;
#else
            float eff_step = 1.f;
#endif
            if (ow_block >= 2 * ur_w && thr_eff > eff_step * eff) {
                ow_block_thr = ow_block;
                eff = thr_eff;
            }
#if 0
            eff_threshold = (jcp.ver == ver_4fma) ? 0.9f : 0.98f;
#else
            eff_threshold = 0.98f;
#endif
            if (eff > eff_threshold) break;
        }
        res_ow_block = nstl::min(jcp.ow, nstl::max(2 * ur_w, ow_block_thr));
        eff = get_thr_eff(nb_oc_blocking, res_ow_block);
        return res_ow_block;
    };

#if 0
    if (jcp.ver == ver_fma && mayiuse(avx512_core)) {
        int try_nb_oc_blocking = 2;
        unsigned int ker_inp_size = typesize * div_up(jcp.iw, jcp.stride_w)
                * jcp.ic_block * jcp.kh * jcp.kd;
        unsigned int ker_out_size
                = typesize * jcp.ow * jcp.oc_block * try_nb_oc_blocking;
        unsigned int ker_wei_size = typesize * jcp.kh * jcp.kw * jcp.ic_block
                * jcp.oc_block * try_nb_oc_blocking * jcp.kd;
        unsigned int ker_total_size
                = ker_inp_size + ker_out_size + ker_wei_size;

        bool embd_bcast_condition_base = true
                && (jcp.kw == 3 && jcp.ow <= 28
                        && ker_total_size < L1_cache_size)
                && !(jcp.kw == 3 && jcp.ow == 13 && jcp.ic >= 192)
                && !(jcp.kw == 3 && jcp.ow == 28 && jcp.ic >= 512);

        // These conditions define a set of shapes with 'ow = 1' which
        // have a very limited optimization space for performance. Try
        // to optimize by using a larger 'nb_oc_blocking' size.
        bool expl_bcast_condition
                = everyone_is(1, jcp.ngroups, jcp.mb, jcp.stride_h, jcp.ow,
                          jcp.stride_w, jcp.id, jcp.od, jcp.kd, jcp.stride_d)
                && jcp.iw == jcp.kw && jcp.nb_oc > 1
                && everyone_is(0, jcp.l_pad, jcp.r_pad, jcp.dilate_w, jcp.f_pad,
                        jcp.back_pad, jcp.dilate_d)
                && jcp.oh >= 60 && jcp.kh >= 3;

        bool embd_bcast_condition = !expl_bcast_condition
                && (jcp.kw > 3
                        || (jcp.stride_w == 1 && jcp.stride_h == 1
                                && embd_bcast_condition_base)
                        || ((jcp.stride_w != 1 || jcp.stride_h != 1)
                                && ((jcp.mb <= 16
                                        && (jcp.oc <= 192 || jcp.oh <= 10)
                                        && embd_bcast_condition_base)))
                        || (jcp.mb == 1
                                && (jcp.ur_w >= jcp.ow || jcp.is_1stconv
                                        || (jcp.ow <= 147 && jcp.oc <= 96))));

        if (jcp.mb == 1) {
            unsigned int inp_size = jcp.mb * div_up(jcp.ih, jcp.stride_h)
                    * div_up(jcp.iw, jcp.stride_w) * jcp.ic;
            unsigned int wei_size = jcp.ic * jcp.oc * jcp.kh * jcp.kw;

            // Estimate whether we need to limit the number of threads
            // and calculate this number. Includes some heuristic.
            int oc_chunks = jcp.nb_oc / jcp.nb_oc_blocking;
            int work_amount = jcp.mb * jcp.ngroups * oc_chunks * jcp.oh;
            int job_size_min = work_amount / nthreads;
            int job_size_max = div_up(work_amount, nthreads);
            int ch_max = rnd_up(jcp.oh, job_size_max);
            int ch_min = (job_size_min == 0) ? jcp.oh
                                             : rnd_up(jcp.oh, job_size_min);
            bool not_aligned_max = ch_max % jcp.oh != 0 && ch_max / jcp.oh < 2
                    && (jcp.oh != 8 || ch_max / jcp.oh > 1);
            bool not_aligned_min = ch_min % jcp.oh != 0 && ch_min / jcp.oh < 2
                    && (jcp.oh != 8 || ch_min / jcp.oh > 1);
            bool eligible_case = (jcp.stride_h == 1 && jcp.stride_w == 1)
                    || nthreads > oc_chunks;
            if (jcp.loop_order == loop_cgn && oc_chunks > 1 && nthreads > 1
                    && wei_size / inp_size > 24
                    && (not_aligned_max || not_aligned_min) && eligible_case) {
                // Try to find number of threads > nthreads / 2 such that
                // oc_chunks is a multiple of nthreads, or nthreads is a
                // multiple of oc_chunks. Otherwise, keep default value.
                // TODO: implement a task-based alternative without throttling.
                jcp.aligned_threads = jcp.nthr;
                for (int i = jcp.nthr; i > jcp.nthr / 2; i--) {
                    if (oc_chunks % i == 0 || i % oc_chunks == 0) {
                        jcp.aligned_threads = i;
                        break;
                    }
                }
            }
        }

        const int max_nb_oc = 5;
        if (embd_bcast_condition) {
            jcp.kernel_kind = embd_bcast;
            jcp.ur_w = nstl::min(jcp.ow, regs);
            jcp.nb_ic_blocking = jcp.nb_oc_blocking = 1;
            if (ker_total_size < L1_cache_size && jcp.ow <= 8 && jcp.kh <= 3
                    && jcp.kw <= 3 && jcp.nb_oc % try_nb_oc_blocking == 0
                    && IMPLICATION(jcp.is_1stconv, jcp.mb == 1)
                    && IMPLICATION(jcp.mb == 1, jcp.ur_w < jcp.ow)) {
                jcp.nb_oc_blocking = try_nb_oc_blocking;
                jcp.ur_w = nstl::min(jcp.ow, 31 / (jcp.nb_oc_blocking + 1));
            }
        } else {
            jcp.kernel_kind = expl_bcast;
            jcp.nb_ic_blocking = 1;
            if (IMPLICATION(jcp.is_1stconv, jcp.mb > 1)
                    || expl_bcast_condition) {
                float best_thr_eff = 0.f;
                int best_nb_oc_blocking = 1;
                for (int i = nstl::min(jcp.nb_oc, max_nb_oc); i > 0; i--) {
                    if (jcp.nb_oc % i == 0) {
                        if (expl_bcast_condition) {
                            best_nb_oc_blocking = i;
                            break;
                        } else {
                            float thr_eff;
                            int ur_w = nstl::min(jcp.ow, 31 / (i + 1));
                            get_ow_block(i, ur_w, thr_eff);
                            if (thr_eff > 1.05f * best_thr_eff) {
                                best_nb_oc_blocking = i;
                                best_thr_eff = thr_eff;
                            }
                        }
                    }
                }
                jcp.nb_oc_blocking = best_nb_oc_blocking;
                jcp.ur_w = nstl::min(jcp.ow, 31 / (jcp.nb_oc_blocking + 1));
            }
        }
    }
#else
    if (jcp.ver == ver_fma && mayiuse(sve)) {
        int try_nb_oc_blocking = 2;
        unsigned int ker_inp_size = typesize * div_up(jcp.iw, jcp.stride_w)
                * jcp.ic_block * jcp.kh * jcp.kd;
        unsigned int ker_out_size
                = typesize * jcp.ow * jcp.oc_block * try_nb_oc_blocking;
        unsigned int ker_wei_size = typesize * jcp.kh * jcp.kw * jcp.ic_block
                * jcp.oc_block * try_nb_oc_blocking * jcp.kd;
        unsigned int ker_total_size
                = ker_inp_size + ker_out_size + ker_wei_size;

        bool embd_bcast_condition_base = true
                && (jcp.kw == 3 && jcp.ow <= 28
                        && ker_total_size < L1_cache_size)
                && !(jcp.kw == 3 && jcp.ow == 13 && jcp.ic >= 192)
                && !(jcp.kw == 3 && jcp.ow == 28 && jcp.ic >= 512);

        // These conditions define a set of shapes with 'ow = 1' which
        // have a very limited optimization space for performance. Try
        // to optimize by using a larger 'nb_oc_blocking' size.
        bool expl_bcast_condition
                = everyone_is(1, jcp.ngroups, jcp.mb, jcp.stride_h, jcp.ow,
                          jcp.stride_w, jcp.id, jcp.od, jcp.kd, jcp.stride_d)
                && jcp.iw == jcp.kw && jcp.nb_oc > 1
                && everyone_is(0, jcp.l_pad, jcp.r_pad, jcp.dilate_w, jcp.f_pad,
                        jcp.back_pad, jcp.dilate_d)
                && jcp.oh >= 60 && jcp.kh >= 3;

        bool embd_bcast_condition = !expl_bcast_condition
                && (jcp.kw > 3
                        || (jcp.stride_w == 1 && jcp.stride_h == 1
                                && embd_bcast_condition_base)
                        || ((jcp.stride_w != 1 || jcp.stride_h != 1)
                                && ((jcp.mb <= 16
                                        && (jcp.oc <= 192 || jcp.oh <= 10)
                                        && embd_bcast_condition_base)))
                        || (jcp.mb == 1
                                && (jcp.ur_w >= jcp.ow || jcp.is_1stconv
                                        || (jcp.ow <= 147 && jcp.oc <= 96))));

        if (jcp.mb == 1) {
            unsigned int inp_size = jcp.mb * div_up(jcp.ih, jcp.stride_h)
                    * div_up(jcp.iw, jcp.stride_w) * jcp.ic;
            unsigned int wei_size = jcp.ic * jcp.oc * jcp.kh * jcp.kw;

            // Estimate whether we need to limit the number of threads
            // and calculate this number. Includes some heuristic.
            int oc_chunks = jcp.nb_oc / jcp.nb_oc_blocking;
            int work_amount = jcp.mb * jcp.ngroups * oc_chunks * jcp.oh;
            int job_size_min = work_amount / nthreads;
            int job_size_max = div_up(work_amount, nthreads);
            int ch_max = rnd_up(jcp.oh, job_size_max);
            int ch_min = (job_size_min == 0) ? jcp.oh
                                             : rnd_up(jcp.oh, job_size_min);
            bool not_aligned_max = ch_max % jcp.oh != 0 && ch_max / jcp.oh < 2
                    && (jcp.oh != 8 || ch_max / jcp.oh > 1);
            bool not_aligned_min = ch_min % jcp.oh != 0 && ch_min / jcp.oh < 2
                    && (jcp.oh != 8 || ch_min / jcp.oh > 1);
            bool eligible_case = (jcp.stride_h == 1 && jcp.stride_w == 1)
                    || nthreads > oc_chunks;
            if (jcp.loop_order == loop_cgn && oc_chunks > 1 && nthreads > 1
                    && wei_size / inp_size > 24
                    && (not_aligned_max || not_aligned_min) && eligible_case) {
                // Try to find number of threads > nthreads / 2 such that
                // oc_chunks is a multiple of nthreads, or nthreads is a
                // multiple of oc_chunks. Otherwise, keep default value.
                // TODO: implement a task-based alternative without throttling.
                jcp.aligned_threads = jcp.nthr;
                for (int i = jcp.nthr; i > jcp.nthr / 2; i--) {
                    if (oc_chunks % i == 0 || i % oc_chunks == 0) {
                        jcp.aligned_threads = i;
                        break;
                    }
                }
            }
        }

        const int max_nb_oc = 5;
#if 0
        if (embd_bcast_condition) {
            jcp.kernel_kind = embd_bcast;
            jcp.ur_w = nstl::min(jcp.ow, regs);
            jcp.nb_ic_blocking = jcp.nb_oc_blocking = 1;
            if (ker_total_size < L1_cache_size && jcp.ow <= 8 && jcp.kh <= 3
                    && jcp.kw <= 3 && jcp.nb_oc % try_nb_oc_blocking == 0
                    && IMPLICATION(jcp.is_1stconv, jcp.mb == 1)
                    && IMPLICATION(jcp.mb == 1, jcp.ur_w < jcp.ow)) {
                jcp.nb_oc_blocking = try_nb_oc_blocking;
                jcp.ur_w = nstl::min(jcp.ow, 31 / (jcp.nb_oc_blocking + 1));
            }
        } else {
#endif
        {
            jcp.kernel_kind = expl_bcast;
            jcp.nb_ic_blocking = 1;
            if (IMPLICATION(jcp.is_1stconv, jcp.mb > 1)
                    || expl_bcast_condition) {
                float best_thr_eff = 0.f;
                int best_nb_oc_blocking = 1;
                for (int i = nstl::min(jcp.nb_oc, max_nb_oc); i > 0; i--) {
                    if (jcp.nb_oc % i == 0) {
                        if (expl_bcast_condition) {
                            best_nb_oc_blocking = i;
                            break;
                        } else {
                            float thr_eff;
                            int ur_w = nstl::min(jcp.ow, 31 / (i + 1));
                            get_ow_block(i, ur_w, thr_eff);
                            if (thr_eff > 1.05f * best_thr_eff) {
                                best_nb_oc_blocking = i;
                                best_thr_eff = thr_eff;
                            }
                        }
                    }
                }
                jcp.nb_oc_blocking = best_nb_oc_blocking;
                jcp.ur_w = nstl::min(jcp.ow, 31 / (jcp.nb_oc_blocking + 1));
            }
        }
    }
#endif

    jcp.ur_w_tail = jcp.ow % jcp.ur_w;

    bool args_ok = true && jcp.l_pad <= jcp.ur_w
            && jcp.ic <= src_d.padded_dims()[1]
            && jcp.oc <= dst_d.padded_dims()[1]
            && jcp.ic <= weights_d.padded_dims()[with_groups + 1]
            && jcp.oc <= weights_d.padded_dims()[with_groups + 0];
    if (!args_ok) return status::unimplemented;

    int r_pad_no_tail = nstl::max(0,
            calculate_end_padding(jcp.l_pad, jcp.ow - jcp.ur_w_tail, jcp.iw,
                    jcp.stride_w, ext_kw));
    if (r_pad_no_tail > jcp.ur_w) return status::unimplemented;

    pick_loop_order(jcp);

    jcp.nb_ic_L2 = jcp.nb_ic;

    float thr_eff;
    jcp.ow_block = get_ow_block(jcp.nb_oc_blocking, jcp.ur_w, thr_eff);
    jcp.nb_ow = div_up(jcp.ow, jcp.ow_block);

//[info]buildエラー仮対処
#if 0
    const int L2_size = get_A64FX_cache_size(2, false, nthreads) / sizeof(float);
#else
    const int L2_size = platform::get_per_core_cache_size(2) / sizeof(float);
#endif

    // Source and output data needs to fit in L2,
    // leaving some space for weights and prefetching.
    int h_L2 = int(((0.6f * L2_size) / jcp.simd_w
                           - nstl::min(0, jcp.kh - jcp.stride_h) * jcp.iw)
            / (jcp.stride_h * jcp.iw + jcp.ow));
    jcp.h_blocking = nstl::max(1, nstl::min(jcp.oh, h_L2));

#if 0
    if (jcp.ver == ver_4fma) {
        if (!is_ow_threading_on(jcp)) {
            for (int divf = 2, temp_nb = jcp.nb_ic_L2; divf <= jcp.nb_ic;
                    divf++) {
                size_t l2_src = (size_t)jcp.iw * jcp.ic_block * jcp.ih * temp_nb
                        * jcp.id;
                size_t l2_dst = (size_t)jcp.ow * jcp.oc_block
                        * jcp.nb_oc_blocking * jcp.oh * jcp.od;
                size_t l2_filt = (size_t)jcp.kw * jcp.oc_block * jcp.ic_block
                        * jcp.kh * jcp.nb_oc_blocking * temp_nb * jcp.kd;
                if (4 * (l2_src + l2_dst + l2_filt)
                        > KNx_L2_EFFECTIVE_CAPACITY) {
                    if (jcp.kh == 3 && jcp.oh == 7) {
                        jcp.nb_ic_L2 = 1;
                        break;
                    }
                    temp_nb = (jcp.nb_ic_L2 % divf == 0 ? jcp.nb_ic_L2 / divf
                                                        : jcp.nb_ic_L2);
                } else {
                    jcp.nb_ic_L2 = temp_nb;
                    break;
                }
            }
        } else if (jcp.ic > 64) {
            jcp.nb_ic_L2 = 2; /* according to performance data*/
        }
    }
#endif

    if (is_data_layout_nxc) {
        // TODO: improve L2 blocking for large IC
        const int nb_ic_theshold_L2 = 32;
        if (jcp.nb_ic > nb_ic_theshold_L2 && jcp.nb_ic < 2 * nb_ic_theshold_L2)
            jcp.nb_ic_L2 = div_up(jcp.nb_ic, 2);
        else
            jcp.nb_ic_L2 = nstl::min(nb_ic_theshold_L2, jcp.nb_ic);
    }

    // A rough check on code size
    // TODO: come up with a tighter bound
    {
        const int max_code_size = 256 * 1024; // default size of jit generator
        int mult = 1 + (jcp.l_pad > 0) + (r_pad > 0);
        const float max_instruction_size = 15;
        float ur_fac
                = (float)jcp.kw * jcp.ic_block * jcp.nb_oc_blocking * jcp.ur_w;
        float code_size = mult * ur_fac * max_instruction_size;
        if (code_size > max_code_size) return status::unimplemented;
    }

    return status::success;
}

void jit_aarch64_sve_512_conv_fwd_kernel::init_scratchpad(
        memory_tracking::registrar_t &scratchpad, const jit_conv_conf_t &jcp) {

    if (jcp.with_bias && jcp.oc != jcp.oc_without_padding)
        scratchpad.book(key_conv_padded_bias, jcp.oc, jcp.typesize_out);
}

#if 0
template <typename Vmm>
void _jit_avx512_common_conv_bwd_data_kernel_f32<Vmm>::prepare_output(
        int ur_w) {
    for (int k = 0; k < jcp.nb_ic_blocking; k++) {
        for (int j = 0; j < ur_w; j++) {
            Vmm vmm = vmm_out(j, k);
            vpxord(vmm, vmm, vmm);
            size_t aux_src_offset = get_diff_src_offset(j, k);
            mic_prefetcht1(EVEX_compress_addr_safe(
                    reg_src_prf, aux_src_offset, reg_long_offt));
        }
    }
}
#else
//[info]v0.21 codeを展開
template <typename Vmm>
void _jit_aarch64_sve_512_conv_bwd_data_kernel_f32<Vmm>::prepare_output(int ur_w)
{
    auto zreg_out_s = [=](int i_ur, int i_oc){
        int idx = i_ur + i_oc * jcp.ur_w;
        assert(idx < ker_reg_base_idx);
        return xa::ZRegS(idx);
    };

    for (int k = 0; k < jcp.nb_ic_blocking; k++) {
        for (int j = 0; j < ur_w; j++) {
            xa::ZRegS zreg = zreg_out_s(j, k);
            CGA64::fmov(zreg);
            size_t aux_src_offset
                = (size_t)typesize * ((size_t)k * jcp.ih * jcp.iw * jcp.id + j)
                * jcp.ic_block;

	    std::string op = "LD";
	    prefetch(op, 2, reg_src_prf, aux_src_offset);
            // mic_prefetcht1(EVEX_compress_addr_safe
	    // (reg_src_prf, aux_src_offset, reg_long_offt));
        }
    }
}
#endif

#if 0
template <typename Vmm>
void _jit_avx512_common_conv_bwd_data_kernel_f32<Vmm>::store_output(int ur_w) {
    Label no_update_label;
    const int ic_tail = jcp.ic_tail;

    mov(reg_channel, ptr[param + GET_OFF(channel)]);
    cmp(reg_channel, 0);
    je(no_update_label, T_NEAR);
    for (int k = 0; k < jcp.nb_ic_blocking; k++) {
        for (int j = 0; j < ur_w; j++) {
            Vmm vmm = vmm_out(j, k);
            size_t aux_src_offset = get_diff_src_offset(j, k);
            vaddps(vmm,
                    EVEX_compress_addr_safe(
                            reg_src, aux_src_offset, reg_long_offt));
        }
    }

    L(no_update_label);
    for (int k = 0; k < jcp.nb_ic_blocking; k++) {
        for (int j = 0; j < ur_w; j++) {
            Vmm vmm = vmm_out(j, k);
            // mask only needed for last oc_block
            if (ic_tail && k + 1 == jcp.nb_ic_blocking)
                vmm = vmm | k_ic_tail_mask;
            size_t aux_src_offset = get_diff_src_offset(j, k);
            vmovups(EVEX_compress_addr_safe(
                            reg_src, aux_src_offset, reg_long_offt),
                    vmm);
            mic_prefetcht0(EVEX_compress_addr_safe(
                    reg_src_prf, aux_src_offset, reg_long_offt));
        }
    }
}
#else
//[info]v0.12 code展開
template <typename Vmm>
void _jit_aarch64_sve_512_conv_bwd_data_kernel_f32<Vmm>::store_output(int ur_w)
{

    auto zreg_tmp = [=](){
        return xa::ZReg(31);
    };
    auto zreg_tmp_s = [=](){
        return xa::ZRegS(31);
    };

    auto zreg_out = [=](int i_ur, int i_oc){
        int idx = i_ur + i_oc * jcp.ur_w;
        assert(idx < ker_reg_base_idx);
        return xa::ZReg(idx);
    };
    auto zreg_out_s = [=](int i_ur, int i_oc){
        int idx = i_ur + i_oc * jcp.ur_w;
        assert(idx < ker_reg_base_idx);
        return xa::ZRegS(idx);
    };
    auto out_load = [=] (int aux_output_offset){
        int ofs = aux_output_offset;
        if( (VL_OFS(ofs) < LDRMAX) &&
            (VL_OFS(ofs) >= (-1 * LDRMAX)) &&
            ((ofs&0x3f) == 0)){
            CGA64::ldr(zreg_tmp(), xa::ptr(reg_src, static_cast<int32_t>(VL_OFS(ofs))));
        }else{
            CGA64::add_imm(reg_tmp_addr, reg_src, ofs, reg_tmp_imm);
            CGA64::ldr(zreg_tmp(), xa::ptr(reg_tmp_addr));
        }
    };

    auto out_str = [=](int j, int k, int aux_output_offset){
        int ofs = aux_output_offset;

        if( (VL_OFS(ofs) < LDRMAX) &&
            (VL_OFS(ofs) >= (-1 * LDRMAX)) &&
            ((ofs&0x3f) == 0)){
            CGA64::str(zreg_out(j, k), xa::ptr(reg_src, static_cast<int32_t>(VL_OFS(ofs))));
        }else{
            CGA64::add_imm(reg_tmp_addr, reg_src, ofs, reg_tmp_imm);
            CGA64::str(zreg_out(j, k), xa::ptr(reg_tmp_addr));
        }
    };


    xa::LabelAArch64 no_update_label;

    CGA64::ldr(reg_channel, xa::ptr(param, GET_OFF(channel)));
    CGA64::cmp(reg_channel, 0);
    CGA64::b(xa::EQ, no_update_label);
    for (int k = 0; k < jcp.nb_ic_blocking; k++) {
        for (int j = 0; j < ur_w; j++) {
            size_t aux_src_offset = (size_t)typesize
                * ((size_t)k * jcp.ih * jcp.iw * jcp.id + j) * jcp.ic_block;
            out_load(aux_src_offset);
            CGA64::fadd(zreg_out_s(j, k), zreg_out_s(j, k), zreg_tmp_s());
        }
    }

    CGA64::L_aarch64(no_update_label);
    for (int k = 0; k < jcp.nb_ic_blocking; k++) {
        for (int j = 0; j < ur_w; j++) {
            size_t aux_src_offset = (size_t)typesize
                * ((size_t)k * jcp.ih * jcp.iw * jcp.id + j) * jcp.ic_block;

            out_str(j, k, aux_src_offset);
        }
    }
}
#endif

#if 0
template <typename Vmm>
void _jit_avx512_common_conv_bwd_data_kernel_f32<Vmm>::compute_loop_4fma(
        int ur_w, int l_overflow, int r_overflow) {}

template <>
void _jit_avx512_common_conv_bwd_data_kernel_f32<Zmm>::compute_loop_4fma(
        int ur_w, int l_overflow, int r_overflow) {
    assert(!is_dsrc_layout_nxc() && !is_ddst_layout_nxc());
    int ow = jcp.ow;
    int kw = jcp.kw;
    int ic_block = jcp.ic_block;
    int oc_block = jcp.oc_block;
    Label kh_label, last_iter_label, loop_end_label, kd_label;
    int ker_load_number = 4;
    int shift_ker_ptr = typesize * kw * oc_block * ic_block;
    int shift_dst_ptr = typesize * ow * oc_block;
    int ii_dpref_t0 = get_iw_start(0, l_overflow);
    int iw_end_ipref = get_iw_end(ur_w, 0, r_overflow);

    bool check_last_kh = (jcp.kh > 3);
    auto kernel_offset = [=](int icb, int oc, int ki) {
        int blk_idx = icb * jcp.kh * jcp.kw * jcp.kd + ki;
        int blk_offset = blk_idx * jcp.oc_block * jcp.ic_block;
        int oc_offset = oc * jcp.oc_block;
        return typesize * (blk_offset + oc_offset);
    };
    auto kernel_loads = [=](int ki, int oc, int kk) {
        for (int ii = 0; ii < ker_load_number; ii++) {
            int aux_kernel_offset = kernel_offset(kk, oc + ii, ki);
            vmovups(vmm_ker(ii),
                    EVEX_compress_addr(aux_reg_ker, aux_kernel_offset));
        }
    };
    auto prefetch_dst_next_kh = [&](int ki, int ki_start, int cnt0, int cnt1) {
        if (cnt1 >= ker_load_number && cnt0 >= ker_load_number && ki >= ki_start
                && ii_dpref_t0 < iw_end_ipref) {
            int aux_dst_offset = typesize
                    * ((ii_dpref_t0 + jcp.l_pad) * oc_block
                            + jcp.ow * oc_block);
            prefetcht0(EVEX_compress_addr(aux_reg_dst, aux_dst_offset));
            ii_dpref_t0++;
        }
    };

    if (one_of(jcp.ndims, 3, 4)) {
        mov(aux_reg_dst, reg_dst);
        mov(aux_reg_ker, reg_ker);
        mov(aux_reg_dst_prf, reg_dst_prf);
        mov(aux_reg_ker_prf, reg_ker_prf);
    }

    if (jcp.ndims == 5) {
        push(reg_src_prf);
        push(reg_src);

        mov(reg_ki, ptr[param + GET_OFF(kd_padding)]);
        mov(aux_reg_dst_d, reg_dst);
        mov(aux_reg_ker_d, ptr[param + GET_OFF(filt)]);
        mov(aux_reg_dst_d_prf, reg_dst_prf);
        mov(aux_reg_ker_d_prf, reg_ker_prf);

        L(kd_label);
        mov(reg_kj, ptr[param + GET_OFF(kh_padding)]);
    } else {
        mov(reg_kj, reg_kh);
    }

    if (jcp.ndims == 5) {
        mov(aux_reg_dst, aux_reg_dst_d);
        mov(aux_reg_ker, aux_reg_ker_d);
        mov(aux_reg_dst_prf, aux_reg_dst_d_prf);
        mov(aux_reg_ker_prf, aux_reg_ker_d_prf);
    }

    align(16);
    L(kh_label);
    if (check_last_kh) {
        for_(int ki = 0; ki < kw; ki++)
        for_(int oc = 0; oc < oc_block; oc += 4)
        for (int kk = 0; kk < jcp.nb_ic_blocking; kk++) {
            bool last_kernel_loads = (kk == jcp.nb_ic_blocking - 1
                    && ki == kw - 1 && (oc + 4) == oc_block);

            if (last_kernel_loads) {
                cmp(reg_kj, 1);
                je(last_iter_label, T_NEAR);
            }

            kernel_loads(ki, oc, kk);
            for (int ii = get_iw_start(ki, l_overflow), prf_count_t0 = 0,
                     prf_count_t1 = 0;
                    ii < get_iw_end(ur_w, ki, r_overflow); ii++) {
                int aux_dst_offset
                        = typesize * ((ii + jcp.l_pad - ki) * oc_block + oc);
                v4fmaddps(vmm_out(ii, kk), vmm_ker(0),
                        EVEX_compress_addr(aux_reg_dst, aux_dst_offset));

                if (ii % 2) {
                    if (prf_count_t0 < 4) {
                        int aux_kernel_prf;
                        if (last_kernel_loads)
                            aux_kernel_prf
                                    = kernel_offset(0,
                                              prf_count_t0 + oc + 4 - oc_block,
                                              0)
                                    + typesize * kw * oc_block * ic_block;
                        else
                            aux_kernel_prf = kernel_offset(
                                    kk, oc + 4 + prf_count_t0, ki);
                        mic_prefetcht0(EVEX_compress_addr(
                                aux_reg_ker, aux_kernel_prf));
                        prf_count_t0++;
                    } else if (prf_count_t1 < 4) {
                        mic_prefetcht1(EVEX_compress_addr(aux_reg_ker_prf,
                                kernel_offset(kk, oc + prf_count_t1, ki)));
                        prf_count_t1++;
                    }
                } else
                    prefetch_dst_next_kh(ki, 2, prf_count_t0, prf_count_t1);
            }
            if (last_kernel_loads) {
                jmp(loop_end_label, T_NEAR);

                L(last_iter_label);

                kernel_loads(ki, oc, kk);
                for (int ii = get_iw_start(ki, l_overflow), prf_count_t0 = 0,
                         prf_count_t1 = 0;
                        ii < get_iw_end(ur_w, ki, r_overflow); ii++) {
                    int aux_dst_offset = typesize
                            * ((ii + jcp.l_pad - ki) * oc_block + oc);
                    v4fmaddps(vmm_out(ii, kk), vmm_ker(0),
                            EVEX_compress_addr(aux_reg_dst, aux_dst_offset));
                    if (ii % 2) {
                        if (prf_count_t0 < 4) {
                            mic_prefetcht0(EVEX_compress_addr(aux_reg_ker_prf,
                                    kernel_offset(0, prf_count_t0, 0)));
                            prf_count_t0++;
                        } else if (prf_count_t1 < 4) {
                            mic_prefetcht1(EVEX_compress_addr(aux_reg_ker_prf,
                                    kernel_offset(kk, oc + prf_count_t1, ki)));
                            prf_count_t1++;
                        }
                    }
                }
                L(loop_end_label);
            }
        }
    } else {
        for_(int ki = 0; ki < kw; ki++)
        for_(int oc = 0; oc < oc_block; oc += 4)
        for (int kk = 0; kk < jcp.nb_ic_blocking; kk++) {
            kernel_loads(ki, oc, kk);

            for (int ii = get_iw_start(ki, l_overflow), prf_count_t1 = 0;
                    ii < get_iw_end(ur_w, ki, r_overflow); ii++) {
                int aux_dst_offset
                        = typesize * ((ii + jcp.l_pad - ki) * oc_block + oc);
                v4fmaddps(vmm_out(ii, kk), vmm_ker(0),
                        EVEX_compress_addr(aux_reg_dst, aux_dst_offset));
                if ((ii % 2) && (prf_count_t1 < 4)) {
                    mic_prefetcht1(EVEX_compress_addr(aux_reg_ker_prf,
                            kernel_offset(kk, oc + prf_count_t1, ki)));
                    prf_count_t1++;
                }
                if (ki == 1 && oc == 0 && kk == 0)
                    mic_prefetcht1(EVEX_compress_addr(
                            aux_reg_dst_prf, aux_dst_offset));
            }
        }
    }

    add(aux_reg_ker, shift_ker_ptr);
    sub(aux_reg_dst, shift_dst_ptr);
    add(aux_reg_ker_prf, shift_ker_ptr);
    sub(aux_reg_dst_prf, shift_dst_ptr);

    dec(reg_kj);
    cmp(reg_kj, 0);
    jg(kh_label, T_NEAR);

    if (jcp.ndims == 5) {
        sub(aux_reg_dst_d, typesize * (jcp.oh * ow) * ic_block);
        add(aux_reg_ker_d, typesize * jcp.kw * jcp.kh * oc_block * ic_block);
        sub(aux_reg_dst_d_prf, typesize * (jcp.oh * ow) * ic_block);
        add(aux_reg_ker_d_prf,
                typesize * jcp.kw * jcp.kh * oc_block * ic_block);

        dec(reg_ki);
        cmp(reg_ki, 0);
        jg(kd_label, T_NEAR);

        pop(reg_src);
        pop(reg_src_prf);
    }
}
#endif

template <typename Vmm>
#if 0
void _jit_avx512_common_conv_bwd_data_kernel_f32<Vmm>::compute_loop_fma(
        int ur_w, int l_overflow, int r_overflow) {
    Label kh_label, kd_label;
    int kw = jcp.kw;
    int ow = jcp.ow;

    int ic_block = jcp.ic_block;
    int oc_block = jcp.oc_block;
    int stride_w = jcp.stride_w;
    int stride_h = jcp.stride_h;

    int ker_pipeline_depth = 4;
    assert(ker_reg_base_idx + ker_pipeline_depth <= 32);
    assert(oc_block >= ker_pipeline_depth);

    int num_ker_loads = oc_block * kw;
    int num_inp_prfs
            = ur_w * nstl::min(kw, stride_w) + nstl::max(0, kw - stride_w);
    int num_prfs = num_ker_loads + num_inp_prfs;
    int num_fmas = num_ker_loads * ur_w / stride_w;
    int prf_inst_spacing = nstl::max(1, num_fmas / num_prfs);
    int prf_inst_trigger = (num_fmas % prf_inst_spacing) / 2;
    const bool ddst_layout_nxc = is_ddst_layout_nxc();
    int oc_mult = ddst_layout_nxc ? jcp.ngroups * jcp.oc : oc_block;
    const bool prf_ker = mayiuse(avx512_mic);
    const bool prf_dsrc = mayiuse(avx512_mic);
    const bool prf_ddst = mayiuse(avx512_mic);
    const bool ocb_loop_in_compute_function = ddst_layout_nxc;
    // reg_channel in ocb loop is the same as aux_reg_dst_prf
    assert(IMPLICATION(prf_ddst, !ocb_loop_in_compute_function));

    const int ic_tail = jcp.ic_tail;
    const int oc_tail = jcp.oc_tail;
    const int max_filter_size = 20;
    Label oc_tail_jmp[max_filter_size];
    if (ic_tail || oc_tail) ker_pipeline_depth = 1;

    if (one_of(jcp.ndims, 3, 4)) {
        mov(aux_reg_dst, reg_dst);
        mov(aux_reg_ker, reg_ker);
        if (prf_ddst) mov(aux_reg_dst_prf, reg_dst_prf);
        if (prf_ker) mov(aux_reg_ker_prf, reg_ker_prf);
    }

    if (jcp.ndims == 5) {
        if (prf_dsrc) push(reg_src_prf);
        push(reg_src);

        mov(reg_ki, ptr[param + GET_OFF(kd_padding)]);
        mov(aux_reg_dst_d, reg_dst);
        if (ocb_loop_in_compute_function) {
            // need to continue with the same kernel pointer, but as
            // aux_reg_ker_d == reg_ker we need to save its value and restore
            // it after kd loop
            assert(aux_reg_ker_d == reg_ker);
            push(aux_reg_ker_d);
        } else
            mov(aux_reg_ker_d, ptr[param + GET_OFF(filt)]);

        if (prf_ddst) mov(aux_reg_dst_d_prf, reg_dst_prf);
        if (prf_ker) mov(aux_reg_ker_d_prf, reg_ker_prf);

        L(kd_label);
        mov(reg_kj, ptr[param + GET_OFF(kh_padding)]);
    } else {
        mov(reg_kj, reg_kh);
    }

    if (jcp.ndims == 5) {
        mov(aux_reg_dst, aux_reg_dst_d);
        mov(aux_reg_ker, aux_reg_ker_d);
        if (prf_ddst) mov(aux_reg_dst_prf, aux_reg_dst_d_prf);
        if (prf_ker) mov(aux_reg_ker_prf, aux_reg_ker_d_prf);
    }

    L(kh_label);
    {
        int step = 0;
        int ker_prfs = 0;
        for (int ki = 0; ki < kw; ki++) {
            for (int oc = 0; oc < oc_block; oc++) {
                if (oc_tail && oc >= oc_tail) {
                    // if src has only tails to compute, skip early
                    if (jcp.oc == oc_tail)
                        break;
                    else if (oc == oc_tail) {
                        cmp(reg_channel, oc_tail);
                        je(oc_tail_jmp[ki], T_NEAR);
                    }
                }
                if (step == 0) {
                    for (int i = 0; i < ker_pipeline_depth; i++) {
                        int aux_kernel_offset = typesize
                                * ((oc + i) * oc_block
                                        + ki * ic_block * oc_block);
                        vmovups(vmm_ker(i),
                                EVEX_compress_addr(
                                        aux_reg_ker, aux_kernel_offset));
                    }
                } else if (step < num_ker_loads - ker_pipeline_depth + 1) {
                    int load_offset = ker_pipeline_depth - 1;
                    int ker_load_reg_idx
                            = (step + load_offset) % ker_pipeline_depth;
                    int aux_kernel_offset = typesize
                            * ((oc + load_offset) * oc_block
                                    + ki * ic_block * oc_block);
                    vmovups(vmm_ker(ker_load_reg_idx),
                            EVEX_compress_addr(aux_reg_ker, aux_kernel_offset));
                }

                bool ker_prf_inserted = false;
                auto vmm_kernel = vmm_ker(step % ker_pipeline_depth);

                int jj_start = get_iw_start(ki, l_overflow);
                int jj_end = get_iw_end(ur_w, ki, r_overflow);
                const int dil_w = jcp.dilate_w + 1;
                const int ref_jj_start
                        = nstl::max(0, l_overflow - (kw - 1 - ki) * dil_w);
                const int ref_jj_end
                        = ur_w - nstl::max(0, r_overflow - ki * dil_w);
                assert(IMPLICATION(stride_w == 1,
                        jj_start == ref_jj_start && jj_end == ref_jj_end));
                UNUSED(dil_w);
                UNUSED(ref_jj_start);
                UNUSED(ref_jj_end);

                for (int jj = jj_start; jj < jj_end; jj += stride_w) {
                    assert((jj + jcp.l_pad - ki * (jcp.dilate_w + 1)) % stride_w
                            == 0);
                    int aux_dst_offset = get_dst_offset(jj, oc, ki);
                    vfmadd231ps(vmm_out(jj, 0), vmm_kernel,
                            EVEX_compress_addr(
                                    aux_reg_dst, aux_dst_offset, true));

                    int fma_idx = (step * ur_w + jj) / stride_w;
                    int prf_slot_idx = fma_idx / prf_inst_spacing;
                    if (fma_idx % prf_inst_spacing == prf_inst_trigger) {
                        if (prf_ker && !ker_prf_inserted
                                && ker_prfs < num_ker_loads) {
                            int ker_prf_offset
                                    = typesize * ker_prfs * jcp.oc_block;
                            mic_prefetcht1(EVEX_compress_addr(
                                    aux_reg_ker_prf, ker_prf_offset));
                            ker_prf_inserted = true;
                            ker_prfs++;
                        } else if (prf_ddst) {
                            int inp_prf_idx = prf_slot_idx - ker_prfs;
                            if (inp_prf_idx < num_inp_prfs) {
                                int inp_prf_offset = ic_block * typesize
                                        * ((inp_prf_idx / kw) * kw
                                                + (inp_prf_idx % kw));
                                mic_prefetcht0(EVEX_compress_addr(
                                        aux_reg_dst_prf, inp_prf_offset));
                            }
                        }
                    }
                }
                step++;
            }
            L(oc_tail_jmp[ki]);
        }

        const int ker_shift = typesize * stride_h * kw * oc_block * ic_block;
        add(aux_reg_ker, ker_shift);
        const int ddst_shift = typesize * (jcp.dilate_h + 1) * ow * oc_mult;
        sub(aux_reg_dst, ddst_shift);
        if (prf_ker) add(aux_reg_ker_prf, ker_shift);
        if (prf_ddst) sub(aux_reg_dst_prf, ddst_shift);

        dec(reg_kj);
        cmp(reg_kj, 0);
        jg(kh_label, T_NEAR);
    }
    if (jcp.ndims == 5) {
        const int depth_ddst_shift
                = typesize * (jcp.dilate_d + 1) * jcp.oh * ow * oc_mult;
        sub(aux_reg_dst_d, depth_ddst_shift);
        const int depth_ker_shift = typesize * jcp.stride_d * jcp.kw * jcp.kh
                * oc_block * ic_block;
        add(aux_reg_ker_d, depth_ker_shift);
        if (prf_ddst) sub(aux_reg_dst_d_prf, depth_ddst_shift);
        if (prf_ker) add(aux_reg_ker_d_prf, depth_ker_shift);

        dec(reg_ki);
        cmp(reg_ki, 0);
        jg(kd_label, T_NEAR);
        if (ocb_loop_in_compute_function) pop(aux_reg_ker_d);
    }

    if (jcp.ndims == 5) {
        pop(reg_src);
        if (prf_dsrc) pop(reg_src_prf);
    }
}
#else
//[info]v0.21 code展開
void _jit_aarch64_sve_512_conv_bwd_data_kernel_f32<Vmm>::compute_loop_fma(
        int ur_w, int l_overflow, int r_overflow)
{
    xa::LabelAArch64 kh_label, kd_label;
    int kw = jcp.kw;
    int ow = jcp.ow;

    int ic_block = jcp.ic_block;
    int oc_block = jcp.oc_block;
    int l_pad = jcp.l_pad;
    int dilate_w = jcp.dilate_w + 1;
    int stride_w = jcp.stride_w;
    int stride_h = jcp.stride_h;

    int ker_pipeline_depth = 3;
    assert(ker_reg_base_idx + ker_pipeline_depth <= 31);
    assert(oc_block >= ker_pipeline_depth);

    int num_ker_loads = oc_block * kw;


    auto zreg_ker = [=](int i_ic) {
        assert(i_ic < ker_pipeline_depth);
        assert((ker_reg_base_idx + i_ic)<31);
        return xa::ZReg(ker_reg_base_idx + i_ic);
    };
    auto zreg_ker_s = [=](int i_ic) {
        assert(i_ic < ker_pipeline_depth);
        assert((ker_reg_base_idx + i_ic)<31);
        return xa::ZRegS(ker_reg_base_idx + i_ic);
    };
    auto zreg_out_s = [=](int i_ur, int i_oc){
        int idx = (i_ur + i_oc * jcp.ur_w );
        assert(idx < ker_reg_base_idx);
        return xa::ZRegS(idx);
    };
    auto zreg_in_s = [=](int idx){
        int num_used_zreg = ker_reg_base_idx + ker_pipeline_depth;
        int zreg_idx = (idx % (32 - num_used_zreg)) + num_used_zreg;
        assert(zreg_idx <= 31);
        return xa::ZRegS(zreg_idx);
    };

    auto bcast_load = [&]( int aux_output_offset, int prev_ofs, int idx){
        int num_used_zreg = ker_reg_base_idx + ker_pipeline_depth;
        int zreg_idx = (idx % (32 - num_used_zreg)) + num_used_zreg;
        if( ((aux_output_offset & 0x3) ==0) &&
                (aux_output_offset < LDRWMAX) &&
                    (aux_output_offset >= 0)){
             CGA64::ld1rw(xa::ZRegS(zreg_idx), reg_p_all_ones,
                    xa::ptr(aux_reg_dst, static_cast<int32_t>(aux_output_offset)));
        }else{
            int ofs;
            ofs = aux_output_offset - prev_ofs;
            if( ((ofs & 0x3) ==0) && (ofs < LDRWMAX) && (ofs >= 0)){

                CGA64::ld1rw(xa::ZRegS(zreg_idx), reg_p_all_ones,
                    xa::ptr(reg_prev_bcast_addr, static_cast<int32_t>(ofs)));
            }else{
                ofs = aux_output_offset;
                CGA64::add_imm(reg_prev_bcast_addr, aux_reg_dst, ofs, reg_tmp_imm);

                CGA64::ld1rw(xa::ZRegS(zreg_idx), reg_p_all_ones, xa::ptr(reg_prev_bcast_addr));
                prev_ofs = ofs;
            }
        }
        return prev_ofs;
    };
    auto ker_load = [=]( int i, int aux_kernel_offset){
        int ofs = aux_kernel_offset;

        if( (VL_OFS(ofs) < LDRMAX) &&
            (VL_OFS(ofs) >= (-1 * LDRMAX)) &&
            ((ofs&0x3f) == 0)){
            CGA64::ldr(zreg_ker(i), xa::ptr(aux_reg_ker, static_cast<int32_t>(VL_OFS(ofs))));
        }else{
            CGA64::add_imm(reg_tmp_addr, aux_reg_ker, ofs, reg_tmp_imm);
            CGA64::ldr(zreg_ker(i), xa::ptr(reg_tmp_addr));
        }

    };



    if (one_of(jcp.ndims, 3, 4)) {
        CGA64::mov(aux_reg_dst, reg_dst);
        CGA64::mov(aux_reg_ker, reg_ker);

        CGA64::mov(aux_reg_dst_prf, reg_dst_prf);
        CGA64::mov(aux_reg_ker_prf, reg_ker_prf);
    }

    if (jcp.ndims == 5) {
        CGA64::mov(reg_src_prf_org, reg_src_prf);
        CGA64::mov(reg_src_org, reg_src);

        CGA64::ldr(reg_ki, xa::ptr(param , GET_OFF(kd_padding)));
        CGA64::mov(aux_reg_dst_d, reg_dst);
        CGA64::ldr(aux_reg_ker_d, xa::ptr(param, GET_OFF(filt)));
        CGA64::mov(aux_reg_dst_d_prf, reg_dst_prf);
        CGA64::mov(aux_reg_ker_d_prf, reg_ker_prf);

        CGA64::L_aarch64(kd_label);
        CGA64::ldr(reg_kj, xa::ptr(param, GET_OFF(kh_padding)));
    } else {
        CGA64::mov(reg_kj, reg_kh);
    }

    if (jcp.ndims == 5) {
        CGA64::mov(aux_reg_dst, aux_reg_dst_d);
        CGA64::mov(aux_reg_ker, aux_reg_ker_d);
        CGA64::mov(aux_reg_dst_prf, aux_reg_dst_d_prf);
        CGA64::mov(aux_reg_ker_prf, aux_reg_ker_d_prf);
    }
    int prev_ofs = 0;
    CGA64::L_aarch64(kh_label); {
        int step = 0;
        for (int ki = 0; ki < kw; ki++) {
            for (int oc = 0; oc < oc_block; oc++) {
                if (step == 0) {
                    for (int i = 0; i < ker_pipeline_depth; i++) {
                        int aux_kernel_offset = typesize * ((oc + i) * oc_block
                                + ki * ic_block * oc_block);
                        ker_load(i, aux_kernel_offset);
                    }
                } else if (step < num_ker_loads - ker_pipeline_depth + 1) {
                    int load_offset = ker_pipeline_depth - 1;
                    int ker_load_reg_idx
                        = (step + load_offset) % ker_pipeline_depth;
                    int aux_kernel_offset = typesize * ((oc + load_offset)
                            * oc_block + ki * ic_block * oc_block);
                    ker_load(ker_load_reg_idx, aux_kernel_offset);
                }

                auto zreg_kernel_s = zreg_ker_s(step % ker_pipeline_depth);

                int jj_start = get_iw_start(ki, l_overflow);
                int jj_end = get_iw_end(ur_w, ki, r_overflow);
                assert(stride_w != 1
                        || jj_start == nstl::max(0,
                            l_overflow - (kw - 1 - ki) * dilate_w));
                assert(stride_w != 1
                        || jj_end == ur_w - nstl::max(0,
                            r_overflow - ki * dilate_w));

                for (int jj = jj_start; jj < jj_end; jj += stride_w) {
                    assert((jj + l_pad - ki * dilate_w) % stride_w == 0);
                    int aux_dst_offset = typesize *
                        (((jj + l_pad - ki * dilate_w)
                                / stride_w) * jcp.oc_block + oc);
                    prev_ofs = bcast_load(aux_dst_offset, prev_ofs, jj);
                    CGA64::fmla(zreg_out_s(jj, 0), reg_p_all_ones,
                            zreg_kernel_s, zreg_in_s(jj));


                }
                step++;
            }
        }

        CGA64::add_imm(aux_reg_ker, aux_reg_ker, typesize * stride_h * kw * oc_block * ic_block, reg_tmp_imm);
        CGA64::sub_imm(aux_reg_dst, aux_reg_dst, typesize * (jcp.dilate_h + 1) * ow * oc_block, reg_tmp_imm);
        CGA64::add_imm(aux_reg_ker_prf, aux_reg_ker_prf,
                    typesize * stride_h * kw * oc_block * ic_block, reg_tmp_imm);
        CGA64::sub_imm(aux_reg_dst_prf, aux_reg_dst_prf,
                    typesize * (jcp.dilate_h + 1) * ow * oc_block, reg_tmp_imm);
        CGA64::sub(reg_kj, reg_kj, 1);
        CGA64::cmp(reg_kj, 0);
        CGA64::b(xa::GT, kh_label); //jg(kh_label, T_NEAR);
    }
    if (jcp.ndims == 5) {
        CGA64::sub_imm(aux_reg_dst_d, aux_reg_dst_d,
                typesize * (jcp.dilate_d + 1) * jcp.oh * ow * ic_block, reg_tmp_imm);
        CGA64::add_imm(aux_reg_ker_d, aux_reg_ker_d, typesize * jcp.stride_d * jcp.kw * jcp.kh
                * oc_block * ic_block, reg_tmp_imm);
        CGA64::sub_imm(aux_reg_dst_d_prf, aux_reg_dst_d_prf,
                typesize * (jcp.dilate_d + 1) * jcp.oh * ow * ic_block, reg_tmp_imm);
        CGA64::add_imm(aux_reg_ker_d_prf, aux_reg_ker_d_prf,
                typesize * jcp.stride_d * jcp.kw * jcp.kh * oc_block * ic_block, reg_tmp_imm);

        CGA64::sub(reg_ki, reg_ki, 1);
        CGA64::cmp(reg_ki, 0);
        CGA64::b(xa::GT, kd_label); //jg(kd_label, T_NEAR);
    }

    if (jcp.ndims == 5)
    {
        CGA64::mov(reg_src, reg_src_org);
        CGA64::mov(reg_src_prf, reg_src_prf_org);
    }
}
#endif

template <typename Vmm>
#if 0
void _jit_avx512_common_conv_bwd_data_kernel_f32<Vmm>::compute_loop_fma_core(
        int ur_w, int l_overflow, int r_overflow, int k_offset) {
    int kw = jcp.kw;
    int ow = jcp.ow;
    int stride_w = jcp.stride_w;
    int ic_block = jcp.ic_block;
    int oc_block = jcp.oc_block;
    int nb_ic_block = jcp.nb_ic_blocking;
    Label kh_label, kd_label;

    const bool ddst_layout_nxc = is_ddst_layout_nxc();
    int shift_ker_ptr = typesize * kw * oc_block * ic_block;
    int oc_mult = ddst_layout_nxc ? jcp.ngroups * jcp.oc : oc_block;
    int shift_dst_ptr = typesize * (jcp.dilate_h + 1) * ow * oc_mult;

    const int oc_tail = jcp.oc_tail;
    const int max_filter_size = 20;
    Label oc_tail_jmp[max_filter_size];

    auto kernel_offset = [=](int icb, int oc, int ki) {
        int blk_idx = icb * jcp.kh * jcp.kw * jcp.kd + ki;
        int blk_offset = blk_idx * jcp.oc_block * jcp.ic_block;
        int oc_offset = oc * jcp.oc_block;
        return typesize * (blk_offset + oc_offset);
    };

    if (one_of(jcp.ndims, 3, 4)) {
        mov(aux_reg_dst, reg_dst);
        mov(aux_reg_ker, reg_ker);
    }

    const bool ocb_loop_in_compute_function = ddst_layout_nxc;
    if (jcp.ndims == 5) {
        push(reg_src);

        mov(reg_ki, ptr[param + GET_OFF(kd_padding)]);
        mov(aux_reg_dst_d, reg_dst);
        if (ocb_loop_in_compute_function) {
            // need to continue with the same kernel pointer, but as
            // aux_reg_ker_d == reg_ker we need to save its value and restore
            // it after kd loop
            assert(aux_reg_ker_d == reg_ker);
            push(aux_reg_ker_d);
        } else
            mov(aux_reg_ker_d, ptr[param + GET_OFF(filt)]);

        L(kd_label);
        mov(reg_kj, ptr[param + GET_OFF(kh_padding)]);
    } else {
        mov(reg_kj, reg_kh);
    }

    if (jcp.ndims == 5) {
        mov(aux_reg_dst, aux_reg_dst_d);
        mov(aux_reg_ker, aux_reg_ker_d);
    }

    L(kh_label);
    {
        for (int ki = 0; ki < kw; ki++) {
            int jj_start = get_iw_start(ki, l_overflow);
            int jj_end = get_iw_end(ur_w, ki, r_overflow);
            for (int oc = 0; oc < oc_block; oc++) {
                if (oc_tail && oc >= oc_tail) {
                    // if src has only tails to compute, skip early
                    if (jcp.oc == oc_tail)
                        break;
                    else if (oc == oc_tail) {
                        cmp(reg_channel, oc_tail);
                        je(oc_tail_jmp[ki], T_NEAR);
                    }
                }
                if (jcp.kernel_kind == expl_bcast) {
                    for (int jj = jj_start; jj < jj_end; jj++) {
                        int aux_output_offset = get_dst_offset(jj, oc, ki);
                        vbroadcastss(vmm_inp(jj, nb_ic_block),
                                ptr[aux_reg_dst + aux_output_offset]);
                    }
                }
                for (int ii = 0; ii < nb_ic_block; ii++) {
                    int aux_kernel_offset
                            = kernel_offset(ii, oc, ki + k_offset);
                    if (jj_end - jj_start > 0)
                        vmovups(vmm_wei,
                                EVEX_compress_addr(
                                        aux_reg_ker, aux_kernel_offset));
                    for (int jj = jj_start; jj < jj_end; jj += stride_w)
                        if (jcp.kernel_kind == expl_bcast)
                            vfmadd231ps(vmm_out(jj, ii),
                                    vmm_inp(jj, nb_ic_block), vmm_wei);
                        else
                            vfmadd231ps(vmm_out(jj, ii), vmm_wei,
                                    EVEX_compress_addr(aux_reg_dst,
                                            get_dst_offset(jj, oc, ki), true));
                }
            }
            L(oc_tail_jmp[ki]);
        }
        add(aux_reg_ker, shift_ker_ptr);
        sub(aux_reg_dst, shift_dst_ptr);
        dec(reg_kj);
        cmp(reg_kj, 0);
        jg(kh_label, T_NEAR);
    }

    if (jcp.ndims == 5) {
        sub(aux_reg_dst_d,
                typesize * (jcp.dilate_d + 1) * jcp.oh * ow * oc_mult);
        add(aux_reg_ker_d, typesize * jcp.kw * jcp.kh * oc_block * ic_block);

        dec(reg_ki);
        cmp(reg_ki, 0);
        jg(kd_label, T_NEAR);

        if (ocb_loop_in_compute_function) pop(aux_reg_ker_d);
        pop(reg_src);
    }
}
#else
//[info]v0.21 code展開
void _jit_aarch64_sve_512_conv_bwd_data_kernel_f32<Vmm>::compute_loop_fma_core(
        int ur_w, int l_overflow, int r_overflow, int k_offset)
{
    int kw = jcp.kw;
    int ow = jcp.ow;
    int stride_w = jcp.stride_w;
    int ic_block = jcp.ic_block;
    int oc_block = jcp.oc_block;
    int nb_ic_block = jcp.nb_ic_blocking;
    xa::LabelAArch64 kh_label, kd_label;

    const bool ddst_layout_nxc = is_ddst_layout_nxc();
    int shift_ker_ptr = typesize * kw * oc_block * ic_block;
    int oc_mult = ddst_layout_nxc ? jcp.ngroups * jcp.oc : oc_block;
    int shift_dst_ptr = typesize * (jcp.dilate_h + 1) * ow * oc_mult;

    const int oc_tail = jcp.oc_tail;
    const int max_filter_size = 20;
    xa::LabelAArch64 oc_tail_jmp[max_filter_size];

    auto kernel_offset = [=](int icb, int oc, int ki) {
        int blk_idx = icb * jcp.kh * jcp.kw * jcp.kd + ki;
        int blk_offset = blk_idx * jcp.oc_block * jcp.ic_block;
        int oc_offset = oc * jcp.oc_block;
        return typesize * (blk_offset + oc_offset);
    };

    auto zreg_inp_s = [=](int i_ic, int nb_x_blocking){
        int idx = ( i_ic + nb_x_blocking * jcp.ur_w );
        assert(idx < 31);
        return xa::ZRegS(idx);
    };
    auto zreg_out_s = [=](int i_ur, int i_oc){
        int idx = (i_ur + i_oc * jcp.ur_w );
        assert(idx < ker_reg_base_idx);
        return xa::ZRegS(idx);
    };
    auto zreg_wei = [=](){
        return xa::ZReg(31);
    };
    auto zreg_wei_s = [=](){
        return xa::ZRegS(31);
    };


    auto bcast_load = [&](int jj, int nb_oc_block, int aux_output_offset, int prev_ofs, int jj_end){
        if( ((aux_output_offset & 0x3) ==0) &&
                (aux_output_offset < LDRWMAX) &&
                    (aux_output_offset >= 0)){
            CGA64::ld1rw(zreg_inp_s(jj, nb_oc_block), reg_p_all_ones,
                    xa::ptr(aux_reg_dst, static_cast<int32_t>(aux_output_offset)));
        }else{
            if( (prev_ofs > -1) &&
                ((aux_output_offset - prev_ofs)>0) &&
                ((aux_output_offset - prev_ofs) < LDRWMAX) &&
                (((aux_output_offset - prev_ofs)& 0x3) ==0)){

                CGA64::ld1rw(zreg_inp_s(jj, nb_oc_block), reg_p_all_ones,
                        xa::ptr(reg_prev_bcast_addr, static_cast<int32_t>(aux_output_offset - prev_ofs)));

            }else{
                int ofs;
                if((prev_ofs > -1) && ((aux_output_offset - prev_ofs)>0)){
                    ofs = aux_output_offset - prev_ofs;
                    CGA64::add_imm(reg_prev_bcast_addr, reg_prev_bcast_addr, ofs, reg_tmp_imm);

                }else{
                    ofs = aux_output_offset;
                    CGA64::add_imm(reg_prev_bcast_addr, aux_reg_dst, ofs, reg_tmp_imm);
                }

                CGA64::ld1rw(zreg_inp_s(jj, nb_oc_block), reg_p_all_ones, xa::ptr(reg_prev_bcast_addr));
                prev_ofs = aux_output_offset;
            }
        }
        return prev_ofs;
    };

    auto bcast_load_30 = [&](int jj, int nb_oc_block, int aux_output_offset, int prev_ofs, int jj_end){
        if( ((aux_output_offset & 0x3) ==0) &&
                (aux_output_offset < LDRWMAX) &&
                    (aux_output_offset >= 0)){
            CGA64::ld1rw(zreg_inp_s(jj/stride_w, nb_oc_block), reg_p_all_ones,
                    xa::ptr(aux_reg_dst, static_cast<int32_t>(aux_output_offset)));
        }else{
            if( (prev_ofs > -1) &&
                ((aux_output_offset - prev_ofs)>0) &&
                ((aux_output_offset - prev_ofs) < LDRWMAX) &&
                (((aux_output_offset - prev_ofs)& 0x3) ==0)){

                CGA64::ld1rw(zreg_inp_s(jj/stride_w, nb_oc_block), reg_p_all_ones,
                        xa::ptr(reg_prev_bcast_addr, static_cast<int32_t>(aux_output_offset - prev_ofs)));

            }else{
                int ofs;
                if((prev_ofs > -1) && ((aux_output_offset - prev_ofs)>0)){
                    ofs = aux_output_offset - prev_ofs;
                    CGA64::add_imm(reg_prev_bcast_addr, reg_prev_bcast_addr, ofs, reg_tmp_imm);

                }else{
                    ofs = aux_output_offset;
                    CGA64::add_imm(reg_prev_bcast_addr, aux_reg_dst, ofs, reg_tmp_imm);
                }

                CGA64::ld1rw(zreg_inp_s(jj/stride_w, nb_oc_block), reg_p_all_ones, xa::ptr(reg_prev_bcast_addr));
                prev_ofs = aux_output_offset;
            }
        }
        return prev_ofs;
    };

    auto wei_load = [=](int aux_kernel_offset){
        int ofs = aux_kernel_offset;

        if( (VL_OFS(ofs) < LDRMAX) &&
            (VL_OFS(ofs) >= (-1 * LDRMAX)) ){
            CGA64::ldr(zreg_wei(), xa::ptr(aux_reg_ker, static_cast<int32_t>(VL_OFS(ofs))));
        }else{
            CGA64::add_imm(reg_tmp_addr, aux_reg_ker, ofs, reg_tmp_imm);
            CGA64::ldr(zreg_wei(), xa::ptr(reg_tmp_addr));
        }

    };

    if (one_of(jcp.ndims, 3, 4)) {
        CGA64::mov(aux_reg_dst, reg_dst);
        CGA64::mov(aux_reg_ker, reg_ker);
    }

    if (jcp.ndims == 5) {
        CGA64::mov(reg_src_prf_org, reg_src_prf);
        CGA64::mov(reg_src_org, reg_src);

        CGA64::ldr(reg_ki, xa::ptr(param, GET_OFF(kd_padding)));
        CGA64::mov(aux_reg_dst_d, reg_dst);
        CGA64::ldr(aux_reg_ker_d, xa::ptr(param, GET_OFF(filt)));

        CGA64::L_aarch64(kd_label);
        CGA64::ldr(reg_kj, xa::ptr(param, GET_OFF(kh_padding)));
    } else {
        CGA64::mov(reg_kj, reg_kh);
    }

    if (jcp.ndims == 5) {
        CGA64::mov(aux_reg_dst, aux_reg_dst_d);
        CGA64::mov(aux_reg_ker, aux_reg_ker_d);
    }

    CGA64::L_aarch64(kh_label);
    {
        for (int ki = 0; ki < kw; ki++) { // kernel width
            int prev_ofs = -1;

            int jj_start = get_iw_start(ki, l_overflow);
            int jj_end = get_iw_end(ur_w, ki, r_overflow);
            for (int oc = 0; oc < oc_block; oc++) {
                if (oc_tail && oc >= oc_tail) {
                    // if src has only tails to compute, skip early
                    if (jcp.oc == oc_tail)
                        break;
                    else if (oc == oc_tail) {
                        CGA64::cmp(reg_channel, oc_tail);
                        CGA64::b(xa::NE, oc_tail_jmp[ki]);
                    }
                }
                if (stride_w == 1) {
                    for (int jj = jj_start; jj < jj_end; jj += 1) {
                        int aux_output_offset = get_dst_offset(jj, oc, ki);
                        prev_ofs = bcast_load(jj, nb_ic_block, aux_output_offset, prev_ofs, jj_end);
                    }
                }
                for (int ii = 0; ii < nb_ic_block; ii++) {
                    int aux_kernel_offset = kernel_offset(ii, oc, ki + k_offset);
                    if (jj_end - jj_start > 0){
                        wei_load(aux_kernel_offset);
                    }
                    for (int jj = jj_start; jj < jj_end; jj += stride_w){
                        if(stride_w == 1){
                            CGA64::fmla(zreg_out_s(jj, ii), reg_p_all_ones,
                                        zreg_inp_s(jj, nb_ic_block), zreg_wei_s());
                        }else{
                            int aux_output_offset = get_dst_offset(jj, oc, ki);
                            prev_ofs = bcast_load_30(jj, nb_ic_block, aux_output_offset, prev_ofs, jj_end);

                            CGA64::fmla(zreg_out_s(jj, ii), reg_p_all_ones,
                                        zreg_inp_s(jj%stride_w, nb_ic_block), zreg_wei_s());
                                        //xa::ZRegS(30), zreg_wei_s());

                        }
                    }

                }
            }
            CGA64::L_aarch64(oc_tail_jmp[ki]);
        }
        CGA64::add_imm(aux_reg_ker, aux_reg_ker, shift_ker_ptr, reg_tmp_imm);
        assert(shift_dst_ptr >= 0);
        CGA64::sub_imm(aux_reg_dst, aux_reg_dst, shift_dst_ptr, reg_tmp_imm);
        CGA64::sub(reg_kj, reg_kj, 1);
        CGA64::cmp(reg_kj, 0);
        CGA64::b(xa::GT, kh_label);
    }

    if (jcp.ndims == 5) {
        CGA64::sub_imm(aux_reg_dst_d, aux_reg_dst_d,
                typesize * (jcp.dilate_d + 1) * jcp.oh * ow * ic_block, reg_tmp_imm);
        CGA64::add_imm(aux_reg_ker_d, aux_reg_ker_d,  typesize * jcp.kw * jcp.kh * oc_block * ic_block, reg_tmp_imm);

        CGA64::sub(reg_ki, reg_ki, 1);
        CGA64::cmp(reg_ki, 0);
        CGA64::b(xa::GT, kd_label);

        CGA64::mov(reg_src, reg_src_org);
        CGA64::mov(reg_src_prf, reg_src_prf_org);
    }
}
#endif

template <typename Vmm>
inline void _jit_aarch64_sve_512_conv_bwd_data_kernel_f32<Vmm>::compute_loop(
        int ur_w, int l_overflow, int r_overflow, int k_offset) {
    if (jcp.ndims == 5) CGA64::mov(reg_oi_org, reg_oi);

    prepare_output(ur_w);

    xa::LabelAArch64 skip_compute_loop;
    if (jcp.ndims == 5) {
        CGA64::ldr(reg_kj, xa::ptr(param, GET_OFF(kd_padding)));
        CGA64::cmp(reg_kj, 0);
        CGA64::b(xa::LE, skip_compute_loop);
    }
    CGA64::ldr(reg_kj, xa::ptr(param, GET_OFF(kh_padding)));
    CGA64::cmp(reg_kj, 0);
    CGA64::b(xa::LE, skip_compute_loop);

    const bool generate_ocb_loop = jcp.nb_oc > 1 && is_ddst_layout_nxc();
    xa::LabelAArch64 oc_loop;
    if (generate_ocb_loop) {
        CGA64::mov(reg_dst_org, reg_dst);
        CGA64::mov(reg_ker_org, reg_ker);

        CGA64::ldr(reg_channel, xa::ptr(param, GET_OFF(reduce_work)));
        CGA64::L_aarch64(oc_loop);
    }

    if (jcp.ver == ver_fma)
        if ( jcp.nb_ic_blocking == 1)
            compute_loop_fma(ur_w, l_overflow, r_overflow);
        else
            compute_loop_fma_core(ur_w, l_overflow, r_overflow, k_offset);

    else
        assert("!unknown convolution version");

    if (generate_ocb_loop) {
        CGA64::add_imm(reg_dst, reg_dst, jcp.oc_block * typesize, reg_tmp_imm);
        const int ker_shift = jcp.nb_ic * jcp.kd * jcp.kh * jcp.kw
                * jcp.ic_block * jcp.oc_block * typesize;
        CGA64::add_imm(reg_ker, reg_ker, ker_shift, reg_tmp_imm);
        CGA64::sub_imm(reg_channel, reg_channel, jcp.oc_block, reg_tmp_imm);
        CGA64::b(xa::GT, oc_loop);

        CGA64::mov(reg_ker, reg_ker_org);
        CGA64::mov(reg_dst, reg_dst_org);
    }

    CGA64::L_aarch64(skip_compute_loop);
    store_output(ur_w);
    if (jcp.ndims == 5) CGA64::mov(reg_oi, reg_oi_org);
}

template <typename Vmm>
void _jit_aarch64_sve_512_conv_bwd_data_kernel_f32<Vmm>::generate() {
    int iw = jcp.iw;
    int kw = jcp.kw;
    int ur_w = jcp.ur_w;
    int ic_block = jcp.ic_block;
    int oc_block = jcp.oc_block;
    int nb_iw = jcp.nb_iw;
    int iw_block = jcp.iw_block;
    int ur_w_tail = jcp.ur_w_tail;
    int dilate_w = jcp.dilate_w + 1;
    int stride_w = jcp.stride_w;

    int dst_shift = jcp.typesize_in * (ur_w / stride_w)
            * (is_ddst_layout_nxc() ? jcp.ngroups * jcp.oc : oc_block);
    int src_shift = jcp.typesize_out * ur_w
            * (is_dsrc_layout_nxc() ? jcp.ngroups * jcp.ic : ic_block);

    preamble();
    CGA64::ptrue( reg_p_all_ones.b );

    CGA64::ldr(reg_src, xa::ptr(param, GET_OFF(src)));
    CGA64::ldr(reg_dst, xa::ptr(param, GET_OFF(dst)));
    CGA64::ldr(reg_ker, xa::ptr(param, GET_OFF(filt)));

    CGA64::ldr(reg_kh, xa::ptr(param, GET_OFF(kh_padding)));
    CGA64::ldr(reg_src_prf, xa::ptr(param, GET_OFF(src_prf)));
    CGA64::ldr(reg_dst_prf, xa::ptr(param, GET_OFF(dst_prf)));
    CGA64::ldr(reg_ker_prf, xa::ptr(param, GET_OFF(filt_prf)));

#if 0
//[info]v1.6 追加コード。取り敢えずコメント化
    const int ic_tail = jcp.ic_tail;
    if (ic_tail) {
        Label masking_done;
        // dummy mask all 1's
        kxnorw(k_ic_tail_mask, k_ic_tail_mask, k_ic_tail_mask);
        mov(reg_load_work, ptr[param1 + GET_OFF(load_work)]);
        cmp(reg_load_work, jcp.nb_ic_blocking * jcp.ic_block);
        je(masking_done, T_NEAR);
        Reg32 reg_tail_32 = reg_tail.cvt32();
        mov(reg_tail_32, (1 << ic_tail) - 1);
        kmovw(k_ic_tail_mask, reg_tail_32);
        L(masking_done);
    }
#endif

    int l_overflow = nstl::max(0, ((kw - 1) * dilate_w - jcp.l_pad) / stride_w);
    int r_overflow = nstl::max(
            0, ((kw - 1) * dilate_w - nstl::max(0, jcp.r_pad)) / stride_w);
    int r_overflow_no_tail = nstl::max(0,
            ((kw - 1) * dilate_w - nstl::max(0, jcp.r_pad + ur_w_tail))
                    / stride_w);

    int body_l_overflow = 0, body_r_overflow = 0;
    int n_oi = iw / ur_w;
    int head_n_oi = 0, body_n_oi = 0, pretail_n_oi = 0, tail_n_oi = 0;
    int head_thread = 0, pretail_thread = 0, tail_thread = 0;
    bool threaded = is_iw_threading_on(jcp);
    xa::LabelAArch64 head_label, body_label, pretail_label, tail_label, end_label;
    assert(n_oi > 0);
    if (r_overflow_no_tail > 0) n_oi--;
    if (l_overflow > 0) n_oi--;
    if (n_oi < 0) {
        // l_overflow and r_overflow_no_tail are handled in the same compute_loop.
        // Perform one iteration of body handling l_overflow and r_overflow_no_tail.
        // TODO: Align other convolution kernels with this kernel. This version
        // now uses r_overflow_no_tail instead of r_overflow in compute loop, this was
        // done since when iw == ur_w, ur_w_tail == 0 and thus
        // r_overflow_no_tail seems more appropriate
        body_l_overflow = l_overflow;
        body_r_overflow = r_overflow_no_tail;
        n_oi = 1;
        l_overflow = 0;
        r_overflow_no_tail = 0;
    }

    if (!threaded) {
        if (n_oi > 1) { CGA64::mov_imm(reg_oi, n_oi); }
    } else {
        // Setup for threaded code generation, and jump into the correct
        // portion of code for execution.
        head_thread = 0;
        tail_thread = nb_iw - 1;
        pretail_thread = tail_thread;

        int base_n_oi = iw_block / ur_w;
        head_n_oi = l_overflow > 0 ? base_n_oi - 1 : base_n_oi;
        tail_n_oi = (iw - iw_block * (nb_iw - 1)) / ur_w;
        pretail_n_oi = tail_n_oi;
        if (r_overflow_no_tail > 0) {
            if (tail_n_oi > 0) {
                pretail_n_oi--;
                tail_n_oi = pretail_n_oi;
            } else {
                // pretail_thread and tail_thread are different
                pretail_n_oi = base_n_oi - 1;
                pretail_thread = tail_thread - 1;
            }
            if (head_thread == pretail_thread) {
                head_n_oi--;
                pretail_n_oi = 0;
                tail_n_oi = 0;
            }
        }
        body_n_oi = (head_thread < pretail_thread - 1) ? base_n_oi : 0;

        // n_oi is used to determine how much control flow in the body portion
        // of the code needs generated. As such, n_oi needs to be set to the
        // maximum number of iterations it will be used the body code section.
        n_oi = nstl::max(body_n_oi, head_n_oi);
        n_oi = nstl::max(n_oi, pretail_n_oi);

        assert(iw_block % ur_w == 0);
        CGA64::ldr(reg_iwb, xa::ptr(param, GET_OFF(iwb)));

        if (head_n_oi != 0) CGA64::mov_imm(reg_oi, head_n_oi);
        CGA64::cmp(reg_iwb, head_thread);
        CGA64::b(xa::EQ, head_label);

        CGA64::cmp(reg_iwb, pretail_thread);
        if (pretail_n_oi == 0) {
            CGA64::b(xa::EQ, pretail_label);
        } else {
            CGA64::mov_imm(reg_oi, pretail_n_oi);
            CGA64::b(xa::EQ, body_label);
        }
        if (pretail_thread != tail_thread) {
            CGA64::cmp(reg_iwb, tail_thread);
            CGA64::b(xa::EQ, tail_label);
        }
        if (body_n_oi != 0) {
            CGA64::mov_imm(reg_oi, body_n_oi);
            CGA64::b(body_label);
        } else {
            CGA64::b(end_label);
        }
    }

    CGA64::L_aarch64(head_label);
    if (l_overflow > 0) {
        compute_loop(ur_w, l_overflow, 0);
        if (threaded && head_n_oi == 0 && head_thread != pretail_thread)
            CGA64::b(end_label);
        else {
            CGA64::add_imm(reg_src, reg_src, src_shift, reg_tmp_imm);
            CGA64::add_imm(reg_dst, reg_dst, dst_shift, reg_tmp_imm);
            CGA64::add_imm(reg_src_prf, reg_src_prf, src_shift, reg_tmp_imm);
            CGA64::add_imm(reg_dst_prf, reg_dst_prf, dst_shift, reg_tmp_imm);
         }
    }

    CGA64::L_aarch64(body_label);
    if (n_oi > 0) {
        xa::LabelAArch64 ow_loop_label;
        CGA64::L_aarch64(ow_loop_label);
        {
            compute_loop(ur_w, body_l_overflow, body_r_overflow);
            if (n_oi > 1 || r_overflow_no_tail > 0 || ur_w_tail != 0) {
                CGA64::add_imm(reg_src, reg_src, src_shift, reg_tmp_imm);
                CGA64::add_imm(reg_src_prf, reg_src_prf, src_shift, reg_tmp_imm);
                if (!jcp.large_w_filter) {
                    CGA64::add_imm(reg_dst, reg_dst, dst_shift, reg_tmp_imm);
                    CGA64::add_imm(reg_dst_prf, reg_dst_prf, dst_shift, reg_tmp_imm);
                }
            }
            if (n_oi > 1) {
                CGA64::sub(reg_oi, reg_oi, 1);
                CGA64::cmp(reg_oi, 0);
                CGA64::b(xa::GT, ow_loop_label);
            }
        }
    }
    if (threaded) {
        CGA64::ldr(reg_iwb, xa::ptr(param, GET_OFF(iwb)));
        CGA64::cmp(reg_iwb, pretail_thread);
        CGA64::b(xa::EQ, end_label);
    }

    CGA64::L_aarch64(pretail_label);
    if (r_overflow_no_tail > 0) {
        compute_loop(ur_w, 0, r_overflow_no_tail);
        if (ur_w_tail != 0) {
            if (threaded && tail_thread != pretail_thread)
                CGA64::b(end_label);
            CGA64::add_imm(reg_src, reg_src, src_shift, reg_tmp_imm);
            CGA64::add_imm(reg_dst, reg_dst, dst_shift, reg_tmp_imm);
            CGA64::add_imm(reg_src_prf, reg_src_prf, src_shift, reg_tmp_imm);
            CGA64::add_imm(reg_dst_prf, reg_dst_prf, dst_shift, reg_tmp_imm);
        }
    }

    CGA64::L_aarch64(tail_label);
    if (ur_w_tail != 0) {
        /* if 'filter-width > ur_w' then the main loop only partially computes
         * width, ur_w_tail needs to offset the initial ur_w from the filter
         * address. */
        if (jcp.large_w_filter)
            compute_loop(ur_w_tail, body_l_overflow, r_overflow - ur_w, ur_w);
        else
            compute_loop(ur_w_tail, 0, r_overflow);
    }

    CGA64::L_aarch64(end_label);

    postamble();
}

status_t jit_aarch64_sve_512_conv_bwd_data_kernel_f32::init_conf(
        jit_conv_conf_t &jcp, const convolution_desc_t &cd,
        memory_desc_t &diff_src_md, memory_desc_t &weights_md,
        memory_desc_t &diff_dst_md, int nthreads) {
    if (!mayiuse(sve)) return status::unimplemented;

    const memory_desc_wrapper diff_src_d(&diff_src_md);
    const memory_desc_wrapper weights_d(&weights_md);
    const memory_desc_wrapper diff_dst_d(&diff_dst_md);
    jcp = zero<decltype(jcp)>();

    const bool with_groups = weights_d.ndims() == diff_src_d.ndims() + 1;
    int ndims = diff_src_d.ndims();

    jcp.nthr = jcp.aligned_threads = nthreads;
    jcp.ndims = ndims;
    jcp.prop_kind = cd.prop_kind;

    jcp.ngroups = with_groups ? weights_d.dims()[0] : 1;
    jcp.mb = diff_src_d.dims()[0];

    jcp.oc = diff_dst_d.dims()[1] / jcp.ngroups;
    jcp.oc_without_padding = jcp.oc;
    jcp.ic = diff_src_d.dims()[1] / jcp.ngroups;
    jcp.ic_without_padding = jcp.ic;

    jcp.id = (ndims == 5) ? diff_src_d.dims()[2] : 1;
    jcp.ih = (ndims == 3) ? 1 : diff_src_d.dims()[ndims - 2];
    jcp.iw = diff_src_d.dims()[ndims - 1];
    jcp.od = (ndims == 5) ? diff_dst_d.dims()[2] : 1;
    jcp.oh = (ndims == 3) ? 1 : diff_dst_d.dims()[ndims - 2];
    jcp.ow = diff_dst_d.dims()[ndims - 1];

    jcp.kd = (ndims == 5) ? weights_d.dims()[with_groups + 2] : 1;
    jcp.kh = (ndims == 3) ? 1 : weights_d.dims()[with_groups + ndims - 2];
    jcp.kw = weights_d.dims()[with_groups + ndims - 1];

    jcp.f_pad = (ndims == 5) ? cd.padding[0][0] : 0;
    jcp.t_pad = (ndims == 3) ? 0 : cd.padding[0][ndims - 4];
    jcp.l_pad = cd.padding[0][ndims - 3];

    jcp.stride_d = (ndims == 5) ? cd.strides[0] : 1;
    jcp.stride_h = (ndims == 3) ? 1 : cd.strides[ndims - 4];
    jcp.stride_w = cd.strides[ndims - 3];

    jcp.dilate_d = (ndims == 5) ? cd.dilates[0] : 0;
    jcp.dilate_h = (ndims == 3) ? 0 : cd.dilates[ndims - 4];
    jcp.dilate_w = cd.dilates[ndims - 3];
    if ((jcp.dilate_w != 0 && jcp.stride_w != 1)
            || (jcp.dilate_d != 0 && jcp.stride_d != 1)
            || (jcp.dilate_h != 0 && jcp.stride_h != 1))
        return status::unimplemented;

    int ext_kw = calculate_extended_filter_size(jcp.kw, jcp.dilate_w);
    int ext_kh = calculate_extended_filter_size(jcp.kh, jcp.dilate_h);
    int ext_kd = calculate_extended_filter_size(jcp.kd, jcp.dilate_d);
    jcp.r_pad = calculate_end_padding(
            jcp.l_pad, jcp.ow, jcp.iw, jcp.stride_w, ext_kw);
    jcp.b_pad = calculate_end_padding(
            jcp.t_pad, jcp.oh, jcp.ih, jcp.stride_h, ext_kh);
    jcp.back_pad = calculate_end_padding(
            jcp.f_pad, jcp.od, jcp.id, jcp.stride_d, ext_kd);
    bool kernel_outside_src = false || ext_kw <= jcp.l_pad
            || ext_kw <= jcp.r_pad || ext_kh <= jcp.t_pad || ext_kh <= jcp.b_pad
            || ext_kd <= jcp.f_pad || ext_kd <= jcp.back_pad;
    if (kernel_outside_src) return status::unimplemented;

    jcp.aligned_threads = 0;
    const auto dat_tag_nxc = pick(ndims - 3, nwc, nhwc, ndhwc);
    const auto dat_tag_nCx4c = pick(ndims - 3, nCw4c, nChw4c, nCdhw4c);
    const auto dat_tag_nCx8c = pick(ndims - 3, nCw8c, nChw8c, nCdhw8c);
    const auto dat_tag_nCx16c = pick(ndims - 3, nCw16c, nChw16c, nCdhw16c);
    auto curr_src_tag = diff_src_d.matches_one_of_tag(
            dat_tag_nxc, dat_tag_nCx16c, dat_tag_nCx8c, dat_tag_nCx4c);
    auto curr_dst_tag = diff_dst_d.matches_one_of_tag(
            dat_tag_nxc, dat_tag_nCx16c, dat_tag_nCx8c, dat_tag_nCx4c);
    bool is_data_layout_nxc
            = utils::everyone_is(dat_tag_nxc, curr_src_tag, curr_dst_tag);
//    if (mayiuse(avx512_mic) && is_data_layout_nxc) return status::unimplemented;

    jcp.is_1stconv = false;

    bool ok_to_pad_channels = true && !is_data_layout_nxc && jcp.ngroups == 1
            && diff_src_d.data_type() == data_type::f32;

    const int full_simd_w = cpu_isa_traits<sve>::vlen / typesize;
    jcp.simd_w = full_simd_w;
    bool ok_to_try_lower_zmm = true
            && IMPLICATION(is_data_layout_nxc,
                    jcp.ic < full_simd_w && jcp.oc < full_simd_w
                            && jcp.ngroups > 1)
            && mayiuse(sve) && diff_src_d.data_type() == data_type::f32
            && !jcp.is_1stconv
            && (jcp.oc % jcp.simd_w != 0 || jcp.ic % jcp.simd_w != 0)
            && !ok_to_pad_channels;

    if (ok_to_try_lower_zmm) {
        for (auto simd : {8, 4}) {
            if (jcp.ic % simd == 0 && jcp.oc % simd == 0) {
                jcp.simd_w = simd;
                break;
            }
        }
    }

    jcp.oc_block = jcp.simd_w;
    jcp.ic_block = jcp.is_1stconv ? jcp.ic : jcp.simd_w;

    if (ok_to_pad_channels) {
        jcp.oc = rnd_up(jcp.oc, jcp.oc_block);
        jcp.ic = rnd_up(jcp.ic, jcp.ic_block);
    }

    if (!IMPLICATION(!is_data_layout_nxc,
                jcp.oc % jcp.oc_block == 0 && jcp.ic % jcp.ic_block == 0))
        return status::unimplemented;
    jcp.ic_tail = is_data_layout_nxc ? jcp.ic % jcp.simd_w : 0;
    jcp.oc_tail = is_data_layout_nxc ? jcp.oc % jcp.simd_w : 0;

    format_tag_t dat_tag, wei_tag;
    const auto nxc_tag = pick(ndims - 3, nwc, nhwc, ndhwc);

    if (jcp.simd_w == 8) {
        assert(with_groups);
        dat_tag = is_data_layout_nxc ? nxc_tag
                                     : pick(ndims - 3, nCw8c, nChw8c, nCdhw8c);
        wei_tag = pick(ndims - 3, gOIw8o8i, gOIhw8o8i, gOIdhw8o8i);
    } else if (jcp.simd_w == 4) {
        assert(with_groups);
        dat_tag = is_data_layout_nxc ? nxc_tag
                                     : pick(ndims - 3, nCw4c, nChw4c, nCdhw4c);
        wei_tag = pick(ndims - 3, gOIw4o4i, gOIhw4o4i, gOIdhw4o4i);
    } else {
        dat_tag = is_data_layout_nxc
                ? pick(ndims - 3, nwc, nhwc, ndhwc)
                : pick(ndims - 3, nCw16c, nChw16c, nCdhw16c);
        wei_tag = pick(2 * ndims - 6 + with_groups, OIw16o16i, gOIw16o16i,
                OIhw16o16i, gOIhw16o16i, OIdhw16o16i, gOIdhw16o16i);
    }

    if (diff_src_md.format_kind == format_kind::any) {
        CHECK(memory_desc_init_by_tag(diff_src_md, dat_tag));
    } else if (curr_src_tag != dat_tag)
        return status::unimplemented;
    jcp.src_tag = dat_tag;

    if (diff_dst_md.format_kind == format_kind::any) {
        CHECK(memory_desc_init_by_tag(diff_dst_md, dat_tag));
    } else if (curr_dst_tag != dat_tag)
        return status::unimplemented;
    jcp.dst_tag = dat_tag;

    if (init_tag(jcp.wei_tag, weights_md, weights_d, wei_tag)
            != status::success)
        return status::unimplemented;

    jcp.nb_ic = div_up(jcp.ic, jcp.ic_block);
    jcp.nb_oc = div_up(jcp.oc, jcp.oc_block);

    jcp.ur_w = jcp.stride_w;

    int regs = 26;
    if (jcp.iw <= regs)
        jcp.ur_w = jcp.iw;
    else {
        for (int ur_w = regs; ur_w > 0; --ur_w)
            if (ur_w % jcp.stride_w == 0) {
                jcp.ur_w = ur_w;
                break;
            }
    }
    int l_overflow = nstl::max(
            0, ((jcp.kw - 1) * (jcp.dilate_w + 1) - jcp.l_pad) / jcp.stride_w);
    int r_overflow_no_tail = nstl::max(0,
            ((jcp.kw - 1) * (jcp.dilate_w + 1)
                    - nstl::max(0, jcp.r_pad + jcp.iw % jcp.ur_w))
                    / jcp.stride_w);
    int n_oi = jcp.iw / jcp.ur_w;
    if (r_overflow_no_tail > 0) n_oi--;

    if (mayiuse(sve) && diff_dst_d.data_type() == data_type::f32
            && weights_d.data_type() == data_type::f32
            && diff_src_d.data_type() == data_type::f32) {
        jcp.ver = ver_fma;
        jcp.typesize_in = typesize;
        jcp.typesize_out = typesize;
#if 0
        if (!is_data_layout_nxc && mayiuse(avx512_mic_4ops) && jcp.stride_w == 1
                && jcp.stride_h == 1 && jcp.stride_d == 1) {
            jcp.ver = ver_4fma;
        }
#endif
    } else {
        return status::unimplemented;
    }

    if (!utils::everyone_is(0, jcp.dilate_d, jcp.dilate_h, jcp.dilate_w)
            && jcp.ver != ver_fma)
        return status::unimplemented;

    jcp.nb_ic_blocking = jcp.nb_oc_blocking = 1;

    // Heuristic to optimize code size on KNX
    bool large_code_size = (jcp.ur_w != jcp.ow)
            && ((l_overflow <= 0 && n_oi > 0) || (l_overflow > 0 && n_oi > 1))
            && (r_overflow_no_tail > 0) && (l_overflow > 0);
    if (large_code_size) {
        const int max_code_size = 24 * 1024;
        const int num_ops_per_reg = 6 + jcp.oc_block * jcp.kw;
        int mult = 1;
        if (l_overflow > 0) mult += 1;
        if (r_overflow_no_tail > 0) mult += 1;
        for (int ur_w = jcp.ur_w; ur_w > regs / 2; --ur_w) {
            if ((ur_w / jcp.stride_w) * mult * num_ops_per_reg * 9.2
                    < max_code_size) {
                if (ur_w % jcp.stride_w == 0) {
                    jcp.ur_w = ur_w;
                    break;
                }
            }
        }
    }

    /* Support for large filter 'kw > 14' is only possible when ur_w is small
     * (e.g ur_w = 1) because of register allocation (max_reg = 31) */
    const int min_filter_size = 14;
    /* Don't let JIT generate too big of a code which might result in an
     * out-of-memory crash. */
    const int max_filter_size = 20;

    /* These conditions define a set of shapes with 'ow = 1' which
     * have a very limited optimization space for performance.
     * Optimize by using a targeted 'jcp.nb_ic_blocking' value. */
    jcp.large_w_filter = jcp.kw >= min_filter_size && jcp.kw < max_filter_size
            && jcp.ow == 1 && jcp.nb_ic > 1 && jcp.kw == jcp.iw
            && jcp.stride_w == 1
            && utils::everyone_is(0, jcp.dilate_d, jcp.dilate_h, jcp.dilate_w);

    if (jcp.ver == ver_fma && mayiuse(sve)) {
        int try_nb_ic_blocking = 2;
#if 0
        unsigned int ker_inp_size = typesize * jcp.iw * jcp.ic_block
                * try_nb_ic_blocking * jcp.kh;
        unsigned int ker_out_size = typesize * jcp.ow * jcp.oc_block;
        unsigned int ker_wei_size = typesize * jcp.kh * jcp.kw * jcp.ic_block
                * jcp.oc_block * try_nb_ic_blocking;
        unsigned int ker_total_size
                = ker_inp_size + ker_out_size + ker_wei_size;
#endif
#if 0
        bool use_expl_bcast
                = !(jcp.kw == 1 || (jcp.kw == 5 && jcp.iw < 8)
                          || (jcp.kw < 5
                                  && ((jcp.iw <= 5
                                              || (jcp.iw > 8 && jcp.iw <= 13))
                                          || ker_total_size > L1_cache_size)))
                || jcp.stride_h > 1 || jcp.stride_d > 1;
#else
//[info]v0.21の修正内容からL1_cache分のシフト処理部分を削除
        bool use_expl_bcast
                = !(jcp.kw == 1 || (jcp.kw == 5 && jcp.iw < 8)
                          || (jcp.kw < 5
                                  && ((jcp.iw <= 5
                                              || (jcp.iw > 8 && jcp.iw <= 13)))))
                || jcp.stride_h > 1 || jcp.stride_d > 1;
#endif
        if (use_expl_bcast && !jcp.large_w_filter) {
            jcp.kernel_kind = embd_bcast;
            jcp.ur_w = nstl::min(jcp.iw, regs);
            jcp.nb_ic_blocking = jcp.nb_oc_blocking = 1;
#if 0
            if (!(jcp.kw > 3
                        || (jcp.kw == 3 && ker_total_size < L1_cache_size
                                && jcp.ow > 8))
                    && jcp.stride_h == 1 && jcp.stride_d == 1)
#else
//[info]v0.21の修正内容からL1_cache分のシフト処理部分を削除
            if (!(jcp.kw > 3
                        || (jcp.kw == 3 &&  jcp.ow > 8))
                    && jcp.stride_h == 1 && jcp.stride_d == 1)
#endif
                if (jcp.nb_ic % try_nb_ic_blocking == 0) {
                    jcp.nb_ic_blocking = try_nb_ic_blocking;
                    jcp.ur_w = 30 / (jcp.nb_ic_blocking + 1);
                    if (jcp.iw < jcp.ur_w) jcp.ur_w = jcp.iw;
                }
        } else {
            jcp.kernel_kind = expl_bcast;
            jcp.nb_oc_blocking = 1;
            jcp.nb_ic_blocking = jcp.large_w_filter ? 2 : 4;
            if (jcp.nb_ic < jcp.nb_ic_blocking) jcp.nb_ic_blocking = jcp.nb_ic;
            if (jcp.nb_ic % jcp.nb_ic_blocking != 0)
                for (int i = jcp.nb_ic_blocking; i > 0; i--)
                    if (jcp.nb_ic % i == 0) {
                        jcp.nb_ic_blocking = i;
                        break;
                    }
            if(jcp.stride_w > 1){
                jcp.ur_w = 30 / (jcp.nb_ic_blocking + 1);
            }else{
                jcp.ur_w = 31 / (jcp.nb_ic_blocking + 1);
            }
            if (jcp.iw < jcp.ur_w) jcp.ur_w = jcp.iw;
        }
    }
    jcp.ur_w_tail = jcp.iw % jcp.ur_w;

#if 0
    auto is_iw_threading_applicable
            = [=]() { return one_of(jcp.ndims, 3, 4) && !mayiuse(avx512_mic); };
#else
    auto is_iw_threading_applicable
            = [=]() { return one_of(jcp.ndims, 3, 4); };
#endif

    auto get_thr_eff = [=](int nb_ic_blocking, int iw_block) {
        // Cost heuristic for threading overhead. Determined using OMP.
        const float iw_block_cost = 32.0;

        int nb_iw = div_up(jcp.iw, iw_block);
        int nb_ic_chunks = div_up(jcp.nb_ic, nb_ic_blocking);
        int work_amount = jcp.mb * jcp.ih * nb_ic_chunks * nb_iw;
        float disbalance = (float)jcp.iw / rnd_up(jcp.iw, iw_block);
        float block_overhead = nstl::max(0.0f, 1.0f - iw_block_cost / iw_block);
        float thr_eff = block_overhead * disbalance
                * ((float)work_amount / rnd_up(work_amount, nthreads));
        return thr_eff;
    };

    auto get_iw_block = [=](int nb_ic_blocking, int ur_w) {
        int res_iw_block = jcp.iw;
        if (!is_iw_threading_applicable()) return res_iw_block;

        int max_nb_iw = div_up(jcp.iw, 2 * ur_w);
        int iw_block_thr;
        float eff;

        if (jcp.ndims == 3) {
            // Blocking optimization to prevent data from leaving cache This
            // blocking optimization does not handle height blocking, so it does
            // not apply to higher dimensions.
            // TODO: Implement a more general optimization taking into account
            // the height dimension.
            int L2_part
                    = (platform::get_per_core_cache_size(2) * 7 / 8) / typesize;
            int size_diff_src_chunk = jcp.ic_block * nb_ic_blocking * ur_w;
            int size_diff_dst_chunk = jcp.oc_block * ur_w;
            int size_wei_chunk
                    = jcp.ic_block * nb_ic_blocking * jcp.oc_block * jcp.kw;
            int nurw_cache = (L2_part - 2 * size_wei_chunk)
                    / (2 * size_diff_dst_chunk + 2 * size_diff_src_chunk);
            // current design of generate() requires iw_block >= 2 * ur_w
            int iw_block_cache = ur_w * nstl::max(2, nurw_cache);

            iw_block_thr = iw_block_cache;
        } else
            iw_block_thr = jcp.iw;
        eff = get_thr_eff(nb_ic_blocking, iw_block_thr);

        // Search for most efficient threading over iw_blocks.
        int start_nb_iw = div_up(jcp.iw, iw_block_thr);
        for (int nb_iw = start_nb_iw; nb_iw <= max_nb_iw; nb_iw++) {
            float eff_threshold = 0.98f;
            if (eff > eff_threshold) break;
            int iw_block
                    = nstl::min(rnd_up(div_up(jcp.iw, nb_iw), ur_w), jcp.iw);
            if (div_up(jcp.iw, iw_block) != nb_iw) continue;
            float thr_eff = get_thr_eff(nb_ic_blocking, iw_block);
            if (iw_block >= 2 * ur_w && thr_eff > eff) {
                iw_block_thr = iw_block;
                eff = thr_eff;
            }
        }
        res_iw_block = nstl::min(jcp.iw, nstl::max(2 * ur_w, iw_block_thr));
        return res_iw_block;
    };

    jcp.iw_block = get_iw_block(jcp.nb_ic_blocking, jcp.ur_w);
    jcp.nb_iw = div_up(jcp.iw, jcp.iw_block);

    if (l_overflow * jcp.stride_w > jcp.ur_w && !jcp.large_w_filter)
        return status::unimplemented;
    r_overflow_no_tail = nstl::max(0,
            ((jcp.kw - 1) * (jcp.dilate_w + 1)
                    - nstl::max(0, jcp.r_pad + jcp.ur_w_tail))
                    / jcp.stride_w);
    bool tails_not_ok = false
            /* maximum 1 ur_w block with r_overflow so far */
            || r_overflow_no_tail * jcp.stride_w > jcp.ur_w
            /* ur_w must be a multiple of stride */
            || ((jcp.iw > jcp.ur_w) && (jcp.ur_w % jcp.stride_w != 0))
            /* r_pad must not extend beyond ur_w_tail */
            || ((jcp.iw > jcp.ur_w) && (jcp.r_pad + jcp.ur_w_tail < 0));
    if (tails_not_ok) return status::unimplemented;

    pick_loop_order(jcp);

    jcp.nb_oc_L2 = jcp.nb_oc;

    if (is_data_layout_nxc) {
        // TODO: improve L2 blocking for large OC
        const int nb_oc_theshold_L2 = 32;
        if (jcp.nb_oc > nb_oc_theshold_L2 && jcp.nb_oc < 2 * nb_oc_theshold_L2)
            jcp.nb_oc_L2 = div_up(jcp.nb_oc, 2);
        else
            jcp.nb_oc_L2 = nstl::min(nb_oc_theshold_L2, jcp.nb_oc);
    }

    bool args_ok = true && jcp.ic <= diff_src_d.padded_dims()[1]
            && jcp.oc <= diff_dst_d.padded_dims()[1]
            && jcp.ic <= weights_d.padded_dims()[with_groups + 1]
            && jcp.oc <= weights_d.padded_dims()[with_groups + 0];
    if (!args_ok) return status::unimplemented;

    // A rough check on code size
    // TODO: come up with a tighter bound
    {
        const int max_code_size = 256 * 1024; // default size of jit generator
        int mult = 1 + (l_overflow > 0) + (r_overflow_no_tail > 0);
        const float max_instruction_size = 15;
        float ur_fac
                = (float)jcp.kw * jcp.oc_block * jcp.nb_ic_blocking * jcp.ur_w;
        float code_size = mult * ur_fac * max_instruction_size;
        if (code_size > max_code_size && !jcp.large_w_filter)
            return status::unimplemented;
    }

    return status::success;
}

void jit_aarch64_sve_512_conv_bwd_data_kernel_f32::init_scratchpad(
        memory_tracking::registrar_t &scratchpad, const jit_conv_conf_t &jcp) {
    UNUSED(scratchpad);
    UNUSED(jcp);
}

// Initialize static data members
const int jit_aarch64_sve_512_conv_bwd_weights_kernel_f32::max_ur_w = 28;
const int jit_aarch64_sve_512_conv_bwd_weights_kernel_f32::min_oh_reduce = 9;

void jit_aarch64_sve_512_conv_bwd_weights_kernel_f32::
        od_step_comeback_pointers() {
    xa::LabelAArch64 kd_comeback_label;

    /* 'depth' loop count bound by 'kd_work_size' */
    CGA64::mov(kj, reg_kd_count);
    CGA64::L_aarch64(kd_comeback_label);
    {
        int inp_mult = is_src_layout_nxc()
                ? jcp.ngroups * jcp.ic
                : (jcp.is_1stconv ? 1 : jcp.ic_block);
#if 0
        int iw = jcp.ver == ver_4fma ? jcp.tr_iw : jcp.iw;
#else
        int iw = jcp.iw;
#endif
        CGA64::sub_imm(reg_input, reg_input, jcp.typesize_in * (jcp.dilate_d + 1) * jcp.ih * iw * inp_mult, reg_tmp_imm);
        CGA64::sub_imm(reg_kernel, reg_kernel, jcp.typesize_out * jcp.kh * jcp.kw * jcp.ic_block * jcp.oc_block, reg_tmp_imm);
        CGA64::sub(kj, kj, 1);
        CGA64::cmp(kj, 0);
        CGA64::b(xa::GT, kd_comeback_label);
    }
}

void jit_aarch64_sve_512_conv_bwd_weights_kernel_f32::
        oh_step_comeback_pointers() {
    xa::LabelAArch64 kh_comeback_label, kd_comeback_label;
    CGA64::mov(kj, reg_kh);
    CGA64::L_aarch64(kh_comeback_label);
    {
        int inp_mult = is_src_layout_nxc()
                ? jcp.ngroups * jcp.ic
                : (jcp.is_1stconv ? 1 : jcp.ic_block);
#if 0
        int iw = jcp.ver == ver_4fma ? jcp.tr_iw : jcp.iw;
#else
        int iw = jcp.iw;
#endif
        CGA64::sub_imm(reg_input, reg_input, jcp.typesize_in * (jcp.dilate_h + 1) * iw * inp_mult, reg_tmp_imm);
        CGA64::sub_imm(reg_kernel, reg_kernel, jcp.typesize_out * jcp.kw * jcp.ic_block * jcp.oc_block, reg_tmp_imm);
        CGA64::sub(kj, kj, 1);
        CGA64::cmp(kj, 0);
        CGA64::b(xa::GT, kh_comeback_label);
    }
}

#if 0
void jit_avx512_common_conv_bwd_weights_kernel_f32::compute_ic_block_step(
        int ur_w, int pad_l, int pad_r, int ic_block_step, int input_offset,
        int kernel_offset, int output_offset, bool input_wraparound) {
    if (jcp.ver == ver_4fma)
        compute_ic_block_step_4fma(ur_w, pad_l, pad_r, ic_block_step,
                input_offset, kernel_offset, output_offset, input_wraparound);
    else if (jcp.ver == ver_fma && jcp.kernel_kind == expl_bcast)
        compute_ic_block_step_fma_expl(ur_w, pad_l, pad_r, ic_block_step,
                input_offset, kernel_offset, output_offset, input_wraparound);
    else if (jcp.ver == ver_fma)
        compute_ic_block_step_fma(ur_w, pad_l, pad_r, ic_block_step,
                input_offset, kernel_offset, output_offset, input_wraparound);
    else
        assert(!"unknown convolution version");
}
#else
//[info]v0.21のコードを展開
void jit_aarch64_sve_512_conv_bwd_weights_kernel_f32::compute_ic_block_step(
    int ur_w, int pad_l, int pad_r,
    int ic_block_step, int input_offset, int kernel_offset,
    int output_offset, bool input_wraparound)
{

    int kw = jcp.kw;
    int ic_block = jcp.ic_block;
    int oc_block = jcp.oc_block;
    for (int i_kw = 0; i_kw < kw; i_kw++) {
        for (int i_ic = 0; i_ic < ic_block_step; i_ic++) {
            CGA64::add_imm(reg_add_tmp, reg_kernel, typesize * (i_kw * ic_block + i_ic)
                     * jcp.oc_block + kernel_offset, reg_tmp_imm);
            CGA64::ldr(xa::ZReg(i_kw * ic_block_step + i_ic), xa::ptr(reg_add_tmp));
                //vmovups(Zmm(i_kw * ic_block_step + i_ic),
                //  EVEX_compress_addr(reg_kernel, typesize * (i_kw * ic_block
                //  + i_ic) * jcp.oc_block + kernel_offset));
        }
    }

    for (int i_ur = 0; i_ur < ur_w; i_ur++) {
        if (i_ur == 0) {
            CGA64::add_imm(reg_add_tmp, reg_output, typesize * (i_ur + 0) * oc_block + output_offset, reg_tmp_imm);
            assert(kw * ic_block_step + (i_ur + 0) % 4 < 31);
            CGA64::ldr(xa::ZReg(kw * ic_block_step + (i_ur + 0) % 4), xa::ptr(reg_add_tmp));
                //vmovups(Zmm(kw * ic_block_step + (i_ur + 0) % 4),
                //    EVEX_compress_addr(reg_output, typesize * (i_ur + 0)
                //    * oc_block + output_offset));
            if (ur_w > 1) {
                CGA64::add_imm(reg_add_tmp, reg_output, typesize * (i_ur + 1) * oc_block + output_offset, reg_tmp_imm);
                assert(kw * ic_block_step + (i_ur + 1) % 4 < 31);
                CGA64::ldr(xa::ZReg(kw * ic_block_step + (i_ur + 1) % 4), xa::ptr(reg_add_tmp));
                    //vmovups(Zmm(kw * ic_block_step + (i_ur + 1) % 4),
                    //EVEX_compress_addr(reg_output, typesize * (i_ur + 1) * oc_block
                    //+ output_offset));
            }
            if (ur_w > 2) {
                CGA64::add_imm(reg_add_tmp, reg_output, typesize * (i_ur + 2) * oc_block + output_offset, reg_tmp_imm);
                assert(kw * ic_block_step + (i_ur + 2) % 4 < 31);
                CGA64::ldr(xa::ZReg(kw * ic_block_step + (i_ur + 2) % 4), xa::ptr(reg_add_tmp));
                    //vmovups(Zmm(kw * ic_block_step + (i_ur + 2) % 4),
                    //EVEX_compress_addr(reg_output, typesize * (i_ur + 2) * oc_block
                    //+ output_offset));
            }
            if (ur_w > 3) {
                CGA64::add_imm(reg_add_tmp, reg_output, typesize * (i_ur + 3) * oc_block + output_offset, reg_tmp_imm);
                assert(kw * ic_block_step + (i_ur + 3) % 4 < 31);
                CGA64::ldr(xa::ZReg(kw * ic_block_step + (i_ur + 3) % 4), xa::ptr(reg_add_tmp));
                    //vmovups(Zmm(kw * ic_block_step + (i_ur + 3) % 4),
                    //EVEX_compress_addr(reg_output, typesize * (i_ur + 3) * oc_block
                    //+ output_offset));
            }
        } else if (i_ur + 3 < ur_w) {
            CGA64::add_imm(reg_add_tmp, reg_output, typesize * (i_ur + 3) * oc_block + output_offset, reg_tmp_imm);
            assert(kw * ic_block_step + (i_ur + 3) % 4 < 31);
            CGA64::ldr(xa::ZReg(kw * ic_block_step + (i_ur + 3) % 4), xa::ptr(reg_add_tmp));
                // vmovups(Zmm(kw * ic_block_step + (i_ur + 3) % 4),
                //     EVEX_compress_addr(reg_output, typesize * (i_ur + 3) * oc_block
                //     + output_offset));
        }

        for (int i_kw = 0; i_kw < kw; i_kw++) {
            int i_iw = i_ur * jcp.stride_w + i_kw * (jcp.dilate_w + 1);
            if (i_iw - pad_l < 0 || i_iw > (ur_w - 1) * jcp.stride_w +
                    (kw - 1) * (jcp.dilate_w + 1) - pad_r) continue;
            for (int i_ic = 0; i_ic < ic_block_step; i_ic++) {
#if 0
                const size_t i_offset = (size_t)input_offset
                    + (size_t)typesize * (jcp.ver == ver_4fma
                            ? (i_iw - pad_l + i_ic * jcp.tr_iw)
                            : (jcp.is_1stconv
                                ? (i_iw - pad_l) + (size_t)i_ic
                                    * ((size_t)jcp.ih*jcp.iw*jcp.id)
                                : (i_iw - pad_l) * ic_block + i_ic));
#else
                const size_t i_offset = (size_t)input_offset
                    + (size_t)typesize * (
                             (jcp.is_1stconv
                                ? (i_iw - pad_l) + (size_t)i_ic
                                    * ((size_t)jcp.ih*jcp.iw*jcp.id)
                                : (i_iw - pad_l) * ic_block + i_ic));
#endif
                assert((i_kw * ic_block_step + i_ic) < 31);
                assert((kw * ic_block_step + (i_ur % 4)) < 31);
                assert(i_offset < (1<<31));
                CGA64::add_imm(reg_add_tmp, reg_input, i_offset, reg_tmp_imm);
                ld1rw(zreg_idata, reg_p_all_ones, xa::ptr(reg_add_tmp));
                CGA64::fmla(xa::ZRegS(i_kw * ic_block_step + i_ic), reg_p_all_ones,
                            xa::ZRegS(kw * ic_block_step + i_ur % 4),
                            xa::ZRegS(zreg_idata));
                    //vfmadd231ps(Zmm(i_kw * ic_block_step + i_ic),
                    //    Zmm(kw * ic_block_step + i_ur % 4),
                    //    EVEX_compress_addr_safe(reg_input, i_offset, reg_long_offt, true));
            }
        }
    }

    for (int i_kw = 0; i_kw < kw; i_kw++) {
        for (int i_ic = 0; i_ic < ic_block_step; i_ic++) {
            CGA64::add_imm(reg_add_tmp, reg_kernel, typesize * (i_kw * ic_block + i_ic) * jcp.oc_block + kernel_offset, reg_tmp_imm);
            CGA64::str(xa::ZReg(i_kw * ic_block_step + i_ic), xa::ptr(reg_add_tmp));
                //vmovups(EVEX_compress_addr(reg_kernel, typesize
                //    * (i_kw * ic_block + i_ic) * jcp.oc_block + kernel_offset),
                //    Zmm(i_kw * ic_block_step + i_ic));
        }
    }
}
#endif

//[info]ニーモニックの置き換えのみ実施
void jit_aarch64_sve_512_conv_bwd_weights_kernel_f32 ::
        compute_oh_step_unroll_ow_icblock(int ic_block_step, int max_ur_w) {
    UNUSED(max_ur_w);

    xa::LabelAArch64 kh_label, kd_label;

    int ic_block = jcp.ic_block;
    int oc_block = jcp.oc_block;
    const bool src_layout_nxc = is_src_layout_nxc();
    int inp_mul = src_layout_nxc ? jcp.ngroups * jcp.ic
                                 : (!jcp.is_1stconv ? ic_block : 1);
#if 0
    int iw = jcp.ver == ver_4fma ? jcp.tr_iw : jcp.iw;
#else
    int iw = jcp.iw;
#endif
    int r_pad = nstl::max(0, jcp.r_pad);
    int l_pad = jcp.l_pad;

    if (jcp.ndims == 5) {
        CGA64::L_aarch64(kd_label);
        CGA64::mov(reg_input, aux_reg_input);
        CGA64::mov(reg_kernel, aux_reg_kernel);
    }

    const int ic_tail = jcp.ic_tail;
    const bool generate_icb_loop = jcp.nb_ic_blocking_max > 1;
    CGA64::mov(kj, reg_kh);
    CGA64::L_aarch64(kh_label);
    {
        xa::LabelAArch64 icb_block_label, icb_block_label_cb, ic_tail_loop, ic_tail_label;
        if (generate_icb_loop || ic_tail) {
            CGA64::mov(reg_input_org, reg_input);
            CGA64::mov(reg_kernel_org, reg_kernel);
            CGA64::ldr(reg_icb, xa::ptr(param, GET_OFF(reduce_work)));
        }

        if (ic_tail) {
            CGA64::cmp(reg_icb, ic_block);
            CGA64::b(xa::LT, ic_tail_loop);
        }

        const int ic_tail_loop_work = rnd_dn(ic_tail, ic_block_step);
        xa::LabelAArch64 icb_block_label_end;
       CGA64::L_aarch64(icb_block_label);
        for (int i_b_ic = 0; i_b_ic < jcp.ic_block; i_b_ic += ic_block_step) {
#if 0
            const int input_offset = jcp.typesize_in
                    * (jcp.ver == ver_4fma ? i_b_ic * iw : i_b_ic);
#else
            const int input_offset = jcp.typesize_in * i_b_ic;
#endif
            compute_ic_block_step(jcp.ur_w, l_pad, r_pad, ic_block_step,
                    input_offset, jcp.typesize_out * i_b_ic * jcp.oc_block, 0,
                    i_b_ic + ic_block_step >= jcp.ic_block);
            if (generate_icb_loop || ic_tail) CGA64::sub_imm(reg_icb, reg_icb, ic_block_step, reg_tmp_imm);
            if (ic_tail && i_b_ic + ic_block_step == ic_tail_loop_work) {
                CGA64::cmp(reg_icb, ic_block_step);
                CGA64::b(xa::LT, icb_block_label_end);
            }
        }
        CGA64::L_aarch64(icb_block_label_end);

        const int input_icb_shift = jcp.typesize_in * ic_block;
        const size_t kernel_icb_shift = (size_t)jcp.typesize_out * jcp.kd
                * jcp.kh * jcp.kw * ic_block * oc_block;

        if (generate_icb_loop) {
            // icb loop supported for src in nxc layout only
            assert(src_layout_nxc);
            CGA64::add_imm(reg_input, reg_input, input_icb_shift, reg_tmp_imm);
            CGA64::add_imm(reg_kernel, reg_kernel, kernel_icb_shift, reg_tmp_imm);
            CGA64::cmp(reg_icb, ic_block);
            CGA64::b(xa::GE, icb_block_label);
        }

        if (ic_tail) {
            CGA64::L_aarch64(ic_tail_loop);
            xa::LabelAArch64 skip_ic_tail;
            CGA64::cmp(reg_icb, 0);
            CGA64::b(xa::LE, skip_ic_tail);
            if (ic_tail_loop_work) {
                CGA64::cmp(reg_icb, ic_tail_loop_work);
                CGA64::b(xa::GE, icb_block_label);
                if (generate_icb_loop) {
                    // compensate offset added in generate_icb_loop
                    CGA64::sub_imm(reg_input, reg_input, input_icb_shift, reg_tmp_imm);
                    CGA64::sub_imm(reg_kernel, reg_kernel, kernel_icb_shift, reg_tmp_imm);
                }
            }

            CGA64::L_aarch64(ic_tail_label);
            if (ic_tail % ic_block_step) {
                CGA64::cmp(reg_icb, 0);
                CGA64::b(xa::LE, skip_ic_tail);
                const int i_b_ic = ic_tail_loop_work;
#if 0
                const int input_offset = jcp.typesize_in
                        * (jcp.ver == ver_4fma ? i_b_ic * iw : i_b_ic);
#else
                const int input_offset = jcp.typesize_in * i_b_ic;
#endif
                compute_ic_block_step(jcp.ur_w, l_pad, r_pad,
                        ic_tail % ic_block_step, input_offset,
                        jcp.typesize_out * i_b_ic * jcp.oc_block, 0);
            }
            CGA64::L_aarch64(skip_ic_tail);
        }

        if (generate_icb_loop || ic_tail) {
            CGA64::mov(reg_kernel, reg_kernel_org);
            CGA64::mov(reg_input, reg_input_org);
        }

        CGA64::add_imm(reg_input, reg_input, jcp.typesize_in * (jcp.dilate_h + 1) * iw * inp_mul, reg_tmp_imm);
        CGA64::add_imm(reg_kernel, reg_kernel, jcp.typesize_out * jcp.kw * ic_block * oc_block, reg_tmp_imm);
        CGA64::sub(kj, kj, 1);
        CGA64::cmp(kj, 0);
        CGA64::b(xa::GT, kh_label);
    }

    if (jcp.ndims == 5) {
        CGA64::add_imm(aux_reg_input, aux_reg_input,
                jcp.typesize_in * (jcp.dilate_d + 1) * jcp.ih * iw * inp_mul, reg_tmp_imm);
        CGA64::add_imm(aux_reg_kernel, aux_reg_kernel, 
                jcp.typesize_out * jcp.kh * jcp.kw * ic_block * oc_block, reg_tmp_imm);
        CGA64::sub(ki, ki, 1);
        CGA64::cmp(ki, 0);
        CGA64::b(xa::GT, kd_label);
    }
}

//[info]ニーモニックの置き換えのみ実施
void jit_aarch64_sve_512_conv_bwd_weights_kernel_f32 ::compute_oh_step_unroll_ow(
        int ic_block_step, int max_ur_w) {
    xa::LabelAArch64 kh_label, ic_block_label, ic_tail_loop_label, ic_tail_label, kd_label;
    const bool src_layout_nxc = is_src_layout_nxc();
    int inp_mul = src_layout_nxc ? jcp.ngroups * jcp.ic
                                 : (!jcp.is_1stconv ? jcp.ic_block : 1);
    const int ic_tail = jcp.ic_tail;
    UNUSED(max_ur_w);

    int ic_block = jcp.ic_block;
    int oc_block = jcp.oc_block;

    int ow = jcp.ow;

    int r_pad = nstl::max(0, jcp.r_pad);
    int l_pad = jcp.l_pad;

    if (jcp.ndims == 5) {
        CGA64::L_aarch64(kd_label);
        CGA64::mov(reg_input, aux_reg_input);
        CGA64::mov(reg_kernel, aux_reg_kernel);
    }

    const bool generate_icb_loop = jcp.nb_ic_blocking_max > 1;
    CGA64::mov(kj, reg_kh);
    CGA64::L_aarch64(kh_label);
    {
        xa::LabelAArch64 icb_block_label;
        if (generate_icb_loop || ic_tail) {
            CGA64::mov(reg_input_org, reg_input);
            CGA64::mov(reg_kernel_org, reg_kernel);
            CGA64::ldr(reg_icb, xa::ptr(param, GET_OFF(reduce_work)));
        }

        if (ic_tail) {
            CGA64::cmp(reg_icb, ic_block);
            CGA64::b(xa::LT, ic_tail_loop_label);
        }

        CGA64::L_aarch64(icb_block_label);
        xa::LabelAArch64 icb_block_label_end;
        CGA64::mov(b_ic, ic_block);
        CGA64::L_aarch64(ic_block_label);
        {
            compute_ic_block_step(ow, l_pad, r_pad, ic_block_step, 0, 0, 0);
#if 0
            size_t inp_icblk_stride = jcp.is_1stconv && !src_layout_nxc
                    ? (size_t)jcp.ih * jcp.iw * jcp.id
                    : (jcp.ver == ver_4fma ? jcp.tr_iw : 1);
#else
            size_t inp_icblk_stride = jcp.is_1stconv && !src_layout_nxc
                    ? (size_t)jcp.ih * jcp.iw * jcp.id
                    : 1;
#endif
            size_t input_offset
                    = inp_icblk_stride * jcp.typesize_in * ic_block_step;
            CGA64::add_imm(reg_input, reg_input, input_offset, reg_tmp_imm);
            CGA64::add_imm(reg_kernel, reg_kernel, jcp.typesize_out * ic_block_step * oc_block, reg_tmp_imm);
            CGA64::sub_imm(b_ic, b_ic, ic_block_step, reg_tmp_imm);
            if (generate_icb_loop || ic_tail) CGA64::sub_imm(reg_icb, reg_icb, ic_block_step, reg_tmp_imm);
            CGA64::cmp(b_ic, ic_block_step);
            CGA64::b(xa::GE, ic_block_label);
        }
        CGA64::L_aarch64(icb_block_label_end);

        const int input_shift
                = jcp.typesize_in * (jcp.dilate_h + 1) * jcp.iw * inp_mul;

        if (generate_icb_loop || ic_tail) {
            const size_t kernel_icb_shift = (size_t)jcp.typesize_out * jcp.kd
                    * jcp.kh * jcp.kw * ic_block * oc_block;
            if (generate_icb_loop) {
                // icb loop supported for src in nxc layout only
                assert(src_layout_nxc);
                xa::LabelAArch64 icb_loop_done;
                CGA64::add_imm(reg_kernel, reg_kernel,
                        kernel_icb_shift
                                - jcp.typesize_out * ic_block * oc_block,
                        reg_tmp_imm);
                CGA64::cmp(reg_icb, ic_block);
                CGA64::b(xa::GE, icb_block_label);
                CGA64::L_aarch64(icb_loop_done);
            }

            CGA64::L_aarch64(ic_tail_loop_label);
            if (ic_tail) {
                xa::LabelAArch64 skip_ic_tail;
                const int ic_tail_loop_work = rnd_dn(ic_tail, ic_block_step);
                CGA64::cmp(reg_icb, 0);
                CGA64::b(xa::LE, skip_ic_tail);
                CGA64::mov(b_ic, reg_icb);
                if (ic_tail_loop_work) {
                    CGA64::cmp(reg_icb, ic_block_step);
                    CGA64::b(xa::GE, ic_block_label);
                    if (generate_icb_loop) {
                        // compensate offset added in generate_icb_loop
                        CGA64::sub_imm(reg_kernel, reg_kernel,
                                kernel_icb_shift
                                        - jcp.typesize_out * ic_block
                                                * oc_block,
                                reg_tmp_imm);
                    }
                }

                CGA64::L_aarch64(ic_tail_label);
                if (ic_tail % ic_block_step) {
                    CGA64::cmp(reg_icb, 0);
                    CGA64::b(xa::LE, skip_ic_tail);
                    compute_ic_block_step(
                            ow, l_pad, r_pad, ic_tail % ic_block_step, 0, 0, 0);
                }
                CGA64::L_aarch64(skip_ic_tail);
            }

            CGA64::mov(reg_kernel, reg_kernel_org);
            CGA64::mov(reg_input, reg_input_org);

            CGA64::add_imm(reg_input, reg_input, input_shift, reg_tmp_imm);
            CGA64::add_imm(reg_kernel, reg_kernel, jcp.typesize_out * jcp.kw * ic_block * oc_block, reg_tmp_imm);

        } else if (jcp.is_1stconv && !src_layout_nxc) {
            size_t input_offset = (size_t)jcp.typesize_in * jcp.id * jcp.ih
                    * jcp.iw * ic_block;
            CGA64::sub_imm(reg_input, reg_input, input_offset, reg_tmp_imm);
            CGA64::add_imm(reg_input, reg_input, input_shift, reg_tmp_imm);
        } else {
            CGA64::add_imm(reg_input, reg_input, input_shift - jcp.typesize_in * jcp.ic_block, reg_tmp_imm);
        }

        if (!(generate_icb_loop || ic_tail))
            CGA64::add_imm(reg_kernel, reg_kernel,
                    jcp.typesize_out * (jcp.kw - 1) * ic_block * oc_block, reg_tmp_imm);
        CGA64::sub(kj, kj, 1);
        CGA64::cmp(kj, 0);
        CGA64::b(xa::GT, kh_label);
    }
    if (jcp.ndims == 5) {
        CGA64::add_imm(aux_reg_input, aux_reg_input,
                jcp.typesize_in * (jcp.dilate_d + 1) * jcp.ih * jcp.iw
                        * inp_mul, reg_tmp_imm);
        CGA64::add_imm(aux_reg_kernel, aux_reg_kernel,
                jcp.typesize_out * jcp.kh * jcp.kw * ic_block * oc_block, reg_tmp_imm);
        CGA64::sub(ki, ki, 1);
        CGA64::cmp(ki, 0);
        CGA64::b(xa::GT, kd_label);
    }
}

void jit_aarch64_sve_512_conv_bwd_weights_kernel_f32 ::compute_oh_step_common(
        int ic_block_step, int max_ur_w) {
    using namespace nstl;
    xa::LabelAArch64 kh_label, ic_block_label, ic_tail_loop_label, ic_tail_label, kd_label;

    const bool src_layout_nxc = is_src_layout_nxc();
    int ic_block = jcp.ic_block;
    int oc_block = jcp.oc_block;

    int ow = jcp.is_hw_transp ? jcp.oh : jcp.ow;
    int r_pad = max(0, jcp.r_pad);
#if 0
    int l_pad = jcp.ver == ver_4fma ? 0 : jcp.l_pad;
#else
    int l_pad = jcp.l_pad;
#endif
    int ur_w = min(ow, max_ur_w);
    int ur_w_trips = ow / ur_w;
    int ur_w_tail = ow % ur_w;
    if ((ur_w_tail == 0 && r_pad != 0) || (r_pad > 0 && r_pad >= ur_w_tail)) {
        if (ur_w_trips > 1) {
            ur_w_tail += ur_w;
            ur_w_trips--;
        } else {
            ur_w_tail += (ur_w - ur_w / 2);
            ur_w = ur_w / 2;
        }
    }

    assert(l_pad <= max_ur_w);
#if 0
    int inp_mult = src_layout_nxc
            ? jcp.ngroups * jcp.ic
            : ((jcp.is_1stconv || jcp.ver == ver_4fma)
                            ? 1
                            : ic_block * (jcp.is_hw_transp ? jcp.iw : 1));
#else
    int inp_mult = src_layout_nxc
            ? jcp.ngroups * jcp.ic
            : ((jcp.is_1stconv)
                            ? 1
                            : ic_block * (jcp.is_hw_transp ? jcp.iw : 1));
#endif
    int out_mult = is_ddst_layout_nxc() ? jcp.ngroups * jcp.oc : oc_block;
    int input_comeback
            = max((ur_w_trips * ur_w * jcp.stride_w - l_pad), 0) * inp_mult;
    int output_comeback = ur_w_trips * ur_w * out_mult;
    const int ic_tail = jcp.ic_tail;
    const bool generate_icb_loop = jcp.nb_ic_blocking_max > 1;

    auto ic_loop = [=](int ic_block_step) {
        xa::LabelAArch64 ow_block_label, ic_block_inner_label;
        int ur_w_blocks = ur_w_trips;

        int l_pad_tail = max(l_pad - ur_w, 0);
        CGA64::L_aarch64(ic_block_inner_label);
        if (l_pad != 0) {
            ur_w_blocks--;
            compute_ic_block_step(ur_w, l_pad, 0, ic_block_step, 0, 0, 0);
            int iw_offset = ur_w * jcp.stride_w - l_pad;
            if (iw_offset > 0)
                CGA64::add_imm(reg_input, reg_input, jcp.typesize_in * iw_offset * inp_mult, reg_tmp_imm);
            CGA64::add_imm(reg_output, reg_output, jcp.typesize_in * ur_w * out_mult, reg_tmp_imm);
        }

        assert(IMPLICATION(l_pad_tail > 0, ur_w_blocks <= 1));
        if (ur_w_blocks > 0) {
            CGA64::mov(reg_ur_w_trips, 0);
            CGA64::L_aarch64(ow_block_label);
            {
                compute_ic_block_step(
                        ur_w, l_pad_tail, 0, ic_block_step, 0, 0, 0);
                CGA64::add_imm(reg_input, reg_input,
                        jcp.typesize_in * (ur_w * jcp.stride_w - l_pad_tail)
                                * inp_mult, reg_tmp_imm);
                CGA64::add_imm(reg_output, reg_output, jcp.typesize_in * ur_w * out_mult, reg_tmp_imm);

                CGA64::add_imm(reg_ur_w_trips, reg_ur_w_trips, 1, reg_tmp_imm);
                CGA64::cmp(reg_ur_w_trips, ur_w_blocks);
                CGA64::b(xa::LT, ow_block_label);
                l_pad_tail = max(l_pad_tail - ur_w, 0);
            }
        }

        if (ur_w_tail > 0)
            compute_ic_block_step(
                    ur_w_tail, l_pad_tail, r_pad, ic_block_step, 0, 0, 0);

        CGA64::sub_imm(reg_output, reg_output, jcp.typesize_in * output_comeback, reg_tmp_imm);
    };

    if (jcp.ndims == 5) {
        CGA64::L_aarch64(kd_label);
        CGA64::mov(reg_input, aux_reg_input);
        CGA64::mov(reg_kernel, aux_reg_kernel);
    }

    CGA64::mov(kj, reg_kh);
    CGA64::L_aarch64(kh_label);
    {
        xa::LabelAArch64 icb_block_label, icb_block_label_cb;
        if (generate_icb_loop || ic_tail) {
            // TODO: May be broadcast work?
            CGA64::mov(reg_input_org, reg_input);
            CGA64::mov(reg_kernel_org, reg_kernel);
            CGA64::ldr(reg_icb, xa::ptr(param, GET_OFF(reduce_work)));
        }

        if (ic_tail) {
            CGA64::cmp(reg_icb, ic_block);
            CGA64::b(xa::LT, ic_tail_loop_label);
        }

        CGA64::L_aarch64(icb_block_label);
        CGA64::mov(b_ic, ic_block);
        CGA64::L_aarch64(ic_block_label);
        xa::LabelAArch64 ic_block_label_end;
        {
            ic_loop(ic_block_step);
            CGA64::sub_imm(reg_input, reg_input, jcp.typesize_in * input_comeback, reg_tmp_imm);
#if 0
            int inp_icblk_stride = jcp.is_1stconv && !src_layout_nxc
                    ? jcp.ih * jcp.iw * jcp.id
                    : (jcp.ver == ver_4fma ? jcp.tr_iw : 1);
#else
            int inp_icblk_stride = jcp.is_1stconv && !src_layout_nxc
                    ? jcp.ih * jcp.iw * jcp.id
                    : 1;
#endif
            size_t input_offset
                    = inp_icblk_stride * jcp.typesize_in * ic_block_step;
            CGA64::add_imm(reg_input, reg_input, input_offset, reg_tmp_imm);
            CGA64::add_imm(reg_kernel, reg_kernel, jcp.typesize_out * ic_block_step * oc_block, reg_tmp_imm);
            CGA64::sub_imm(b_ic, b_ic, ic_block_step, reg_tmp_imm);
            if (generate_icb_loop || ic_tail) CGA64::sub_imm(reg_icb, reg_icb, ic_block_step, reg_tmp_imm);
            CGA64::cmp(b_ic, ic_block_step);
            CGA64::b(xa::GE, ic_block_label);
        }
        CGA64::L_aarch64(ic_block_label_end);

        const int input_shift
                = jcp.typesize_in * (jcp.dilate_h + 1) * jcp.iw * inp_mult;

        if (generate_icb_loop || ic_tail) {
            const size_t kernel_icb_loop_shift_bytes = (size_t)jcp.typesize_out
                    * jcp.kd * jcp.kh * jcp.kw * ic_block * oc_block;

            if (generate_icb_loop) {
                // icb loop supported for src in nxc layout only
                assert(src_layout_nxc);
                CGA64::add_imm(reg_kernel, reg_kernel,
                        kernel_icb_loop_shift_bytes
                                - jcp.typesize_out * ic_block * oc_block, reg_tmp_imm);

                CGA64::cmp(reg_icb, ic_block);
                CGA64::b(xa::GE, icb_block_label);
            }

            CGA64::L_aarch64(ic_tail_loop_label);
            if (ic_tail) {
                xa::LabelAArch64 skip_ic_tail;
                const int ic_tail_loop_work = rnd_dn(ic_tail, ic_block_step);
                CGA64::cmp(reg_icb, 0);
                CGA64::b(xa::LE, skip_ic_tail);
                CGA64::mov(b_ic, reg_icb);
                if (ic_tail_loop_work) {
                    CGA64::cmp(reg_icb, ic_block_step);
                    CGA64::b(xa::GE, ic_block_label);
                    if (generate_icb_loop) {
                        // compensate offset added in generate_icb_loop
                        CGA64::sub_imm(reg_kernel, reg_kernel,
                                kernel_icb_loop_shift_bytes
                                        - jcp.typesize_out * ic_block
                                                * oc_block, reg_tmp_imm);
                    }
                }

                CGA64::L_aarch64(ic_tail_label);
                if (ic_tail % ic_block_step) {
                    CGA64::cmp(reg_icb, 0);
                    CGA64::b(xa::LE, skip_ic_tail);
                    ic_loop(ic_tail % ic_block_step);
                }
                CGA64::L_aarch64(skip_ic_tail);
            }

            CGA64::mov(reg_kernel, reg_kernel_org);
            CGA64::mov(reg_input, reg_input);

            CGA64::add_imm(reg_input, reg_input, input_shift, reg_tmp_imm);
            CGA64::add_imm(reg_kernel, reg_kernel, jcp.typesize_out * jcp.kw * ic_block * oc_block, reg_tmp_imm);
        } else if (jcp.is_1stconv && !src_layout_nxc) {
            size_t input_offset = (size_t)jcp.typesize_in * jcp.id * jcp.ih
                    * jcp.iw * ic_block;
            CGA64::sub_imm(reg_input, reg_input, input_offset, reg_tmp_imm);
            CGA64::add_imm(reg_input, reg_input, input_shift, reg_tmp_imm);
        } else if (!jcp.is_hw_transp) {
            CGA64::add_imm(reg_input, reg_input, input_shift - jcp.typesize_in * ic_block, reg_tmp_imm);
        }
        if (!jcp.is_hw_transp && !(generate_icb_loop || ic_tail))
            CGA64::add_imm(reg_kernel, reg_kernel,
                    jcp.typesize_out * (jcp.kw - 1) * ic_block * oc_block ,reg_tmp_imm);
        CGA64::sub(kj, kj, 1);
        CGA64::cmp(kj, 0);
        CGA64::b(xa::GT, kh_label);
    }
    if (jcp.ndims == 5) {
        CGA64::add_imm(aux_reg_input, aux_reg_input,
                jcp.typesize_in * (jcp.dilate_d + 1) * jcp.ih * jcp.iw
                        * inp_mult, reg_tmp_imm);
        CGA64::add_imm(aux_reg_kernel, aux_reg_kernel,
                jcp.typesize_out * jcp.kh * jcp.kw * ic_block * oc_block, reg_tmp_imm);
        CGA64::sub(ki, ki, 1);
        CGA64::cmp(ki, 0);
        CGA64::b(xa::GT, kd_label);
    }
}

void jit_aarch64_sve_512_conv_bwd_weights_kernel_f32 ::compute_oh_step_disp() {
    int ic_block_step;
    if (jcp.kernel_kind == expl_bcast)
        ic_block_step = jcp.kw <= 3 ? 4 : (jcp.kw <= 6 ? 2 : 1);
    else
        ic_block_step = jcp.kw <= 3 ? 8 : (jcp.kw <= 6 ? 4 : 2);

    if (jcp.is_1stconv) {
        bool large_code = jcp.kw >= 7 && (jcp.l_pad > 0 || jcp.t_pad > 0);
        ic_block_step = (jcp.kw * jcp.ic_block <= 26 && !large_code)
                ? jcp.ic_block
                : 1;
    }

    bool too_large_to_unroll = (jcp.kw > 1 || jcp.kh > 1 || jcp.kd > 1)
            && (jcp.stride_w > 1 || jcp.stride_h > 1 || jcp.stride_d > 1);

    int ow = jcp.is_hw_transp ? jcp.oh : jcp.ow;
    if (jcp.ndims == 5) {
        /* NOTE: reg_kd_count = aux_reg_input = r12. The following order of
         * 'movs' must be guaranteed. */
        CGA64::mov(ki, reg_kd_count);
        CGA64::mov(reg_kd_count_org, reg_kd_count);
        CGA64::mov(aux_reg_input, reg_input);
        CGA64::mov(aux_reg_kernel, reg_kernel);
    }

    if (jcp.kw <= 3 && ow <= 16 && !too_large_to_unroll)
        compute_oh_step_unroll_ow_icblock(ic_block_step, max_ur_w);
    else if (ow <= max_ur_w)
        compute_oh_step_unroll_ow(ic_block_step, max_ur_w);
    else
        compute_oh_step_common(ic_block_step, max_ur_w);

    if (jcp.ndims == 5) {
        CGA64::mov(reg_input, aux_reg_input);
        CGA64::mov(reg_kernel, aux_reg_kernel);
        CGA64::mov(reg_kd_count, reg_kd_count_org);
        od_step_comeback_pointers();
    } else {
        oh_step_comeback_pointers();
    }
}

void jit_aarch64_sve_512_conv_bwd_weights_kernel_f32::maybe_zero_kernel() {
    xa::LabelAArch64 skip_zeroing, zeroing_loop;

    CGA64::ldr(reg_tmp, xa::ptr(param, GET_OFF(channel)));
    CGA64::cmp(reg_tmp, 0);
    CGA64::b(xa::EQ, skip_zeroing);

    xa::ZRegS zero = xa::ZRegS(0);
    CGA64::eor(zero, reg_p_all_ones, zero); //vpxord(zero, zero, zero);
#if 1
    const bool generate_icb_loop = jcp.nb_ic_blocking_max > 1;
    const size_t kernel_block_bytes = (size_t)jcp.ic_block * jcp.oc_block
            * jcp.kw * jcp.kh * jcp.kd * jcp.typesize_out;
    xa::LabelAArch64 icb_block_label, icb_block_label_cb;
    if (generate_icb_loop) {
        CGA64::mov(reg_kernel_org, reg_kernel);

        CGA64::ldr(reg_icb, xa::ptr(param, GET_OFF(reduce_work)));
        CGA64::L_aarch64(icb_block_label);
    }
#endif

    CGA64::mov(reg_tmp, 0);
    CGA64::L_aarch64(zeroing_loop);
    {
        assert(jcp.oc_block * jcp.typesize_out
                == cpu_isa_traits<sve>::vlen);
        for (int ic1 = 0; ic1 < jcp.ic_block; ic1++)
        {
            CGA64::add(reg_add_tmp, reg_kernel, reg_tmp);
            CGA64::add_imm(reg_add_tmp, reg_add_tmp, ic1 * jcp.oc_block * jcp.typesize_out, reg_tmp_imm);
            CGA64::str(xa::ZReg(0), xa::ptr(reg_add_tmp));
        }

        CGA64::add_imm(reg_tmp, reg_tmp, jcp.ic_block * jcp.oc_block * jcp.typesize_out, reg_tmp_imm);
        if(kernel_block_bytes > ADDMAX){
            CGA64::mov_imm(reg_tmp_imm, kernel_block_bytes);
            CGA64::cmp(reg_tmp, reg_tmp_imm);
        }else{
            CGA64::cmp(reg_tmp, kernel_block_bytes);
        }
        CGA64::b(xa::NE, zeroing_loop);
    }
    if (generate_icb_loop) {
        CGA64::add_imm(reg_kernel, reg_kernel, kernel_block_bytes, reg_tmp_imm);
        CGA64::sub_imm(reg_icb, reg_icb, jcp.ic_block, reg_tmp_imm);
        CGA64::cmp(reg_icb, 0);
        CGA64::b(xa::GT, icb_block_label);

        CGA64::mov(reg_kernel, reg_kernel_org);
    }

    CGA64::L_aarch64(skip_zeroing);
}

void jit_aarch64_sve_512_conv_bwd_weights_kernel_f32::bias_kernel_2d() {
    assert(jcp.ndims == 4); // only supports 2d
    xa::LabelAArch64 skip_bias, bias_loop;
    const int oc_tail = jcp.oc_tail;

    CGA64::ldr(reg_tmp, xa::ptr(param, GET_OFF(flags)));
    CGA64::ldr(reg_bias, xa::ptr(param, GET_OFF(bias)));
    CGA64::tst(reg_tmp, reg_tmp);
    CGA64::b(xa::NE, skip_bias);

    CGA64::ldr(xa::ZReg(0), xa::ptr(reg_bias));

    CGA64::mov_imm(reg_oi, jcp.ow);
    CGA64::mov(reg_tmp, 0);
    CGA64::L_aarch64(bias_loop);
    {
#if 0
        auto zmm_out = Zmm(1);
        if (oc_tail) zmm_out = zmm_out | k_oc_mask | T_z;
        vmovups(zmm_out, ptr[reg_output + reg_tmp]);
#else
//[info]v1.6追加コード分は取り敢えず除外。
        CGA64::add(reg_add_tmp, reg_output, reg_tmp);
        CGA64::ldr(xa::ZReg(1), xa::ptr(reg_add_tmp));
#endif
        CGA64::fadd(xa::ZRegS(0), xa::ZRegS(0), xa::ZRegS(1));
        const int oc_stride
                = is_ddst_layout_nxc() ? jcp.ngroups * jcp.oc : jcp.oc_block;
        CGA64::add_imm(reg_tmp, reg_tmp, jcp.typesize_out * oc_stride, reg_tmp_imm);
        CGA64::sub(reg_oi, reg_oi, 1);
        CGA64::cmp(reg_oi, 0);
        CGA64::b(xa::GT, bias_loop);
    }
    CGA64::str(xa::ZReg(0), xa::ptr(reg_bias)); //vmovups(ptr[reg_bias], Zmm(0));

    CGA64::L_aarch64(skip_bias);
}

void jit_aarch64_sve_512_conv_bwd_weights_kernel_f32::bias_kernel_3d() {
    assert(jcp.ndims == 5); // only supports 3d
    xa::LabelAArch64 skip_bias, bias_loop, skip_load_bias;
    const bool oc_tail = jcp.oc_tail;

    CGA64::ldr(reg_tmp, xa::ptr(param, GET_OFF(flags)));
    CGA64::tst(reg_tmp, reg_tmp);
    CGA64::b(xa::NE, skip_bias);

    CGA64::ldr(reg_bias, xa::ptr(param, GET_OFF(bias)));
    CGA64::ldr(reg_output, xa::ptr(param, GET_OFF(dst)));
    CGA64::eor(xa::ZRegS(1), reg_p_all_ones, xa::ZRegS(1));

    CGA64::ldr(reg_tmp, xa::ptr(param, GET_OFF(channel)));
    CGA64::cmp(reg_tmp, 0);
    CGA64::b(xa::NE, skip_load_bias);
    CGA64::ldr(xa::ZReg(1), xa::ptr(reg_bias));

    CGA64::L_aarch64(skip_load_bias);

    CGA64::ldr(reg_oi, xa::ptr(param, GET_OFF(os_index_end)));
    CGA64::ldr(reg_tmp_imm, xa::ptr(param, GET_OFF(os_index_begin)));
    CGA64::sub(reg_oi, reg_oi, reg_tmp_imm);
    CGA64::cmp(reg_oi, 0);
    CGA64::b(xa::LE, skip_bias); // no iterations along depth dimension

    const size_t oc_mult
            = is_ddst_layout_nxc() ? jcp.ngroups * jcp.oc : jcp.oc_block;
    CGA64::mov_imm(reg_tmp, oc_mult * jcp.ow * jcp.oh * jcp.typesize_out);
    CGA64::mul(reg_oi, reg_oi, reg_tmp);

    CGA64::mov(reg_tmp, 0);
    CGA64::L_aarch64(bias_loop);
    {
#if 0
        auto zmm_out = Zmm(0);
        if (oc_tail) zmm_out = zmm_out | k_oc_mask | T_z;
        vmovups(zmm_out, ptr[reg_output + reg_tmp]);
#else
//[info]v1.6追加コードを取り敢えず除外
        CGA64::add(reg_add_tmp, reg_output, reg_tmp);
        CGA64::ldr(xa::ZReg(1), xa::ptr(reg_add_tmp));
#endif
        CGA64::fadd(xa::ZRegS(1), xa::ZRegS(1), xa::ZRegS(0)); 
        CGA64::add_imm(reg_tmp, reg_tmp, oc_mult * jcp.typesize_out, reg_tmp_imm);
        CGA64::cmp(reg_tmp, reg_oi);
        CGA64::b(xa::LT, bias_loop);
    }
    CGA64::str(xa::ZReg(1), xa::ptr(reg_bias)); //vmovups(ptr[reg_bias], Zmm(1));

    CGA64::L_aarch64(skip_bias);
}

void jit_aarch64_sve_512_conv_bwd_weights_kernel_f32 ::compute_oh_loop_common() {
    assert(one_of(jcp.harness, harness_mb_reduction, harness_3d_reduction));
    int b_pad = jcp.b_pad;
    int t_pad = jcp.t_pad;
    bool is_dilated = jcp.dilate_h != 0;
    int dilate_h = jcp.dilate_h + 1;
    int stride_h = jcp.stride_h;
    const int inp_mult = is_src_layout_nxc()
            ? jcp.ngroups * jcp.ic
            : (jcp.is_1stconv ? 1 : jcp.ic_block);
    const int out_mult
            = is_ddst_layout_nxc() ? jcp.ngroups * jcp.oc : jcp.oc_block;
#if 0
    int iw = jcp.ver == ver_4fma ? jcp.tr_iw : jcp.is_hw_transp ? 1 : jcp.iw;
#else
    int iw = jcp.is_hw_transp ? 1 : jcp.iw;
#endif
    xa::LabelAArch64 oh_label, oh_label_end, oh_tpad_label, oh_tpad_tail_label,
            oh_bpad_label, oh_bpad_label_end, oh_dilate_label_shift,
            oh_dilate_label_noshift, oh_dilate_label_end;

    int ow = jcp.is_hw_transp ? jcp.oh : jcp.ow;
    int oh = jcp.is_hw_transp ? jcp.ow : jcp.oh;
    int kw = jcp.is_hw_transp ? jcp.tr_kw : jcp.kw;
    int kh = jcp.is_hw_transp ? jcp.tr_kh : jcp.kh;
    int ih = jcp.is_hw_transp ? jcp.tr_ih : jcp.ih;
    int ihp = jcp.is_hw_transp ? jcp.tr_ih : jcp.ihp;

    assert(IMPLICATION(jcp.is_hw_transp,
            everyone_is(1, oh, stride_h, dilate_h)
                    && everyone_is(0, b_pad, t_pad)));

    CGA64::mov(reg_kh, kh);
    CGA64::mov(reg_oj, 0);
    /* Compute 'top' edge */
    if (t_pad > 0) {
        const int kh_range = 1 + (kh - 1) * dilate_h;
        const int overflow = nstl::max(0, kh - div_up(t_pad + ih, dilate_h));
        const int underflow = div_up(t_pad, dilate_h);
        const int initial_inp_ker_overlap = kh - overflow - underflow;
        CGA64::mov_imm(reg_kh, initial_inp_ker_overlap);
        CGA64::add_imm(reg_kernel, reg_kernel,
                jcp.typesize_out * underflow * kw * jcp.ic_block
                        * jcp.oc_block, reg_tmp_imm);
        // generate loop to process kernel while it remains within t_pad + ih
        if (kh_range < t_pad + ih) {
            if (is_dilated) {
                const int tail = t_pad % dilate_h;
                const int shift = tail == 0 ? 0 : dilate_h - tail;
                CGA64::mov_imm(reg_tmp, shift);
                if (tail != 0)
                    CGA64::add_imm(reg_input, reg_input, jcp.typesize_in * shift * iw * inp_mult, reg_tmp_imm);
            }
            CGA64::L_aarch64(oh_tpad_label);
            {
                CGA64::cmp(reg_oj, oh);
                CGA64::b(xa::GE, oh_label_end);

                compute_oh_step_disp();
                CGA64::add_imm(reg_output, reg_output, jcp.typesize_in * ow * out_mult, reg_tmp_imm);
                if (is_dilated) {
                    CGA64::add_imm(reg_tmp, reg_tmp, 1, reg_tmp_imm);
                    CGA64::cmp(reg_tmp, dilate_h);
                    CGA64::b(xa::LT, oh_dilate_label_shift);
                    // unshift input as new kernel element enters
                    CGA64::sub_imm(reg_input, reg_input,
                            jcp.typesize_in * (dilate_h - 1) * iw * inp_mult, reg_tmp_imm);
                    CGA64::mov(reg_tmp, 0);
                }
                // kernel overlap only changes when (t_pad + oj) % dilate_h == 0
                CGA64::sub_imm(reg_kernel, reg_kernel,
                        jcp.typesize_out * stride_h * kw * jcp.ic_block
                                * jcp.oc_block, reg_tmp_imm);
                CGA64::add_imm(reg_kh, reg_kh, stride_h, reg_tmp_imm);
                if (is_dilated) {
                    CGA64::b(oh_dilate_label_noshift);
                    CGA64::L_aarch64(oh_dilate_label_shift);
                    // shift input as old kernel element progresses
                    CGA64::add_imm(reg_input, reg_input, jcp.typesize_in * stride_h * iw * inp_mult, reg_tmp_imm);
                    CGA64::L_aarch64(oh_dilate_label_noshift);
                }
                CGA64::add_imm(reg_oj, reg_oj, 1, reg_tmp_imm);

                // final number of kernel elements that overlap with input
                const int final_inp_ker_overlap
                        = nstl::min(kh, div_up(ih, dilate_h));
                CGA64::cmp(reg_kh, final_inp_ker_overlap);
                CGA64::b(xa::LT, oh_tpad_label);
            }
        }
        // need second loop to process kernel if it is larger than the input
        // (does not apply to dilations as they must have unit stride)
        if (kh_range
                >= ih + (t_pad % stride_h == 0 ? stride_h : t_pad % stride_h)) {
            assert(!is_dilated);
            CGA64::mov_imm(reg_kh, ih);
            CGA64::L_aarch64(oh_tpad_tail_label);
            {
                CGA64::cmp(reg_oj, oh);
                CGA64::b(xa::GE, oh_label_end);

                compute_oh_step_disp();
                CGA64::add_imm(reg_output, reg_output, jcp.typesize_in * ow * out_mult, reg_tmp_imm);
                CGA64::sub_imm(reg_kernel, reg_kernel,
                        jcp.typesize_out * stride_h * kw * jcp.ic_block
                                * jcp.oc_block, reg_tmp_imm);

                CGA64::add_imm(reg_oj, reg_oj, 1, reg_tmp_imm);
                CGA64::cmp(reg_oj, nstl::min(utils::div_up(t_pad, stride_h), oh));
                CGA64::b(xa::LT, oh_tpad_tail_label);
            }
        }
        // correct any excess shifts to kernel and input
        // (does not apply to dilations as they must have unit stride,
        //  kernel must fit inside input, and padding is smaller than input)
        if (t_pad <= oh * stride_h) {
            // kernel has moved beyond padding (adjust for stride effects)
            if (t_pad % stride_h != 0) {
                assert(!is_dilated);
                int inp_corr = stride_h - t_pad % stride_h;
                CGA64::add_imm(reg_kernel, reg_kernel,
                        jcp.typesize_out * inp_corr * kw * jcp.ic_block
                                * jcp.oc_block, reg_tmp_imm);
                CGA64::add_imm(reg_input, reg_input, jcp.typesize_in * inp_corr * iw * inp_mult, reg_tmp_imm);
            }
        } else {
            // kernel still overlaps padding (complete reset)
            assert(!is_dilated);
            CGA64::sub_imm(reg_kernel, reg_kernel,
                    jcp.typesize_out * (t_pad - oh * stride_h) * kw
                            * jcp.ic_block * jcp.oc_block, reg_tmp_imm);
        }
    }

    const int oj_end_value = nstl::min(
            oh, utils::div_up(ihp - b_pad - (kh - 1) * dilate_h, stride_h));
    CGA64::cmp(reg_oj, oj_end_value);
    CGA64::b(xa::GE, oh_label_end);

    /* Compute middle block(s) */
    CGA64::mov_imm(reg_kh, kh);
    CGA64::L_aarch64(oh_label);
    {
        compute_oh_step_disp();
        CGA64::add_imm(reg_input, reg_input, jcp.typesize_in * stride_h * iw * inp_mult, reg_tmp_imm);
        CGA64::add_imm(reg_output, reg_output, jcp.typesize_in * ow * out_mult, reg_tmp_imm);

        CGA64::add_imm(reg_oj, reg_oj, 1, reg_tmp_imm);
        CGA64::cmp(reg_oj, oj_end_value);
        CGA64::b(xa::LT, oh_label);
    }
    CGA64::L_aarch64(oh_label_end);

    /* Compute bottom edge */
    if (b_pad > 0) {
        CGA64::cmp(reg_oj, oh);
        CGA64::b(xa::GE, oh_bpad_label_end);

        if (is_dilated) {
            CGA64::mov_imm(reg_kh, kh - 1); // assumes unit stride for dilations
            CGA64::mov(reg_tmp, 0);
        } else {
            CGA64::mov_imm(reg_kh, ihp - b_pad);
            CGA64::mov(reg_tmp, reg_oj);
            CGA64::mov_imm(reg_tmp_imm, stride_h);
            CGA64::mul(reg_tmp, reg_tmp, reg_tmp_imm);
            CGA64::sub(reg_kh, reg_kh, reg_tmp);
        }
        CGA64::L_aarch64(oh_bpad_label);
        {
            compute_oh_step_disp();
            CGA64::add_imm(reg_input, reg_input, jcp.typesize_in * stride_h * iw * inp_mult, reg_tmp_imm);
            CGA64::add_imm(reg_output, reg_output, jcp.typesize_in * ow * out_mult, reg_tmp_imm);
            if (is_dilated) {
                CGA64::add_imm(reg_tmp, reg_tmp, 1, reg_tmp_imm);
                CGA64::cmp(reg_tmp, dilate_h);
                CGA64::b(xa::LT, oh_dilate_label_end);
                CGA64::mov(reg_tmp, 0);
            }
            CGA64::sub_imm(reg_kh, reg_kh, stride_h, reg_tmp_imm);
            CGA64::cmp(reg_kh, 0);
            CGA64::b(xa::LE, oh_bpad_label_end);
            if (is_dilated) CGA64::L_aarch64(oh_dilate_label_end);

            CGA64::add_imm(reg_oj, reg_oj, 1, reg_tmp_imm);
            CGA64::cmp(reg_oj, oh);
            CGA64::b(xa::LT, oh_bpad_label);
        }
        CGA64::L_aarch64(oh_bpad_label_end);
    }
}

void jit_aarch64_sve_512_conv_bwd_weights_kernel_f32::compute_oh_loop_partial() {
    assert(jcp.harness == harness_2d_reduction);
    int ic_block = jcp.ic_block;
    int oc_block = jcp.oc_block;
    const int inp_mult = is_src_layout_nxc()
            ? jcp.ngroups * jcp.ic
            : (jcp.is_1stconv ? 1 : jcp.ic_block);
    const int out_mult
            = is_ddst_layout_nxc() ? jcp.ngroups * jcp.oc : jcp.oc_block;
    const int input_bottom_padding_overlap
            = div_up(jcp.ih + jcp.t_pad - (jcp.kh - 1), jcp.stride_h);

    const size_t filter_shift = jcp.typesize_out * jcp.kw * ic_block * oc_block;
    const size_t input_shift = jcp.typesize_in * jcp.iw * inp_mult;
    const size_t output_shift = jcp.typesize_out * jcp.ow * out_mult;;

    xa::LabelAArch64 loop_begin_label, loop_end_label, common_block_label,
            top_padding_end_label, bottom_padding_end_label,
            bottom_padding_label;

    if (jcp.with_bias) {
        xa::LabelAArch64 skip_zero_bias;
        CGA64::ldr(reg_bias, xa::ptr(param, GET_OFF(bias)));
        CGA64::ldr(reg_tmp, xa::ptr(param, GET_OFF(channel)));
        CGA64::tst(reg_tmp, reg_tmp);
        CGA64::b(xa::EQ, skip_zero_bias);
        CGA64::ldr(reg_tmp, xa::ptr(param, GET_OFF(flags)));
        CGA64::tst(reg_tmp, reg_tmp);
        CGA64::b(xa::NE, skip_zero_bias);
        CGA64::eor(xa::ZRegS(1), reg_p_all_ones.b, xa::ZRegS(1));
        CGA64::str(xa::ZReg(1), xa::ptr(reg_bias)); //vmovups(ptr[reg_bias], Zmm(1));
        CGA64::L_aarch64(skip_zero_bias);
    }

    /* Offset filter position to adjust for top padding */
    CGA64::ldr(reg_tmp_imm, xa::ptr(param, GET_OFF(kh_offset)));
    CGA64::add(reg_kernel, reg_kernel, reg_tmp_imm); //add(reg_kernel, ptr[param + GET_OFF(kh_offset)]);

    CGA64::ldr(reg_oj, xa::ptr(param, GET_OFF(os_index_begin)));
    CGA64::ldr(reg_kh, xa::ptr(param, GET_OFF(kh_padding)));

    CGA64::cmp(reg_kh, 0);
    CGA64::b(xa::LE, loop_end_label); // no iterations along kh
    CGA64::ldr(reg_tmp_imm, xa::ptr(param, GET_OFF(os_index_end)));
    CGA64::cmp(reg_oj, reg_tmp_imm); //cmp(reg_oj, ptr[param + GET_OFF(os_index_end)]);

    CGA64::b(xa::GE, loop_end_label); // no iterations along height dimension

    CGA64::L_aarch64(loop_begin_label);

    if (jcp.with_bias)
        bias_kernel_2d();
    compute_oh_step_disp();

    /* Compute 'top' edge */
    if (jcp.t_pad > 0) {

        /* Check if within top padding region */
        assert(div_up(jcp.t_pad, jcp.stride_h) >= 0 &&
                div_up(jcp.t_pad, jcp.stride_h) < ADDMAX);
        CGA64::cmp(reg_oj, div_up(jcp.t_pad, jcp.stride_h));
        CGA64::b(xa::GE, top_padding_end_label);

        /* Increment step counter and adjust filter position */
        CGA64::sub_imm(reg_kernel, reg_kernel, filter_shift * jcp.stride_h, reg_tmp_imm);
        CGA64::add_imm(reg_kh, reg_kh, jcp.stride_h, reg_tmp_imm);

        /* Final number of kernel elements that overlap with input */
        const int inp_ker_overlap = nstl::min(jcp.kh, jcp.ih);
        CGA64::mov_imm(reg_tmp_imm, inp_ker_overlap);
        CGA64::cmp(reg_kh, reg_tmp_imm);

        CGA64::b(xa::LE, common_block_label);

        /* Correct any excess shifts to kernel and input */
        if (jcp.t_pad <= jcp.oh * jcp.stride_h) {
            /* Filter has moved beyond padding (adjust for stride effects) */
            if (jcp.t_pad % jcp.stride_h != 0) {
                int inp_corr = jcp.stride_h - jcp.t_pad % jcp.stride_h;
                CGA64::add_imm(reg_kernel, reg_kernel, filter_shift * inp_corr, reg_tmp_imm);
                CGA64::add_imm(reg_input, reg_input, input_shift * inp_corr, reg_tmp_imm);
            }
        } else {
            /* Filter still overlaps padding (complete reset) */
            CGA64::sub_imm(reg_kernel, reg_kernel, (jcp.t_pad - jcp.oh * jcp.stride_h) * filter_shift, reg_tmp_imm);
        }

        /* Apply correction */
        CGA64::mov_imm(reg_kh, inp_ker_overlap);
        CGA64::b(common_block_label);

        CGA64::L_aarch64(top_padding_end_label);
    }

    /* Compute 'bottom' edge */
    if (jcp.b_pad > 0) {

        /* Check if within bottom padding region */
        assert((input_bottom_padding_overlap - 1)>=0 &&
                (input_bottom_padding_overlap - 1)<ADDMAX);
        CGA64::cmp(reg_oj, input_bottom_padding_overlap - 1);
        CGA64::b(xa::LT, bottom_padding_end_label);
        CGA64::b(xa::GT, bottom_padding_label);

        /* Execute overlap correction between the filter and the initial
         * bottom padding region. */
        CGA64::mov_imm(reg_kh, jcp.ih + jcp.t_pad
                        - input_bottom_padding_overlap * jcp.stride_h);
        CGA64::b(bottom_padding_end_label);

        CGA64::L_aarch64(bottom_padding_label);
        CGA64::sub_imm(reg_kh, reg_kh, jcp.stride_h, reg_tmp_imm);
        CGA64::cmp(reg_kh, 0);
        CGA64::b(xa::LE, loop_end_label);

        CGA64::L_aarch64(bottom_padding_end_label);
    }

    /* Compute middle block */
    CGA64::add_imm(reg_input, reg_input, input_shift * jcp.stride_h, reg_tmp_imm);

    /* Execute common block and loop */
    CGA64::L_aarch64(common_block_label);
    CGA64::add_imm(reg_output, reg_output, output_shift, reg_tmp_imm);

    CGA64::add_imm(reg_oj, reg_oj, 1, reg_tmp_imm);
    CGA64::ldr(reg_tmp_imm, xa::ptr(param, GET_OFF(os_index_end)));
    CGA64::cmp(reg_oj, reg_tmp_imm); //cmp(reg_oj, ptr[param + GET_OFF(os_index_end)]);

    CGA64::b(xa::LT, loop_begin_label);

    CGA64::L_aarch64(loop_end_label);
}

void jit_aarch64_sve_512_conv_bwd_weights_kernel_f32::compute_od_loop_partial() {
    assert(jcp.harness == harness_3d_reduction);
    int ic_block = jcp.ic_block;
    int oc_block = jcp.oc_block;
    const int inp_mult = is_src_layout_nxc()
            ? jcp.ngroups * jcp.ic
            : (jcp.is_1stconv ? 1 : jcp.ic_block);
    const int out_mult
            = is_ddst_layout_nxc() ? jcp.ngroups * jcp.oc : jcp.oc_block;
#if 0
    int iw = jcp.ver == ver_4fma ? jcp.tr_iw : jcp.iw;
#else
    int iw = jcp.iw;
#endif
    int ow = jcp.ow;
    const int input_backpad_overlap
            = div_up(jcp.id + jcp.f_pad - (jcp.kd - 1), jcp.stride_d);

    const size_t filter_shift
            = jcp.typesize_out * jcp.kh * jcp.kw * ic_block * oc_block;
    const size_t input_shift = jcp.typesize_in * jcp.ih * iw * inp_mult;
    const size_t output_shift = jcp.typesize_in * jcp.oh * ow * out_mult;

    xa::LabelAArch64 d_loop_label, loop_end_label, common_block_label, fpad_end_label,
            backpad_end_label, backpad_label;

    if (jcp.with_bias) bias_kernel_3d();

    /* initially offset 'kd' by f_pad */
    CGA64::ldr(reg_tmp_imm, xa::ptr(param, GET_OFF(kd_offset)));
    CGA64::add(reg_kernel, reg_kernel, reg_tmp_imm);

    CGA64::ldr(reg_input_d, xa::ptr(param, GET_OFF(src)));
    CGA64::ldr(reg_output_d, xa::ptr(param, GET_OFF(dst)));
    CGA64::ldr(reg_d_index, xa::ptr(param, GET_OFF(os_index_begin)));
    CGA64::ldr(reg_kd_count, xa::ptr(param, GET_OFF(kd_padding)));

    CGA64::cmp(reg_kd_count, 0);
    CGA64::b(xa::LE, loop_end_label); // no iterations along kd
    CGA64::ldr(reg_tmp_imm, xa::ptr(param, GET_OFF(os_index_end)));
    CGA64::cmp(reg_d_index, reg_tmp_imm);
    CGA64::b(xa::GE, loop_end_label); // no iterations along depth dimension

    CGA64::L_aarch64(d_loop_label);

    CGA64::mov(reg_input, reg_input_d);
    CGA64::mov(reg_output, reg_output_d);

    CGA64::mov(reg_input_d_org, reg_input_d);
    CGA64::mov(reg_output_d_org ,reg_output_d);
    CGA64::mov(reg_d_index_org ,reg_d_index);

    compute_oh_loop_common();

    CGA64::mov(reg_d_index, reg_d_index_org);
    CGA64::mov(reg_output_d, reg_output_d_org);
    CGA64::mov(reg_input_d, reg_input_d_org);

    /* Compute 'front' edge */
    if (jcp.f_pad > 0) {

        /* Check if within fpad region */
        CGA64::cmp(reg_d_index, div_up(jcp.f_pad, jcp.stride_d));
        CGA64::b(xa::GE, fpad_end_label);

        /* Fpad steps */
        CGA64::sub_imm(reg_kernel, reg_kernel, filter_shift * jcp.stride_d, reg_tmp_imm);
        CGA64::add_imm(reg_kd_count, reg_kd_count, jcp.stride_d, reg_tmp_imm);

        /* Final number of kernel elements that overlap with input */
        const int inp_ker_overlap = nstl::min(jcp.kd, jcp.id);
        CGA64::cmp(reg_kd_count, inp_ker_overlap);
        CGA64::b(xa::LE, common_block_label);

        /* Correct any excess shifts to kernel and input */
        if (jcp.f_pad <= jcp.od * jcp.stride_d) {
            /* Filter has moved beyond padding (adjust for stride effects) */
            if (jcp.f_pad % jcp.stride_d != 0) {
                int inp_corr = jcp.stride_d - jcp.f_pad % jcp.stride_d;
                CGA64::add_imm(reg_kernel, reg_kernel, filter_shift * inp_corr, reg_tmp_imm);
                CGA64::add_imm(reg_input_d, reg_input_d, input_shift * inp_corr, reg_tmp_imm);
            }
        } else {
            /* Filter still overlaps padding (complete reset) */
            CGA64::sub_imm(reg_kernel, reg_kernel, (jcp.f_pad - jcp.od * jcp.stride_d) * filter_shift, reg_tmp_imm);
        }

        /* Apply correction */
        CGA64::mov_imm(reg_kd_count, inp_ker_overlap);
        CGA64::b(common_block_label);

        CGA64::L_aarch64(fpad_end_label);
    }

    /* Compute bottom edge */
    if (jcp.back_pad > 0) {

        /* Check if within back_pad region */
        CGA64::cmp(reg_d_index, input_backpad_overlap - 1);
        CGA64::b(xa::LT, backpad_end_label);
        CGA64::b(xa::GT, backpad_label);

        /* Execute overlap correction between the filter and the initial
         * back_pad region. */
        CGA64::mov_imm(reg_kd_count,
                jcp.id + jcp.f_pad - input_backpad_overlap * jcp.stride_d);
        CGA64::b(backpad_end_label);

        CGA64::L_aarch64(backpad_label);
        CGA64::sub_imm(reg_kd_count, reg_kd_count, jcp.stride_d, reg_tmp_imm);
        CGA64::cmp(reg_kd_count, 0);
        CGA64::b(xa::LE, loop_end_label);

        CGA64::L_aarch64(backpad_end_label);
    }

    /* Compute middle block */
    CGA64::add_imm(reg_input_d, reg_input_d, input_shift * jcp.stride_d, reg_tmp_imm);

    /* Execute common block and loop */
    CGA64::L_aarch64(common_block_label);
    CGA64::add_imm(reg_output_d, reg_output_d, output_shift, reg_tmp_imm);
    CGA64::add_imm(reg_d_index, reg_d_index, 1, reg_tmp_imm);
    CGA64::ldr(reg_tmp_imm, xa::ptr(param, GET_OFF(os_index_end)));
    CGA64::cmp(reg_d_index, reg_tmp_imm);
    CGA64::b(xa::LT, d_loop_label);

    CGA64::L_aarch64(loop_end_label);
}

void jit_aarch64_sve_512_conv_bwd_weights_kernel_f32::compute_loop() {

    maybe_zero_kernel();

    switch (jcp.harness) {
        case harness_2d_reduction: compute_oh_loop_partial(); break;
        case harness_3d_reduction: compute_od_loop_partial(); break;
        case harness_mb_reduction: compute_oh_loop_common(); break;
        case harness_nxc: break;
        default: assert(!"Invalid harness type");
    }
}

#if 0
//[info]v1.6追加関数。取り敢えずコメント化。
void jit_avx512_common_conv_bwd_weights_kernel_f32::generate_microkernel() {

    reg64_t reg_dwei = abi_param1;
    reg64_t reg_src = abi_param2;
    reg64_t reg_ddst = abi_param3;
    reg64_t reg_iw_base = abi_param4;
    reg64_t aux_reg_icb = r10;
    reg64_t aux_reg_kwb = r11;
    reg64_t reg_src_save = r12;
    reg64_t reg_dwei_save = r13;
    reg64_t reg_iw_base_save = r14;
    reg64_t reg_tmp = r15;

    //Currently kernel is small so passing parameters via registers is preferred
    //whenever possible
#ifdef _WIN32
    // Must be a scratch register since load is before preamble
    reg64_t reg_owb = rax;
    mov(reg_owb, ptr[get_stack_params_address(false)]);
#else
    reg64_t reg_owb = abi_param5;
#endif

    preamble();

    const int kw_unroll = jcp.ur_kw;
    const int ow_unroll = jcp.ur_ow;
    const int iw_unroll = ow_unroll + kw_unroll - 1;
    const int ic_unroll = jcp.ur_ic;

    const int ker_reg_count = ic_unroll;
    const int src_reg_count = iw_unroll * ic_unroll;
    const int ddst_reg_count = ow_unroll;

    MAYBE_UNUSED(ddst_reg_count);
    assert(ker_reg_count + src_reg_count + ddst_reg_count <= 32);

    auto dwei_offset = [&](int i_kw, int i_ic) {
        const int oc_block_size = sizeof(float);
        const int ic_block_size = jcp.oc_block * oc_block_size;
        const int kw_block_size = jcp.ic_block * ic_block_size;
        const int kh_block_size = jcp.kw * kw_block_size;
        const int kd_block_size = jcp.kh * kh_block_size;
        const int icb_block_size = jcp.kd * kd_block_size;

        int icb = i_ic / jcp.ic_block;
        i_ic = i_ic % jcp.ic_block;

        return icb * icb_block_size + i_kw * kw_block_size
                + i_ic * ic_block_size;
    };

    auto src_offset = [&](int i_ic, int i_iw) {
        const int ic_block_size = sizeof(float);
        const int g_block_size = jcp.ic * ic_block_size;
        const int iw_block_size = jcp.ngroups * g_block_size;

        return i_iw * iw_block_size + i_ic * ic_block_size;
    };

    auto ddst_offset = [&](int i_ow) {
        const int oc_block_size = sizeof(float);
        const int g_block_size = jcp.oc * oc_block_size;
        const int ow_block_size = jcp.ngroups * g_block_size;

        return i_ow * ow_block_size;
    };

    auto get_src_zmm = [=](int iw_index, int i_ic) {
        int zmm_index = iw_index * ic_unroll + i_ic + ker_reg_count;
        return Zmm(zmm_index);
    };

    auto get_ddst_zmm = [=](int i_ow) {
        int zmm_index = i_ow + src_reg_count + ker_reg_count;
        return Zmm(zmm_index);
    };

    auto get_ker_zmm = [=](int i_ic) { return Zmm(i_ic); };

    auto load_ddsts = [=](int ur_ow) {
        for (int i_ow = 0; i_ow < ur_ow; i_ow++) {
            vmovups(get_ddst_zmm(i_ow), zword[reg_ddst + ddst_offset(i_ow)]);
        }
    };

    auto load_srcs = [=](int ur_iw, int ur_ic, bool is_iw_edge) {
        Label iw_load_end;
        if (is_iw_edge) {
            for_(int i_iw_index = 0; i_iw_index < ur_iw; i_iw_index++)
            for (int i_ic = 0; i_ic < ur_ic; i_ic++) {
                vpxord(get_src_zmm(i_iw_index, i_ic),
                        get_src_zmm(i_iw_index, i_ic),
                        get_src_zmm(i_iw_index, i_ic));
            }
        }

        for (int i_iw_index = 0; i_iw_index < ur_iw; i_iw_index++) {
            Label ic_load_end;
            if (is_iw_edge) {
                cmp(reg_iw_base, jcp.iw - i_iw_index * jcp.stride_w);
                jge(iw_load_end, T_NEAR);
                if (jcp.l_pad > 0) {
                    cmp(reg_iw_base, -i_iw_index * jcp.stride_w);
                    jl(ic_load_end, T_NEAR);
                }
            }
            for (int i_ic = 0; i_ic < ur_ic; i_ic++) {
                vbroadcastss(get_src_zmm(i_iw_index, i_ic),
                        zword[reg_src
                                + src_offset(i_ic, jcp.stride_w * i_iw_index)]);
            }
            L(ic_load_end);
        }
        L(iw_load_end);
    };

    auto compute_kernel = [=](int ur_ow, int ur_ic, int ur_kw, int is_iw_edge) {
        Label kw_loop_end;
        load_srcs(ur_ow + ur_kw - 1, ur_ic, is_iw_edge);

        for (int i_kw = 0; i_kw < ur_kw; i_kw++) {
            for (int i_ic = 0; i_ic < ur_ic; i_ic++) {
                vpxord(get_ker_zmm(i_ic), get_ker_zmm(i_ic), get_ker_zmm(i_ic));
            }
            for (int i_ow = 0; i_ow < ur_ow; i_ow++) {
                for (int i_ic = 0; i_ic < ur_ic; i_ic++) {
                    vfmadd231ps(get_ker_zmm(i_ic),
                            get_src_zmm(i_ow + i_kw, i_ic), get_ddst_zmm(i_ow));
                }
            }
            for (int i_ic = 0; i_ic < ur_ic; i_ic++) {
                int ker_offset = dwei_offset(i_kw, i_ic);
                vaddps(get_ker_zmm(i_ic), zword[reg_dwei + ker_offset]);
                vmovups(zword[reg_dwei + ker_offset], get_ker_zmm(i_ic));
            }
        }

        L(kw_loop_end);
    };

    auto kw_loop = [=](int ur_ow, int ur_ic, int is_iw_edge) {
        Label kwb_loop_begin, kwb_loop_end;
        int kw_tail = jcp.kw % kw_unroll;
        int kw_iter = jcp.kw / kw_unroll;

        if (kw_iter > 0) {
            if (kw_iter > 1) {
                mov(aux_reg_kwb, jcp.kw - kw_tail);
                L(kwb_loop_begin);
            }
            compute_kernel(ur_ow, ur_ic, kw_unroll, is_iw_edge);

            if (kw_iter > 1 || kw_tail) {
                add(reg_iw_base, (jcp.dilate_w + 1) * kw_unroll);
                add(reg_src, src_offset(0, (jcp.dilate_w + 1) * kw_unroll));
                add(reg_dwei, dwei_offset(kw_unroll, 0));
            }

            if (kw_iter > 1) {
                sub(aux_reg_kwb, kw_unroll);
                jg(kwb_loop_begin, T_NEAR);
            }
        }

        if (kw_tail) compute_kernel(ur_ow, ur_ic, kw_tail, is_iw_edge);

        L(kwb_loop_end);
    };

    auto ic_loop = [=](int ur_ow, int is_iw_edge) {
        Label icb_loop_begin, icb_loop_end;
        int ic_tail = jcp.ic % ic_unroll;
        int ic_iter = jcp.ic / ic_unroll;

        if (ic_iter > 0) {
            if (ic_iter > 1 || ic_tail) {
                mov(aux_reg_icb, jcp.ic - ic_tail);
                L(icb_loop_begin);
                // Saving onto the stack here appears to significantly slow down
                // code execution. If this kernel runs out of registers, getting
                // rid of the *_save registers should be possible by using
                // subtracts to restore the value and maintain performance.
                mov(reg_src_save, reg_src);
                mov(reg_dwei_save, reg_dwei);
                mov(reg_iw_base_save, reg_iw_base);
            }

            kw_loop(ur_ow, ic_unroll, is_iw_edge);

            if (ic_iter > 1 || ic_tail) {
                mov(reg_iw_base, reg_iw_base_save);
                mov(reg_dwei, reg_dwei_save);
                mov(reg_src, reg_src_save);

                Label inter_block_increment, increment_finish;
                sub(aux_reg_icb, ic_unroll);
                if (jcp.ic > jcp.ic_block) {
                    const int log2_ic_block = 4;
                    lea(reg_tmp, ptr[aux_reg_icb - jcp.ic - ic_tail]);
                    test(reg_tmp, (1 << log2_ic_block) - 1);
                    jnz(inter_block_increment, T_NEAR);

                    add(reg_dwei,
                            dwei_offset(0, jcp.ic_block)
                                    - dwei_offset(0, jcp.ic_block - ic_unroll));
                    jmp(increment_finish);
                    L(inter_block_increment);
                }
                add(reg_dwei, dwei_offset(0, ic_unroll));
                L(increment_finish);

                add(reg_src, src_offset(ic_unroll, 0));
            }
            if (ic_iter > 1) {
                cmp(aux_reg_icb, 0);
                jg(icb_loop_begin, T_NEAR);
            }
        }

        if (ic_tail) kw_loop(ur_ow, ic_tail, is_iw_edge);

        L(icb_loop_end);
    };

    auto ic_loop_dispatch = [=](int ur_ow) {
        Label iw_edge_case, ic_end;

        const int iw_overflow_bound = jcp.iw - (ur_ow - 1) * jcp.stride_w
                - (jcp.kw - 1) * (jcp.dilate_w + 1);
        cmp(reg_iw_base, iw_overflow_bound);
        jge(iw_edge_case, T_NEAR);
        if (jcp.l_pad > 0) {
            cmp(reg_iw_base, 0);
            jl(iw_edge_case, T_NEAR);
        }

        ic_loop(ur_ow, false);
        jmp(ic_end, T_NEAR);

        L(iw_edge_case);
        ic_loop(ur_ow, true);

        L(ic_end);
    };

    Label ow_end, ow_tail;
    int ow_tail_size = jcp.ow % ow_unroll;
    cmp(reg_owb, jcp.ow - ow_tail_size);
    jge(ow_tail, T_NEAR);

    load_ddsts(ow_unroll);
    ic_loop_dispatch(ow_unroll);
    jmp(ow_end, T_NEAR);

    L(ow_tail);
    load_ddsts(ow_tail_size);
    ic_loop_dispatch(ow_tail_size);

    L(ow_end);

    postamble();
    ret();
}
#endif

void jit_aarch64_sve_512_conv_bwd_weights_kernel_f32::generate() {
    preamble();
    CGA64::ptrue( reg_p_all_ones.b );

    CGA64::ldr(reg_input, xa::ptr(param, GET_OFF(src)));
    CGA64::ldr(reg_output, xa::ptr(param, GET_OFF(dst)));
    CGA64::ldr(reg_kernel, xa::ptr(param, GET_OFF(filt)));

#if 0
//[info]v1.6追加コード。取り敢えずコメント化。
    const int oc_tail = jcp.oc_tail;
    if (oc_tail) {
        Label skip;
        Reg32 reg_tail_32 = reg_oc_tail.cvt32();
        if (jcp.nb_oc > 1) {
            kxnorw(k_oc_mask, k_oc_mask, k_oc_mask);
            mov(reg_oc_tail, ptr[param + GET_OFF(load_work)]);
            cmp(reg_oc_tail, 16);
            je(skip, T_NEAR);
        }
        mov(reg_tail_32, (1 << oc_tail) - 1);
        kmovw(k_oc_mask, reg_tail_32);
        L(skip);
    }
#endif

    compute_loop();

    postamble();
}

status_t jit_aarch64_sve_512_conv_bwd_weights_kernel_f32::init_conf(
        jit_conv_conf_t &jcp, const convolution_desc_t &cd,
        memory_desc_t &src_md, memory_desc_t &diff_weights_md,
        memory_desc_t &diff_bias_md, memory_desc_t &diff_dst_md, int nthreads) {
    if (!mayiuse(sve)) return status::unimplemented;

    const memory_desc_wrapper src_d(&src_md);
    const memory_desc_wrapper diff_weights_d(&diff_weights_md);
    const memory_desc_wrapper diff_bias_d(&diff_bias_md);
    const memory_desc_wrapper diff_dst_d(&diff_dst_md);

    const bool with_groups = diff_weights_d.ndims() == src_d.ndims() + 1;
    int ndims = src_d.ndims();

    jcp = zero<decltype(jcp)>();

    jcp.simd_w = cpu_isa_traits<sve>::vlen / typesize;
    jcp.nthr = jcp.aligned_threads = nthreads;
    jcp.ndims = ndims;
    jcp.prop_kind = cd.prop_kind;

    jcp.ngroups = with_groups ? diff_weights_d.dims()[0] : 1;
    jcp.mb = src_d.dims()[0];

    jcp.oc = diff_dst_d.dims()[1] / jcp.ngroups;
    jcp.oc_without_padding = jcp.oc;
    jcp.ic = src_d.dims()[1] / jcp.ngroups;
    jcp.ic_without_padding = jcp.ic;

    jcp.id = (ndims == 5) ? src_d.dims()[2] : 1;
    jcp.ih = (ndims == 3) ? 1 : src_d.dims()[ndims - 2];
    jcp.iw = src_d.dims()[ndims - 1];
    jcp.od = (ndims == 5) ? diff_dst_d.dims()[2] : 1;
    jcp.oh = (ndims == 3) ? 1 : diff_dst_d.dims()[ndims - 2];
    jcp.ow = diff_dst_d.dims()[ndims - 1];

    jcp.kd = (ndims == 5) ? diff_weights_d.dims()[with_groups + 2] : 1;
    jcp.kh = (ndims == 3) ? 1 : diff_weights_d.dims()[with_groups + ndims - 2];
    jcp.kw = diff_weights_d.dims()[with_groups + ndims - 1];

    jcp.f_pad = (ndims == 5) ? cd.padding[0][0] : 0;
    jcp.t_pad = (ndims == 3) ? 0 : cd.padding[0][ndims - 4];
    jcp.l_pad = cd.padding[0][ndims - 3];

    jcp.stride_d = (ndims == 5) ? cd.strides[0] : 1;
    jcp.stride_h = (ndims == 3) ? 1 : cd.strides[ndims - 4];
    jcp.stride_w = cd.strides[ndims - 3];

    jcp.dilate_d = (ndims == 5) ? cd.dilates[0] : 0;
    jcp.dilate_h = (ndims == 3) ? 0 : cd.dilates[ndims - 4];
    jcp.dilate_w = cd.dilates[ndims - 3];

    int ext_kw = calculate_extended_filter_size(jcp.kw, jcp.dilate_w);
    int ext_kh = calculate_extended_filter_size(jcp.kh, jcp.dilate_h);
    int ext_kd = calculate_extended_filter_size(jcp.kd, jcp.dilate_d);

    bool ok = true
            // general condition to simplify dilations
            && IMPLICATION(jcp.dilate_d != 0, jcp.stride_d == 1)
            && IMPLICATION(jcp.dilate_h != 0, jcp.stride_h == 1)
            // special condition to simplify dilations in compute_oh_loop_common
            && IMPLICATION(jcp.dilate_h != 0, ext_kh <= jcp.ih);
    if (!ok) return status::unimplemented;

    jcp.r_pad = nstl::max(0,
            calculate_end_padding(
                    jcp.l_pad, jcp.ow, jcp.iw, jcp.stride_w, ext_kw));
    jcp.b_pad = nstl::max(0,
            calculate_end_padding(
                    jcp.t_pad, jcp.oh, jcp.ih, jcp.stride_h, ext_kh));
    jcp.back_pad = nstl::max(0,
            calculate_end_padding(
                    jcp.f_pad, jcp.od, jcp.id, jcp.stride_d, ext_kd));

    /* XXX: currently, does not support dilation_d > 0 */
    if (ndims == 5)
        if (jcp.dilate_d > 0) return status::unimplemented;

    /* Set bounds for large filter 'kw > 14' support and optimized JIT
     * implementation for small output-width 'ow = 1' */
    const int min_filter_size = 14;
    const int max_filter_size = 20;
    const auto dat_tag_nxc = pick(ndims - 3, nwc, nhwc, ndhwc);
    const auto dat_tag_ncx = pick(ndims - 3, ncw, nchw, ncdhw);
    const auto dat_tag_nCx16c = pick(ndims - 3, nCw16c, nChw16c, nCdhw16c);
    auto curr_src_tag = src_d.matches_one_of_tag(
            dat_tag_nxc, dat_tag_nCx16c, dat_tag_ncx);
    auto curr_dst_tag
            = diff_dst_d.matches_one_of_tag(dat_tag_nxc, dat_tag_nCx16c);
    bool is_data_layout_nxc
            = utils::everyone_is(dat_tag_nxc, curr_src_tag, curr_dst_tag);
#if 0
//[info]修正方法不明のため、取り敢えずコメント化。
    if (mayiuse(avx512_mic) && is_data_layout_nxc) return status::unimplemented;
#endif

    /* Optimization: when `output-width == 1' deploy a special case of the
     * JIT-Kernel by unrolling with regards to height instead of width for
     * the source and filter tensors. The JIT-Kernel also transposes the
     * strides for the input and filter memory access. */
#if 0
    jcp.is_hw_transp = !is_data_layout_nxc && ndims == 4 && !mayiuse(avx512_mic)
            && jcp.kw >= min_filter_size && jcp.kw < max_filter_size
            && jcp.ow == 1 && jcp.kw == jcp.iw
            && everyone_is(1, jcp.stride_w, jcp.stride_h)
            && everyone_is(0, jcp.dilate_h, jcp.dilate_w)
            && everyone_is(0, jcp.l_pad, jcp.t_pad, jcp.r_pad, jcp.b_pad);
#else
    jcp.is_hw_transp = !is_data_layout_nxc && ndims == 4
            && jcp.kw >= min_filter_size && jcp.kw < max_filter_size
            && jcp.ow == 1 && jcp.kw == jcp.iw
            && everyone_is(1, jcp.stride_w, jcp.stride_h)
            && everyone_is(0, jcp.dilate_h, jcp.dilate_w)
            && everyone_is(0, jcp.l_pad, jcp.t_pad, jcp.r_pad, jcp.b_pad);
#endif

    if (jcp.is_hw_transp) {
        jcp.tr_kw = jcp.kh;
        jcp.tr_kh = jcp.kw;
        jcp.tr_iw = jcp.ih;
        jcp.tr_ih = jcp.iw;
    }

    jcp.ihp = jcp.ih + jcp.t_pad + jcp.b_pad;
    jcp.iwp = jcp.iw + jcp.l_pad + jcp.r_pad;
    jcp.ohp = jcp.oh;
    jcp.owp = jcp.ow;
    jcp.aligned_threads = 0;

    /* check for the 1st convolution */
    jcp.is_1stconv = is_1stconv(jcp);

    jcp.oc_block = jcp.simd_w;

    bool ok_to_pad_channels = true && !is_data_layout_nxc && jcp.ngroups == 1
            && src_d.data_type() == data_type::f32;

    if (ok_to_pad_channels) jcp.oc = rnd_up(jcp.oc, jcp.simd_w);

    if (!IMPLICATION(!is_data_layout_nxc, jcp.oc % jcp.oc_block == 0))
        return status::unimplemented;
    jcp.ic_tail = is_data_layout_nxc ? jcp.ic % jcp.simd_w : 0;
    jcp.oc_tail = is_data_layout_nxc ? jcp.oc % jcp.simd_w : 0;

    auto dst_tag = is_data_layout_nxc ? dat_tag_nxc : dat_tag_nCx16c;
    auto wei_tag = with_groups
            ? pick(ndims - 3, gOIw16i16o, gOIhw16i16o, gOIdhw16i16o)
            : pick(ndims - 3, OIw16i16o, OIhw16i16o, OIdhw16i16o);

    if (diff_dst_md.format_kind == format_kind::any) {
        CHECK(memory_desc_init_by_tag(diff_dst_md, dst_tag));
    } else if (curr_dst_tag != dst_tag)
        return status::unimplemented;
    jcp.dst_tag = dst_tag;

    /* conditions on bias memory */
    jcp.with_bias = cd.diff_bias_desc.format_kind != format_kind::undef;
    if (jcp.with_bias) {
        if (diff_bias_d.format_kind() == format_kind::any)
            CHECK(memory_desc_init_by_tag(diff_bias_md, x));
    }

    jcp.nb_oc = div_up(jcp.oc, jcp.oc_block);

    /* kernel applicability check wrt boundaries
     * the conditions are quite general across the kernels we have,
     * but ideally the check should belong to a specific kernel... */
    const int max_pad_h = ext_kh / 2;
    const bool boundaries_ok = true && jcp.l_pad < ext_kw && jcp.r_pad < ext_kw
            && jcp.t_pad <= max_pad_h && jcp.b_pad <= max_pad_h
            && jcp.f_pad < ext_kd && jcp.back_pad < ext_kd
            && IMPLICATION(jcp.f_pad > 0, jcp.kd < jcp.id + jcp.f_pad)
            && jcp.l_pad <= max_ur_w && jcp.r_pad <= max_ur_w;
    if (!boundaries_ok) return status::unimplemented;

    /* yet another common check */
    if (!jcp.is_hw_transp && jcp.kw > 14) return status::unimplemented;

    /* setting register strategy */
    const int unroll_dim = jcp.is_hw_transp ? jcp.oh : jcp.ow;
    for (int ur_w = nstl::min(max_ur_w, unroll_dim); ur_w > 0; --ur_w) {
        if (unroll_dim % ur_w == 0) {
            jcp.ur_w = ur_w;
            break;
        }
    }

    if (jcp.is_1stconv) {
        auto src_tag = is_data_layout_nxc ? dat_tag_nxc : dat_tag_ncx;
        if (src_d.format_kind() == format_kind::any) {
            CHECK(memory_desc_init_by_tag(src_md, src_tag));
        } else {
            // if `ic == 1`, then `nxc` and `ncx` are effectively equivalent
            if (jcp.ic == 1 && one_of(curr_src_tag, dat_tag_nxc, dat_tag_ncx))
                src_tag = curr_src_tag;
            if (curr_src_tag != src_tag) return status::unimplemented;
        }
        jcp.src_tag = src_tag;

        const bool src_ok = true
                && utils::everyone_is(data_type::f32, src_d.data_type(),
                        diff_weights_d.data_type(), diff_dst_d.data_type())
                && IMPLICATION(!is_data_layout_nxc,
                        (one_of(jcp.ic, 1, 2, 3) && jcp.ngroups == 1));
        if (!src_ok) return status::unimplemented;

        const int tr_ld = rnd_up(
                div_up(jcp.iw + jcp.l_pad + jcp.r_pad, jcp.stride_w), 16);
        const int kh_step = nstl::max((28 - jcp.with_bias) / jcp.kw, 1);
        const int kh_step_rem = jcp.kh % kh_step;

        const auto wei_4fma_tag = with_groups
                ? pick(ndims - 3, gOiw16o, gOihw16o, gOidhw16o)
                : pick(ndims - 3, Oiw16o, Oihw16o, Oidhw16o);

        auto current_wei_tag = format_tag::undef;
        if (diff_weights_d.format_kind() != format_kind::any)
            current_wei_tag = diff_weights_d.matches_one_of_tag(wei_4fma_tag);

#if 0
//[info]取り敢えずコメント化。
        const bool use_4fma = true && !is_data_layout_nxc && one_of(ndims, 3, 4)
                && mayiuse(avx512_mic_4ops) && dnnl_thr_syncable()
                && everyone_is(0, jcp.dilate_d, jcp.dilate_h, jcp.dilate_w)
                && everyone_is(0, jcp.l_pad, jcp.r_pad, jcp.t_pad, jcp.b_pad)
                && jcp.kw <= 28 - jcp.with_bias && jcp.stride_w == 4
                && tr_ld / jcp.simd_w <= 4 /* [bwd_w:tr_src:r1] */
                && IMPLICATION(
                        jcp.with_bias, kh_step_rem == 1) /* [bwd_w:b:r1] */
                && IMPLICATION(diff_weights_d.format_kind() != format_kind::any,
                        current_wei_tag == wei_4fma_tag);
#endif

#if 0
        if (use_4fma) {
            jcp.ver = ver_4fma;
            jcp.kh_step = kh_step;
            jcp.tr_ld = tr_ld;
            jcp.ic_block = 1;
            if (diff_weights_d.format_kind() == format_kind::any)
                CHECK(memory_desc_init_by_tag(diff_weights_md, wei_4fma_tag));
            jcp.wei_tag = wei_4fma_tag;
        } else {
            jcp.ver = ver_fma;
            jcp.ic_block = jcp.ic;

            wei_tag = with_groups
                    ? pick(ndims - 3, gOwi16o, gOhwi16o, gOdhwi16o)
                    : pick(ndims - 3, Owi16o, Ohwi16o, Odhwi16o);

            if (init_tag(jcp.wei_tag, diff_weights_md, diff_weights_d, wei_tag)
                    != status::success)
                return status::unimplemented;
        }
#else
            jcp.ver = ver_fma;
            jcp.ic_block = jcp.ic;

            wei_tag = with_groups
                    ? pick(ndims - 3, gOwi16o, gOhwi16o, gOdhwi16o)
                    : pick(ndims - 3, Owi16o, Ohwi16o, Odhwi16o);

            if (init_tag(jcp.wei_tag, diff_weights_md, diff_weights_d, wei_tag)
                    != status::success)
                return status::unimplemented;
#endif

        jcp.nb_ic = div_up(jcp.ic, jcp.ic_block);
    } else {
        auto src_tag = is_data_layout_nxc ? dat_tag_nxc : dat_tag_nCx16c;
        if (src_md.format_kind == format_kind::any) {
            CHECK(memory_desc_init_by_tag(src_md, src_tag));
        } else if (curr_src_tag != src_tag)
            return status::unimplemented;
        jcp.src_tag = src_tag;

        if (init_tag(jcp.wei_tag, diff_weights_md, diff_weights_d, wei_tag)
                != status::success)
            return status::unimplemented;

        jcp.ic_block = jcp.simd_w;
        if (ok_to_pad_channels) jcp.ic = rnd_up(jcp.ic, jcp.ic_block);
        jcp.nb_ic = div_up(jcp.ic, jcp.ic_block);
#if 0
        if ((mayiuse(avx512_mic) || mayiuse(avx512_core))
                && utils::everyone_is(data_type::f32, src_d.data_type(),
                        diff_weights_d.data_type(), diff_dst_d.data_type())) {
            jcp.ver = ver_fma;
            if (one_of(ndims, 3, 4) && mayiuse(avx512_mic_4ops)
                    && !is_data_layout_nxc && jcp.stride_w == 1
                    && everyone_is(0, jcp.dilate_d, jcp.dilate_h, jcp.dilate_w)
                    && dnnl_thr_syncable()) {
                jcp.ver = ver_4fma;
            }
        } else {
            return status::unimplemented;
        }
#else
        if (mayiuse(sve)
                && utils::everyone_is(data_type::f32, src_d.data_type(),
                        diff_weights_d.data_type(), diff_dst_d.data_type())) {
            jcp.ver = ver_fma;
        } else {
            return status::unimplemented;
        }
#endif

#if 0
        if (jcp.ver == ver_4fma) {
            jcp.ur_w = jcp.ow;
            // XXX, BUGBUGBUG, but not a FIXME: this assumes that it's OK to
            // cross the right boundary. The only requirement is not to have
            // NaNs there because another multiplicand is always guaranteed to
            // be zero. This also may require the top-level driver to allocate
            // four extra guarding elements at the very end of the buffer.
            // I'm not proud of this hack, but it improves performance by
            // about 5-10% depending on the dimensions (Roma)

            const int tr_round = 4;

            jcp.tr_iw = rnd_up(jcp.iw + jcp.kw - 1, tr_round);
            jcp.tr_src_num_guard_elems = tr_round; // upper bound
        }
#endif
    }

#if 0
    if (utils::one_of(jcp.ver, ver_4fma, ver_fma)) {
        jcp.typesize_in = typesize;
        jcp.typesize_out = typesize;
    } else
        return status::unimplemented;
#else
    if (jcp.ver == ver_fma) {
        jcp.typesize_in = typesize;
        jcp.typesize_out = typesize;
    } else
        return status::unimplemented;
#endif

    bool use_nxc_harness = false;
    if (is_data_layout_nxc && jcp.ver == ver_fma) {
        dim_t kernel_size
                = jcp.ic * jcp.oc * jcp.kd * jcp.kh * jcp.kw * jcp.typesize_out;
        dim_t src_size
                = jcp.mb * jcp.ic * jcp.id * jcp.ih * jcp.iw * jcp.typesize_in;
        dim_t diff_dst_size
                = jcp.mb * jcp.oc * jcp.id * jcp.ih * jcp.iw * jcp.typesize_in;
        dim_t data_size = src_size + diff_dst_size;

        // The advantage of the nxc kernel is cache traversal, this comes at a
        // cost of extra work updating the weights buffers more often. As such,
        // if everything fits in cache, this kernel is at a disadvantage to the
        // inner loop over ow. More optimizing/balancing is required to
        // determine when this is needed for multidimensional kernels because
        // the data reuses within the kernel height/depth dimension make the
        // computation more computationally bound and cache traversal advantage
        // less important. Due to the current blocked weights format, the
        // weights and the data buffers cannot both be traversed optimally, so
        // for performance, the weights must fit in cache.
        use_nxc_harness
                = (data_size / nthreads + kernel_size > L2_cache_size / 3)
                && (jcp.oc % jcp.simd_w == 0) && (jcp.ic % jcp.simd_w == 0)
                && jcp.kw > 1 && ndims == 3
                && (kernel_size < L2_cache_size / 2);
    }

    jcp.harness = use_nxc_harness
            ? harness_nxc
            : ndims == 5 ? harness_3d_reduction : harness_mb_reduction;
    if (jcp.dilate_h == 0 && jcp.ndims == 4 && jcp.oh > min_oh_reduce
            && jcp.ver == ver_fma && !jcp.is_hw_transp && !is_data_layout_nxc)
        jcp.harness = harness_2d_reduction; // 2d harness with oh reduction
    bool args_ok = true
            && IMPLICATION(!is_data_layout_nxc,
                    jcp.ic % jcp.ic_block == 0 && jcp.oc % jcp.oc_block == 0)
            && jcp.ic <= src_d.padded_dims()[1]
            && jcp.oc <= diff_dst_d.padded_dims()[1]
            && jcp.ic <= diff_weights_d.padded_dims()[with_groups + 1]
            && jcp.oc <= diff_weights_d.padded_dims()[with_groups + 0];
    if (!args_ok) return status::unimplemented;

    int nthr, nthr_mb, nthr_g, nthr_oc_b, nthr_ic_b;
    if (jcp.harness == harness_nxc) {
        // The harness_nxc is quite different from the other kernels. The
        // init_conf function should probably be refactored so that it calls
        // functions along the line of tune_nxc, tun_4fma, tune_fma which
        // independently tune the kernels for each implementation with tuning
        // common to multiple implementations performed by helper functions.
        // This will help maintainability and help prevent the different
        // implementations from stepping on each other.
        int zmm_regs = 32;

        // Block by ic and kw in the compute kernel to decrease loads from the
        // src buffer
        jcp.ur_ic = 2 - jcp.ic % 2;
        jcp.ur_kw = 1;
        if (jcp.stride_w == jcp.dilate_w + 1) {
            jcp.ur_kw = jcp.kw;
            if (jcp.kw > 7) {
                // Blocking by kw is more effective than by ic in the compute
                // kernel since neighbor kw operations share src data
                jcp.ur_ic = 1;
                if (jcp.kw > zmm_regs / (jcp.ur_ic + 1))
                    jcp.ur_kw = jcp.kw % (zmm_regs / (jcp.ur_ic + 1));
            }
        }

        // Unroll by ow to decrease updates to diff_weights. In practice, this
        // should be approximately 1/4 - 1/2 of the zmm registers
        jcp.ur_ow = nstl::min(
                (zmm_regs - jcp.ur_kw * jcp.ur_ic) / (jcp.ur_ic + 1), jcp.ow);

        int work_amount_base = jcp.mb * jcp.od * jcp.oh;
        int ow_iter = div_up(jcp.ow, jcp.ur_ow);
        int nthr_ow = nstl::min(
                jcp.nthr / math::gcd(work_amount_base, jcp.nthr), ow_iter);
        int ow_block = div_up(ow_iter, nthr_ow) * jcp.ur_ow;

        jcp.ow_block = ow_block;
        jcp.nb_ow = div_up(jcp.ow, jcp.ow_block);

        // Choose a simple parallelization method. A more advance may need made
        // later
        int work_amount = jcp.mb * jcp.od * jcp.oh * jcp.nb_ow;
        nthr_mb = nstl::min(jcp.nthr, work_amount);
        nthr_g = 1;
        nthr_oc_b = 1;
        nthr_ic_b = 1;
        nthr = nthr_mb * nthr_g * nthr_oc_b * nthr_ic_b;
    } else { // balancing
        balance(jcp, nthr, nthr_mb, nthr_g, nthr_oc_b, nthr_ic_b, jcp.nthr);
    }

    jcp.nthr = nthr;
    jcp.nthr_mb = nthr_mb;
    jcp.nthr_g = nthr_g;
    jcp.nthr_oc_b = nthr_oc_b;
    jcp.nthr_ic_b = nthr_ic_b;

    jcp.kernel_kind = embd_bcast;
    if (is_data_layout_nxc && jcp.stride_w == 1 && jcp.dilate_w == 0
            && !jcp.is_1stconv) {
        jcp.kernel_kind = expl_bcast;
    }

    jcp.nb_ic_blocking_max = 1;
    if (is_data_layout_nxc && (jcp.ow > max_ur_w || jcp.ndims == 5)) {
        assert(!jcp.is_hw_transp);
        jcp.nb_ic_blocking_max = nstl::min(8, div_up(jcp.nb_ic, jcp.nthr_ic_b));
    }

    return status::success;
}

void jit_aarch64_sve_512_conv_bwd_weights_kernel_f32::init_scratchpad(
        memory_tracking::registrar_t &scratchpad, const jit_conv_conf_t &jcp) {
#if 0
    if (jcp.ver == ver_4fma) {
        if (jcp.is_1stconv) {
            const size_t tr_src_size = jcp.nthr / jcp.nthr_oc_b * jcp.ih
                    * jcp.stride_w * jcp.tr_ld;
            scratchpad.book(key_conv_tr_src, tr_src_size, jcp.typesize_in);
        } else {
            // XXX: See the comment about tr_iw and guarding elements in
            // jit_avx512_common_conv_bwd_weights_kernel_f32::init_conf()
            const size_t max_nthr = jcp.nthr_mb * jcp.ngroups * jcp.nb_ic;
            const size_t min_tr_src_size_per_thr
                    = jcp.ih * jcp.ic_block * jcp.tr_iw;
            const size_t tr_src_size = max_nthr * min_tr_src_size_per_thr
                    + jcp.tr_src_num_guard_elems;
            scratchpad.book(key_conv_tr_src, tr_src_size, jcp.typesize_in);
        }

        /* prepare synchronization contexts */
        if (jcp.nthr_oc_b > 1) {
            const int tr_src_bctx_size = jcp.nthr / jcp.nthr_oc_b;
            scratchpad.book<simple_barrier::ctx_t>(
                    key_conv_tr_src_bctx, tr_src_bctx_size);
        }
    }
#endif

    if (jcp.nthr_mb > 1) {
        const int wei_size = jcp.ngroups * rnd_up(jcp.oc, jcp.oc_block)
                * rnd_up(jcp.ic, jcp.ic_block) * jcp.kh * jcp.kw * jcp.kd;
        const int bia_size = jcp.ngroups * rnd_up(jcp.oc, jcp.oc_block);
        const size_t wei_bia_reduction_size = wei_size + bia_size;

        scratchpad.book(key_conv_wei_bia_reduction,
                wei_bia_reduction_size * (jcp.nthr_mb - 1), jcp.typesize_out);
        scratchpad.book<simple_barrier::ctx_t>(
                key_conv_wei_bia_reduction_bctx, 1);
    }

    if (jcp.with_bias && jcp.oc_without_padding % jcp.oc_block != 0) {
        const size_t nelems_padded_bias
                = jcp.ngroups * utils::rnd_up(jcp.oc, jcp.oc_block);
        scratchpad.book(
                key_conv_padded_bias, nelems_padded_bias, jcp.typesize_out);
    }
}

void jit_aarch64_sve_512_conv_bwd_weights_kernel_f32::balance(
        const jit_conv_conf_t &j, int &nthr_, int &nthr_mb_, int &nthr_g_,
        int &nthr_oc_b_, int &nthr_ic_b_, int nthreads) {
    nthr_ = nthr_mb_ = nthr_g_ = nthr_oc_b_ = nthr_ic_b_ = 1;

    if (nthreads < j.ngroups) {
        /* simplification... fortunately it doesn't hurt much */
        nthr_ = nthr_g_ = nthreads;
        return;
    }

#if 0
    if (!dnnl_thr_syncable() && j.ver == ver_4fma) {
        // should not happen -- the driver is not ready
        // for TBB-like non-synchronous threading yet
        return;
    }
#endif

#if 0
    if (j.ver == ver_4fma && j.is_1stconv) {
        nthr_g_ = 1;
        nthr_oc_b_ = 1;
        nthr_ic_b_ = nstl::min(j.nb_ic, nthreads);
        nthr_mb_ = nstl::min(nthreads / nthr_ic_b_, j.mb);
        nthr_ = nthr_mb_ * nthr_oc_b_ * nthr_ic_b_ * nthr_g_;
        return;
    }
#endif

    nthr_g_ = j.ngroups;
    const int nthr = nthreads / nthr_g_;

    const int ih = j.is_hw_transp ? j.tr_ih : j.ih;
    const int oh = j.is_hw_transp ? j.ow : j.oh;

    int ih_reduce = j.harness == harness_2d_reduction ? ih : 1;
    int oh_reduce = j.harness == harness_2d_reduction ? oh : 1;
    int ih_no_reduce = j.harness == harness_2d_reduction ? 1 : ih;
    int oh_no_reduce = j.harness == harness_2d_reduction ? 1 : oh;
    int nthr_oh_reduce = nstl::max(1, oh_reduce / min_oh_reduce);

    auto calc_mem_cost = [=](int nthr_mb, int nthr_oc_b, int nthr_ic_b) {
        /* calculate per thread memory cost (read/write). high level optimizer
         * tries to minimize memory consumption. few notes:
         *  (n1) unclear why, but that essentially helps first convolution...
         *  (n2) assuming the reduction over minibatch is always there:
         *    - instead of 8 it should be 5 here (write ~= 2 read):
         *      kernel: temporal workspace 1 write
         *      reduction: 1 read from workspace and 1 write to the diff_wei
         *    - but experiments showed 8 works better than 5 or 6... */
#if 0
        const dim_t src_coef = j.ver == ver_4fma ? 4 : 1;
#else
        const dim_t src_coef = 1;
#endif
        const dim_t dst_coef = 1;
        const dim_t wei_coef = 8;
        const dim_t iw = j.is_hw_transp ? j.tr_iw : j.iw;
        const dim_t ow = j.is_hw_transp ? j.oh : j.ow;

        return 0
                + src_coef * div_up(j.mb * ih_reduce, nthr_mb)
                * div_up(j.ngroups, nthr_g_) * div_up(j.nb_ic, nthr_ic_b)
                * j.ic_block * ih_no_reduce * iw * j.id / j.stride_d
                / j.stride_h / j.stride_w /* (n1) */
                + dst_coef * div_up(j.mb * oh_reduce, nthr_mb)
                * div_up(j.ngroups, nthr_g_) * div_up(j.nb_oc, nthr_oc_b)
                * j.oc_block * oh_no_reduce * ow * j.od
                + wei_coef /* (n2) */
                * div_up(j.ngroups, nthr_g_) * div_up(j.nb_oc, nthr_oc_b)
                * div_up(j.nb_ic, nthr_ic_b) * j.kh * j.kw * j.kd * j.ic_block
                * j.oc_block;
    };

    dim_t best_mem_cost = calc_mem_cost(nthr_mb_, nthr_oc_b_, nthr_ic_b_);

    /* step 1: find the best thread distribution with lowest memory cost */
    const int nthr_mb_max = nstl::min(nthr, j.mb * j.od * nthr_oh_reduce);
    for (int nthr_mb = 1; nthr_mb <= nthr_mb_max; ++nthr_mb) {
        const int nthr_par = nthr / nthr_mb;
        const int nthr_oc_b_max = nstl::min(nthr_par, j.nb_oc);
        for (int nthr_oc_b = 1; nthr_oc_b <= nthr_oc_b_max; ++nthr_oc_b) {
            int nthr_ic_b = nstl::min(nthr_par / nthr_oc_b, j.nb_ic);

            dim_t mem_cost = calc_mem_cost(nthr_mb, nthr_oc_b, nthr_ic_b);
            if (mem_cost <= best_mem_cost) {
                best_mem_cost = mem_cost;
                nthr_mb_ = nthr_mb;
                nthr_oc_b_ = nthr_oc_b;
                nthr_ic_b_ = nthr_ic_b;
            }
        }
    }

#if 0
    if (!mayiuse(avx512_mic)) {
        auto calc_comp_cost = [=](int nthr_mb, int nthr_oc_b, int nthr_ic_b) {
            return (dim_t)div_up(j.mb * oh_reduce, nthr_mb)
                    * div_up(j.ngroups, nthr_g_) * div_up(j.nb_oc, nthr_oc_b)
                    * div_up(j.nb_ic, nthr_ic_b);
        };

        /* step 2: search for a thread distribution with lower compute cost.
         * the constrains:
         *  - memory cost cannot exceed 110% of the best found in the step 1
         *  - unless compute cost is 133% lower than the current best case
         * note: both constants were found empirically */
        dim_t best_comp_cost = calc_comp_cost(nthr_mb_, nthr_oc_b_, nthr_ic_b_);
        for (int nthr_mb = 1; nthr_mb <= nthr_mb_max; ++nthr_mb) {
            const int nthr_par = nthr / nthr_mb;
            const int nthr_oc_b_max = nstl::min(nthr_par, j.nb_oc);
            for (int nthr_oc_b = 1; nthr_oc_b <= nthr_oc_b_max; ++nthr_oc_b) {
                int nthr_ic_b = nstl::min(nthr_par / nthr_oc_b, j.nb_ic);
                dim_t mem_cost = calc_mem_cost(nthr_mb, nthr_oc_b, nthr_ic_b);
                dim_t comp_cost = calc_comp_cost(nthr_mb, nthr_oc_b, nthr_ic_b);

                const bool opt1 = comp_cost <= best_comp_cost
                        && IMPLICATION(!j.is_hw_transp,
                                mem_cost < 1.1 * best_mem_cost);
                const bool opt2 = 4 * comp_cost <= 3 * best_comp_cost;

                if (opt1 || opt2) {
                    best_comp_cost = comp_cost;
                    nthr_mb_ = nthr_mb;
                    nthr_oc_b_ = nthr_oc_b;
                    nthr_ic_b_ = nthr_ic_b;
                }
            }
        }
    }
#endif

    if (nthr_mb_ > nthreads / 2 && nthr_mb_ < nthreads)
        nthr_mb_ = nstl::min(j.mb * j.od * nthr_oh_reduce, nthreads);
    nthr_ = nthr_mb_ * nthr_g_ * nthr_oc_b_ * nthr_ic_b_;

    assert(nthr_ <= nthreads);
}

template struct _jit_aarch64_sve_512_conv_fwd_kernel<Zmm>;
template struct _jit_aarch64_sve_512_conv_fwd_kernel<Ymm>;
template struct _jit_aarch64_sve_512_conv_fwd_kernel<Xmm>;
template struct _jit_aarch64_sve_512_conv_bwd_data_kernel_f32<Zmm>;
template struct _jit_aarch64_sve_512_conv_bwd_data_kernel_f32<Ymm>;
template struct _jit_aarch64_sve_512_conv_bwd_data_kernel_f32<Xmm>;

} // namespace aarch64
} // namespace cpu
} // namespace impl
} // namespace dnnl
