;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;  Copyright(c) 2011-2016 Intel Corporation All rights reserved.
;
;  Redistribution and use in source and binary forms, with or without
;  modification, are permitted provided that the following conditions
;  are met:
;    * Redistributions of source code must retain the above copyright
;      notice, this list of conditions and the following disclaimer.
;    * Redistributions in binary form must reproduce the above copyright
;      notice, this list of conditions and the following disclaimer in
;      the documentation and/or other materials provided with the
;      distribution.
;    * Neither the name of Intel Corporation nor the names of its
;      contributors may be used to endorse or promote products derived
;      from this software without specific prior written permission.
;
;  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
;  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
;  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
;  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
;  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
;  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
;  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
;  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
;  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
;  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
;  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

default rel
[bits 64]

%include "reg_sizes.asm"

extern isal_deflate_body_base
extern isal_deflate_body_01
extern isal_deflate_body_02
extern isal_deflate_body_04
extern isal_deflate_finish_base
extern isal_deflate_finish_01

extern isal_deflate_icf_body_hash_hist_base
extern isal_deflate_icf_body_hash_hist_01
extern isal_deflate_icf_body_hash_hist_02
extern isal_deflate_icf_body_hash_hist_04
extern isal_deflate_icf_finish_hash_hist_base
extern isal_deflate_icf_finish_hash_hist_01

extern isal_deflate_icf_finish_hash_map_base

extern isal_update_histogram_base
extern isal_update_histogram_01
extern isal_update_histogram_04

extern gen_icf_map_h1_base
extern gen_icf_map_lh1_04

extern encode_deflate_icf_base
extern encode_deflate_icf_04

extern set_long_icf_fg_base
extern set_long_icf_fg_04

extern encode_deflate_icf_06
extern encode_deflate_icf_10
extern set_long_icf_fg_06
extern gen_icf_map_lh1_06
extern parse_icf_map_g_base
extern parse_icf_map_g_06

extern adler32_base
extern adler32_avx2_4
extern adler32_avx512
extern adler32_sse

extern isal_deflate_hash_base
extern isal_deflate_hash_crc_01

extern isal_deflate_hash_mad_base

extern icf_body_hash1_fillgreedy_lazy
extern icf_body_lazyhash1_fillgreedy_greedy

section .text

%include "multibinary.asm"

;;;;;
; mbin_dispatch_init7_vbmi parameters
; 1-> function name
; 2-> base function
; 3-> SSE4_2 or 00/01 optimized function
; 4-> AVX/02 opt func
; 5-> AVX2/04 opt func
; 6-> AVX512/06 opt func
; 7-> AVX512 Update/10 opt func that also uses AVX512 VBMI
;
; Like mbin_dispatch_init6, with the 10 opt func when the processor has the
; AVX512 Update features and VBMI, which they do not include
;;;;;
%macro mbin_dispatch_init7_vbmi 7
	section .text
	%1_dispatch_init:
		push	rsi
		push	rax
		push	rbx
		push	rcx
		push	rdx
		push	rdi
		lea	rsi, [%2 WRT_OPT] ; Default - use base function

		mov	eax, 1
		cpuid
		mov	ebx, ecx ; save cpuid1.ecx
		test	ecx, FLAG_CPUID1_ECX_SSE4_2
		je	_%1_init_done	  ; Use base function if no SSE4_2
		lea	rsi, [%3 WRT_OPT] ; SSE possible so use 00/01 opt

		;; Test for XMM_YMM support/AVX
		test	ecx, FLAG_CPUID1_ECX_OSXSAVE
		je	_%1_init_done
		xor	ecx, ecx
		xgetbv	; xcr -> edx:eax
		mov	edi, eax	  ; save xgetvb.eax

		and	eax, FLAG_XGETBV_EAX_XMM_YMM
		cmp	eax, FLAG_XGETBV_EAX_XMM_YMM
		jne	_%1_init_done
		test	ebx, FLAG_CPUID1_ECX_AVX
		je	_%1_init_done
		lea	rsi, [%4 WRT_OPT] ; AVX/02 opt

		;; Test for AVX2
		xor	ecx, ecx
		mov	eax, 7
		cpuid
		test	ebx, FLAG_CPUID7_EBX_AVX2
		je	_%1_init_done		; No AVX2 possible
		lea	rsi, [%5 WRT_OPT] 	; AVX2/04 opt func

		;; Test for AVX512
		and	edi, FLAG_XGETBV_EAX_ZMM_OPM
		cmp	edi, FLAG_XGETBV_EAX_ZMM_OPM
		jne	_%1_init_done	  ; No AVX512 possible
		and	ebx, FLAGS_CPUID7_EBX_AVX512_G1
		cmp	ebx, FLAGS_CPUID7_EBX_AVX512_G1
		jne	_%1_init_done
		lea	rsi, [%6 WRT_OPT] ; AVX512/06 opt

		;; Test for AVX512 Update and VBMI
		and	ecx, FLAGS_CPUID7_ECX_AVX512_G2 | FLAG_CPUID7_ECX_AVX512VBMI
		cmp	ecx, FLAGS_CPUID7_ECX_AVX512_G2 | FLAG_CPUID7_ECX_AVX512VBMI
		lea	rbx, [%7 WRT_OPT] ; AVX512 VBMI/10 opt
		cmove	rsi, rbx

	_%1_init_done:
		pop	rdi
		pop	rdx
		pop	rcx
		pop	rbx
		pop	rax
		mov	[%1_dispatched], rsi
		pop	rsi
		ret
%endmacro

mbin_interface		isal_deflate_body
mbin_dispatch_init5	isal_deflate_body, isal_deflate_body_base, isal_deflate_body_01, isal_deflate_body_02, isal_deflate_body_04
mbin_interface		isal_deflate_finish
mbin_dispatch_init5	isal_deflate_finish, isal_deflate_finish_base, isal_deflate_finish_01, isal_deflate_finish_01, isal_deflate_finish_01

mbin_interface		isal_deflate_icf_body_lvl1
mbin_dispatch_init5	isal_deflate_icf_body_lvl1, isal_deflate_icf_body_hash_hist_base, isal_deflate_icf_body_hash_hist_01, isal_deflate_icf_body_hash_hist_02, isal_deflate_icf_body_hash_hist_04

mbin_interface		isal_deflate_icf_body_lvl2
mbin_dispatch_init5	isal_deflate_icf_body_lvl2, isal_deflate_icf_body_hash_hist_base, isal_deflate_icf_body_hash_hist_01, isal_deflate_icf_body_hash_hist_02, isal_deflate_icf_body_hash_hist_04

mbin_interface		isal_deflate_icf_body_lvl3
mbin_dispatch_init5	isal_deflate_icf_body_lvl3, icf_body_hash1_fillgreedy_lazy, icf_body_hash1_fillgreedy_lazy, icf_body_hash1_fillgreedy_lazy, icf_body_lazyhash1_fillgreedy_greedy

mbin_interface		isal_deflate_icf_finish_lvl1
mbin_dispatch_init5	isal_deflate_icf_finish_lvl1, isal_deflate_icf_finish_hash_hist_base, isal_deflate_icf_finish_hash_hist_01, isal_deflate_icf_finish_hash_hist_01, isal_deflate_icf_finish_hash_hist_01

mbin_interface		isal_deflate_icf_finish_lvl2
mbin_dispatch_init5	isal_deflate_icf_finish_lvl2, isal_deflate_icf_finish_hash_hist_base, isal_deflate_icf_finish_hash_hist_01, isal_deflate_icf_finish_hash_hist_01, isal_deflate_icf_finish_hash_hist_01

mbin_interface		isal_deflate_icf_finish_lvl3
mbin_dispatch_init5	isal_deflate_icf_finish_lvl3, isal_deflate_icf_finish_hash_map_base, isal_deflate_icf_finish_hash_map_base, isal_deflate_icf_finish_hash_map_base, isal_deflate_icf_finish_hash_map_base

mbin_interface		isal_update_histogram
mbin_dispatch_init5	isal_update_histogram, isal_update_histogram_base, isal_update_histogram_01, isal_update_histogram_01, isal_update_histogram_04

mbin_interface		encode_deflate_icf
mbin_dispatch_init7_vbmi	encode_deflate_icf, encode_deflate_icf_base, encode_deflate_icf_base, encode_deflate_icf_base, encode_deflate_icf_04, encode_deflate_icf_06, encode_deflate_icf_10

;; set_long_icf_fg_06 only works with the match maps of gen_icf_map_lh1_06, which extends the
;; matches of 8 bytes that it finds, so the two must be dispatched together
mbin_interface		set_long_icf_fg
mbin_dispatch_init6	set_long_icf_fg, set_long_icf_fg_base, set_long_icf_fg_base, set_long_icf_fg_base, set_long_icf_fg_04, set_long_icf_fg_06

;; gen_icf_map_lh1_06 extends the matches of 8 bytes that set_long_icf_fg_06 would, and the
;; other versions leave them to set_long_icf_fg, so the two must be dispatched together.
;; igzip_icf_map_test checks the pair.
mbin_interface		gen_icf_map_lh1
mbin_dispatch_init6	gen_icf_map_lh1, gen_icf_map_h1_base, gen_icf_map_h1_base, gen_icf_map_h1_base, gen_icf_map_lh1_04, gen_icf_map_lh1_06

mbin_interface		parse_icf_map_g
mbin_dispatch_init6	parse_icf_map_g, parse_icf_map_g_base, parse_icf_map_g_base, parse_icf_map_g_base, parse_icf_map_g_base, parse_icf_map_g_06

mbin_interface		isal_adler32
mbin_dispatch_init8	isal_adler32, adler32_base, adler32_sse, adler32_sse, adler32_avx2_4, adler32_avx2_4, adler32_avx2_4, adler32_avx512

mbin_interface		isal_deflate_hash_lvl0
mbin_dispatch_init5	isal_deflate_hash_lvl0, isal_deflate_hash_base, isal_deflate_hash_crc_01, isal_deflate_hash_crc_01, isal_deflate_hash_crc_01

mbin_interface		isal_deflate_hash_lvl1
mbin_dispatch_init5	isal_deflate_hash_lvl1, isal_deflate_hash_base, isal_deflate_hash_crc_01, isal_deflate_hash_crc_01, isal_deflate_hash_crc_01

mbin_interface		isal_deflate_hash_lvl2
mbin_dispatch_init5	isal_deflate_hash_lvl2, isal_deflate_hash_base, isal_deflate_hash_crc_01, isal_deflate_hash_crc_01, isal_deflate_hash_crc_01

mbin_interface		isal_deflate_hash_lvl3
mbin_dispatch_init5	isal_deflate_hash_lvl3, isal_deflate_hash_base, isal_deflate_hash_base, isal_deflate_hash_base, isal_deflate_hash_mad_base
