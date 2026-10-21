;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;  Copyright(c) 2026 Intel Corporation All rights reserved.
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

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;       Function API:
;       UINT32 crc32_gzip_refl_by16_10_hybrid(
;               UINT32 init_crc, //initial CRC value, 32 bits
;               const unsigned char *buf, //buffer pointer to calculate CRC on
;               UINT64 len //buffer length in bytes (64-bit data)
;       );
;
;       crc32_gzip_refl_by16_10 for CPUs where vpclmulqdq limits the folding
;       loop and loads and vpternlogq have room to spare, like AMD Zen 5.
;
;       Q(x) = x^300 + x^155 + x^117 + x^89 + 1 is a multiple of the CRC
;       polynomial P(x), and so is Q(x)^32 = x^9600 + x^4960 + x^3744 + x^2848 + 1.
;       Within a message, a byte at distance d from the end therefore
;       contributes the same CRC as the same byte xored in at the distances
;       d - 1200, d - 844, d - 732 and d - 580 bytes, so each 64 byte block
;       but the last 1200 bytes can be pushed forward with xors alone:
;
;               r[i] = m[i] ^ r[i - 1200] ^ r[i - 844] ^ r[i - 732] ^ r[i - 580]
;
;       where only blocks before the last 1200 bytes push. The message then
;       has the same CRC as its last 1200 bytes of r, the residue.
;
;       For buffers of at least 8 KB, the first 4/7 of the buffer is folded
;       with vpclmulqdq while the rest goes through the recurrence in the same
;       loop. The folded part is reduced to 16 bytes, moved to the end of the
;       buffer with three folds by table constants, and xored into the end of
;       the residue, whose CRC crc32_gzip_refl_by16_10 then computes together
;       with the bytes after the last whole 64 byte block.
;
;       With CRC32_NORM defined, this builds the same for crc32_ieee, whose
;       folding swaps the bytes of each 16 byte lane.
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

%include "reg_sizes.asm"
%include "include/crc.inc"

%ifndef FUNCTION_NAME
%define FUNCTION_NAME crc32_gzip_refl_by16_10_hybrid
%endif

%ifdef CRC32_NORM
 %define BASE_FUNCTION	crc32_ieee_by16_10
 %define CONST_LABEL	crc32_ieee_const
 %define FOLD_IMM_A	0x00		; vpclmulqdq imm8 values of a fold
 %define FOLD_IMM_B	0x11
%else
 %define BASE_FUNCTION	crc32_gzip_refl_by16_10
 %define CONST_LABEL	crc32_gzip_refl_const
 %define FOLD_IMM_A	0x10
 %define FOLD_IMM_B	0x01
%endif

%ifndef HYBRID_MIN
%define HYBRID_MIN	8192			; shorter buffers are only folded
%endif
%define CHUNK		(16 * 1024 * 1024)	; longer buffers go in pieces this long
%define RES_LEN		1200			; residue bytes, the longest distance
%define DIST2		844
%define DIST3		732
%define DIST4		580			; the shortest distance
%define WIN_LEN		8192			; window for r, slid back when full
%define WIN_OFF		64			; window start above the stack pointer
%define STACK_LEN	(WIN_OFF + WIN_LEN + 64)

extern BASE_FUNCTION
extern CONST_LABEL
%ifdef CRC32_NORM
extern bswap_shuf_mask
%endif

[bits 64]
default rel

section .text

%ifidn __OUTPUT_FORMAT__, win64
	%xdefine	arg1 rcx
	%xdefine	arg2 rdx
	%xdefine	arg3 r8

	%xdefine	arg1_low32 ecx
%else
	%xdefine	arg1 rdi
	%xdefine	arg2 rsi
	%xdefine	arg3 rdx

	%xdefine	arg1_low32 edi
%endif

%define crc		ebx	; crc of the bytes before buf
%define buf		r12
%define len		r13
%define a_ptr		r14	; start of the recurrence part
%define w		r15	; r[i] is at [w + i]
%define b_ptr		rax	; next 256 bytes to fold
%define b_end		r10
%define idx		r11	; i, bytes of the recurrence part done
%define a_len		r9
%define full_lim	r8	; below this i, all four sources take part
%define tmp1		rcx
%define tmp2		rdx
%define tmp3		rsi
%define tmp4		rdi
%define tmp3_32		esi
%define chunk_len	[rsp + 32]	; above the shadow space of the call

;; Fold zmm%1 with the 64 bytes at %2, using zmm%3 and zmm%4
%macro FOLD64 4
%ifdef CRC32_NORM
	vmovdqu64	zmm%4, [%2]
	vpshufb		zmm%4, zmm%4, zmm31
	vpclmulqdq	zmm%3, zmm%1, zmm20, FOLD_IMM_A
	vpclmulqdq	zmm%1, zmm%1, zmm20, FOLD_IMM_B
	vpternlogq	zmm%1, zmm%3, zmm%4, 0x96
%else
	vpclmulqdq	zmm%3, zmm%1, zmm20, FOLD_IMM_A
	vpclmulqdq	zmm%1, zmm%1, zmm20, FOLD_IMM_B
	vpternlogq	zmm%1, zmm%3, [%2], 0x96
%endif
%endmacro

;; Fold the 256 bytes in zmm16-zmm19 with the 256 bytes at %1
%macro FOLD256 1
	FOLD64	16, %1 + 64*0, 24, 0
	FOLD64	17, %1 + 64*1, 25, 1
	FOLD64	18, %1 + 64*2, 26, 2
	FOLD64	19, %1 + 64*3, 27, 3
%endmacro

;; r[i] for the 64 bytes at i = idx + %1, with all four sources
%macro REC64 3
	vmovdqu64	%2, [a_ptr + idx + %1]
	vmovdqu64	%3, [w + idx + %1 - RES_LEN]
	vpternlogq	%2, %3, [w + idx + %1 - DIST2], 0x96
	vmovdqu64	%3, [w + idx + %1 - DIST3]
	vpternlogq	%2, %3, [w + idx + %1 - DIST4], 0x96
	vmovdqu64	[w + idx + %1], %2
%endmacro

;; Set mask %1 to the bytes of the block at idx that the source at distance
;; %2 reaches: only bytes before the residue push, so the source at byte
;; t takes part for t < a_len - RES_LEN + %2
%macro SRC_MASK 2
	lea	tmp3, [a_len + %2 - RES_LEN]
	xor	tmp4, tmp4
	sub	tmp3, idx
	cmovl	tmp3, tmp4
	mov	tmp4, 64
	cmp	tmp3, tmp4
	cmova	tmp3, tmp4
	mov	tmp4, -1
	bzhi	tmp4, tmp4, tmp3
	kmovq	%1, tmp4
%endmacro

;; Move the last RES_LEN bytes of r to the start of the window if the next
;; %1 bytes of r do not fit
%macro SLIDE 1
	lea	tmp3, [w + idx + %1]
	lea	tmp4, [rsp + WIN_OFF + WIN_LEN]
	cmp	tmp3, tmp4
	jbe	%%fits
	lea	tmp3, [w + idx - RES_LEN]
%assign j 0
%rep (RES_LEN + 63) / 64
	vmovdqu64	zmm24, [tmp3 + 64*j]
	vmovdqa64	[rsp + WIN_OFF + 64*j], zmm24
%assign j (j + 1)
%endrep
	lea	w, [rsp + WIN_OFF + RES_LEN]
	sub	w, idx
%%fits:
%endmacro

align 16
mk_global FUNCTION_NAME, function
FUNCTION_NAME:
	endbranch

	cmp	arg3, HYBRID_MIN
	jb	BASE_FUNCTION WRT_OPT

	push	rbx
	push	rbp
	push	r12
	push	r13
	push	r14
	push	r15
%ifidn __OUTPUT_FORMAT__, win64
	push	rsi
	push	rdi
%endif
	mov	rbp, rsp

	; Allocate the window a page at a time, as Windows commits the stack
%assign j 0
%rep (STACK_LEN + 4095) / 4096
	sub	rsp, 4096
	test	[rsp], esp
%endrep
	and	rsp, -64

	mov	crc, arg1_low32
	mov	buf, arg2
	mov	len, arg3

.next_chunk:
	; Buffers of 2 * CHUNK bytes and more go in CHUNK byte pieces, which keeps
	; the distances within the advance tables
	mov	tmp2, len
	mov	tmp1, CHUNK
	cmp	len, 2 * CHUNK
	cmovae	tmp2, tmp1

	; Of the whole 64 byte blocks, 4/7 are folded, in whole 256 byte groups,
	; and the rest goes through the recurrence
	mov	tmp1, tmp2
	and	tmp1, -64
	mov	a_len, tmp1
	shr	tmp1, 6
	imul	tmp1, tmp1, 0x24924925
	shr	tmp1, 32
	shl	tmp1, 8
	sub	a_len, tmp1
	lea	b_end, [buf + tmp1]
	mov	a_ptr, b_end
	mov	chunk_len, tmp2

	lea	tmp1, [rel CONST_LABEL]
	vbroadcasti32x4	zmm20, [tmp1 + crc_fold_const_fold_16x128b]

	; Load the first 256 bytes with the initial crc
	mov	tmp3_32, crc
	not	tmp3_32
	vmovd	xmm24, tmp3_32
%ifdef CRC32_NORM
	vbroadcasti32x4	zmm31, [bswap_shuf_mask]
	vpslldq		xmm24, xmm24, 12
	vmovdqu64	zmm16, [buf + 64*0]
	vmovdqu64	zmm17, [buf + 64*1]
	vmovdqu64	zmm18, [buf + 64*2]
	vmovdqu64	zmm19, [buf + 64*3]
	vpshufb		zmm16, zmm16, zmm31
	vpshufb		zmm17, zmm17, zmm31
	vpshufb		zmm18, zmm18, zmm31
	vpshufb		zmm19, zmm19, zmm31
	vpxorq		zmm16, zmm16, zmm24
%else
	vpxorq		zmm16, zmm24, [buf + 64*0]
	vmovdqu64	zmm17, [buf + 64*1]
	vmovdqu64	zmm18, [buf + 64*2]
	vmovdqu64	zmm19, [buf + 64*3]
%endif
	lea	b_ptr, [buf + 256]

	; The sources before the recurrence part are 0
	vpxorq	zmm24, zmm24, zmm24
%assign j 0
%rep (RES_LEN + 63) / 64
	vmovdqa64	[rsp + WIN_OFF + 64*j], zmm24
%assign j (j + 1)
%endrep
	lea	w, [rsp + WIN_OFF + RES_LEN]
	xor	idx, idx
	lea	full_lim, [a_len + DIST4 - RES_LEN]

	; Fold 512 bytes and run the recurrence on 384 bytes at a time
align 16
.fold_and_rec_loop:
	lea	tmp1, [b_ptr + 512]
	cmp	tmp1, b_end
	ja	.tail
	lea	tmp1, [idx + 384]
	cmp	tmp1, full_lim
	ja	.tail
	SLIDE	384

	FOLD256	b_ptr
	REC64	64*0, zmm21, zmm22
	REC64	64*1, zmm23, zmm28
	FOLD256	b_ptr + 256
	REC64	64*2, zmm29, zmm30
	REC64	64*3, zmm21, zmm22
	REC64	64*4, zmm23, zmm28
	REC64	64*5, zmm29, zmm30

	add	b_ptr, 512
	add	idx, 384
	jmp	.fold_and_rec_loop

	; Fold the rest of the folded part, 256 bytes at a time, alongside the
	; rest of the recurrence, whose last blocks take masked sources
.tail:
	cmp	b_ptr, b_end
	jae	.tail_rec
	FOLD256	b_ptr
	add	b_ptr, 256
.tail_rec:
	mov	tmp2, 3
.tail_rec_loop:
	cmp	idx, a_len
	jae	.tail_next
	SLIDE	64
	lea	tmp1, [idx + 64]
	cmp	tmp1, full_lim
	ja	.tail_rec_masked
	REC64	0, zmm21, zmm22
	jmp	.tail_rec_next
.tail_rec_masked:
	SRC_MASK	k2, DIST2
	SRC_MASK	k3, DIST3
	SRC_MASK	k4, DIST4
	vmovdqu64	zmm21, [a_ptr + idx]
	vmovdqu64	zmm22, [w + idx - RES_LEN]
	vmovdqu8	zmm23{k2}{z}, [w + idx - DIST2]
	vpternlogq	zmm21, zmm22, zmm23, 0x96
	vmovdqu8	zmm22{k3}{z}, [w + idx - DIST3]
	vmovdqu8	zmm23{k4}{z}, [w + idx - DIST4]
	vpternlogq	zmm21, zmm22, zmm23, 0x96
	vmovdqu64	[w + idx], zmm21
.tail_rec_next:
	add	idx, 64
	dec	tmp2
	jnz	.tail_rec_loop
.tail_next:
	cmp	b_ptr, b_end
	jb	.tail
	cmp	idx, a_len
	jb	.tail

	; Fold the last 256 folded bytes into 16 bytes, as BASE_FUNCTION does
	lea	tmp1, [rel CONST_LABEL]
	vbroadcasti32x4	zmm20, [tmp1 + crc_fold_const_fold_8x128b]
	vpclmulqdq	zmm24, zmm16, zmm20, FOLD_IMM_A
	vpclmulqdq	zmm25, zmm16, zmm20, FOLD_IMM_B
	vpternlogq	zmm18, zmm24, zmm25, 0x96
	vpclmulqdq	zmm24, zmm17, zmm20, FOLD_IMM_A
	vpclmulqdq	zmm25, zmm17, zmm20, FOLD_IMM_B
	vpternlogq	zmm19, zmm24, zmm25, 0x96

	vmovdqu8	zmm20, [tmp1 + crc_fold_const_fold_7x128b]
	vmovdqu8	zmm21, [tmp1 + crc_fold_const_fold_3x128b]
	vpclmulqdq	zmm24, zmm18, zmm20, FOLD_IMM_A
	vpclmulqdq	zmm25, zmm18, zmm20, FOLD_IMM_B
	vextracti64x2	xmm28, zmm19, 3
	vpclmulqdq	zmm26, zmm19, zmm21, FOLD_IMM_A
	vpclmulqdq	zmm27, zmm19, zmm21, FOLD_IMM_B
	vpternlogq	zmm24, zmm25, zmm26, 0x96
	vpternlogq	zmm24, zmm27, zmm28, 0x96
	vshufi64x2	zmm25, zmm24, zmm24, 0x4e
	vpxorq		ymm25, ymm25, ymm24
	vextracti64x2	xmm26, ymm25, 1
	vpxorq		xmm28, xmm26, xmm25

	; Move the 16 bytes to the end of the recurrence part, a_len bytes on,
	; with folds by (a_len / 64) % 64, (a_len / 4096) % 64 and a_len / 262144
	; times 64 bytes
	lea	tmp1, [rel advance_consts]
	mov	tmp3, a_len
	shr	tmp3, 6
%assign j 0
%rep 3
	mov	tmp4, tmp3
	and	tmp4, 63
	shl	tmp4, 4
	vmovdqa64	xmm20, [tmp1 + tmp4 + 64*16*j]
	vpclmulqdq	xmm24, xmm28, xmm20, FOLD_IMM_A
	vpclmulqdq	xmm25, xmm28, xmm20, FOLD_IMM_B
	vpxorq		xmm28, xmm24, xmm25
	shr	tmp3, 6
%assign j (j + 1)
%endrep

	; Xor them into the end of the residue, and put the bytes after the last
	; whole block after it
%ifdef CRC32_NORM
	vpshufb		xmm28, xmm28, xmm31
%endif
	vpxorq		xmm28, xmm28, [w + a_len - 16]
	vmovdqu64	[w + a_len - 16], xmm28
	mov	tmp2, chunk_len
	mov	tmp1, tmp2
	and	tmp1, 63
	mov	tmp4, -1
	bzhi	tmp4, tmp4, tmp1
	kmovq	k1, tmp4
	lea	tmp3, [buf + tmp2]
	sub	tmp3, tmp1
	vmovdqu8	zmm24{k1}{z}, [tmp3]
	vmovdqu8	[w + a_len]{k1}, zmm24

	; The crc of the chunk is the crc of the residue and those bytes, from 0
	add	buf, tmp2
	sub	len, tmp2
	lea	arg3, [tmp1 + RES_LEN]
	lea	arg2, [w + a_len - RES_LEN]
	mov	arg1_low32, 0xffffffff
	call	BASE_FUNCTION WRT_OPT
	mov	crc, eax

	cmp	len, 0
	jne	.next_chunk

	mov	rsp, rbp
%ifidn __OUTPUT_FORMAT__, win64
	pop	rdi
	pop	rsi
%endif
	pop	r15
	pop	r14
	pop	r13
	pop	r12
	pop	rbp
	pop	rbx
	ret

section .rodata
align 16
; Fold constants for moving 16 bytes by 64 * j bytes, 4096 * j bytes and
; 262144 * j bytes, j = 0..63, in the layout of crc_fold_const_fold_16x128b
advance_consts:
%ifdef CRC32_NORM
	dq 0xcbf1acda00000000, 0x04c11db700000000
	dq 0xd3504ec700000000, 0x57a8445500000000
	dq 0x022ffca500000000, 0x9d9ee22f00000000
	dq 0x46a352e900000000, 0x07a3708300000000
	dq 0x1851689900000000, 0xa3dc855100000000
	dq 0xd49c3b5100000000, 0x6757b2e000000000
	dq 0xaf85919600000000, 0x1b4953df00000000
	dq 0x04b2cd6600000000, 0x9ba9616a00000000
	dq 0x759f05d700000000, 0x29b69c1000000000
	dq 0xc9a8ab0800000000, 0xa6585f6d00000000
	dq 0xf6de958600000000, 0xb814a7be00000000
	dq 0xd09c752e00000000, 0x6253abf200000000
	dq 0x0a21cd7000000000, 0xb947885000000000
	dq 0x260bae0000000000, 0x6e8cb12500000000
	dq 0x2795b0b200000000, 0x004c2a9300000000
	dq 0xc4e3ea3600000000, 0xcd6650cc00000000
	dq 0xd6de9fba00000000, 0xe9de0f8900000000
	dq 0x6c3cc8e600000000, 0x2d728c4100000000
	dq 0x9bf9240100000000, 0x0a3990b400000000
	dq 0xb1d2ccb300000000, 0x87179ab800000000
	dq 0x2a4e0e3a00000000, 0xacc85a3a00000000
	dq 0xfabcf1ed00000000, 0x8feacd1000000000
	dq 0x6345af4f00000000, 0x06e06ad800000000
	dq 0xfbc364eb00000000, 0x2325171300000000
	dq 0x43ab95d000000000, 0xf9b8dd1100000000
	dq 0x08f6549700000000, 0x9b716d4100000000
	dq 0x1bbe440c00000000, 0x9f0340cd00000000
	dq 0x48e9c08000000000, 0x5fa4a0f500000000
	dq 0x4f7987f800000000, 0x446b88bc00000000
	dq 0x3be8deb000000000, 0x051f7d5600000000
	dq 0x7544d3d800000000, 0xf7a95fd100000000
	dq 0x16094be300000000, 0xbb22a01700000000
	dq 0xdeebb80b00000000, 0x887bc79500000000
	dq 0x30f90b9000000000, 0x23b6808a00000000
	dq 0x5cfe15d700000000, 0x5c51662500000000
	dq 0x810edf2400000000, 0x8eacb71c00000000
	dq 0x3c37488f00000000, 0x340a14b400000000
	dq 0xf9d37a4300000000, 0x08fdd7b900000000
	dq 0x11509a6c00000000, 0x1cab728400000000
	dq 0x42cfc32400000000, 0x5000c8b900000000
	dq 0x0883d31100000000, 0x0c0d163700000000
	dq 0x74ac497100000000, 0x54c2fb1200000000
	dq 0x42fc743b00000000, 0xac670c2200000000
	dq 0x7ba715ad00000000, 0x9e9fe6f600000000
	dq 0x7235cf0400000000, 0x6eb4db7700000000
	dq 0x96cac82700000000, 0x099af1a100000000
	dq 0x6ebac46000000000, 0x1a49a26400000000
	dq 0xac9c86e100000000, 0xa7b01d0c00000000
	dq 0x9a27ebbd00000000, 0xdb51575a00000000
	dq 0xa9d9287100000000, 0xb46b7c3800000000
	dq 0xd89af79400000000, 0xdca6fa3d00000000
	dq 0xf32e2a1100000000, 0x880814b500000000
	dq 0x290a1bee00000000, 0x848f01bc00000000
	dq 0xa125ade900000000, 0xa0a0788100000000
	dq 0x1bb2072e00000000, 0xb9244c5700000000
	dq 0x2a2c5c9e00000000, 0x105bc63b00000000
	dq 0x1f68cb9300000000, 0x0442f41a00000000
	dq 0x0ecc8d3c00000000, 0x10dd390100000000
	dq 0x9fd881b100000000, 0xd02ee69800000000
	dq 0x28e7e47900000000, 0xedd0965200000000
	dq 0x4bd1315600000000, 0x642e9b2e00000000
	dq 0xa964960200000000, 0x170da03d00000000
	dq 0x4ffb5d4300000000, 0xccd7821300000000
	dq 0x438dbf6600000000, 0x2e31ff8000000000
	dq 0xcbf1acda00000000, 0x04c11db700000000
	dq 0x424b1b9b00000000, 0x692a36b400000000
	dq 0x51d6243f00000000, 0x8edecd5a00000000
	dq 0x2cc701f300000000, 0xfb515c6200000000
	dq 0xfef4ee0e00000000, 0xffe80ec500000000
	dq 0x6f966ae900000000, 0x8820db7900000000
	dq 0x446483fb00000000, 0x2832f46300000000
	dq 0xbe80755900000000, 0x5b03419400000000
	dq 0xa6859a9f00000000, 0x68b84ee800000000
	dq 0x7145bf0900000000, 0x1e1ef17700000000
	dq 0x75f8c89a00000000, 0x6823087900000000
	dq 0x9085fdc900000000, 0x89fc95c100000000
	dq 0x2059232400000000, 0x774f538e00000000
	dq 0xee661e9d00000000, 0xf920aa2d00000000
	dq 0xad351e9a00000000, 0xf3c2f63300000000
	dq 0x42f9e01200000000, 0xae077de000000000
	dq 0x91280e1300000000, 0xef1145b000000000
	dq 0xcb31521300000000, 0x96de410d00000000
	dq 0x8321023500000000, 0xcd8f5b0e00000000
	dq 0x8adb13ba00000000, 0x22cc0eb000000000
	dq 0xc66d5f0d00000000, 0x95f7f09d00000000
	dq 0x0e804de600000000, 0xe0c1ec6e00000000
	dq 0x72adb65700000000, 0x44ce2a1900000000
	dq 0x0ab1f7f300000000, 0x95750d0c00000000
	dq 0xfb92654400000000, 0xb15b575e00000000
	dq 0x1013465900000000, 0x9ddab94f00000000
	dq 0x147876a300000000, 0x8fe99e7000000000
	dq 0x7ac24b3400000000, 0x8c8f117600000000
	dq 0x502f97d000000000, 0x9a9e56e200000000
	dq 0x42932cac00000000, 0xa1b6e82300000000
	dq 0x5120d90500000000, 0xe8234c8c00000000
	dq 0x1cdc396e00000000, 0xec5e5e0700000000
	dq 0x6963d73c00000000, 0x80950e7a00000000
	dq 0xdd02b93300000000, 0xf3fd514200000000
	dq 0x2639df2800000000, 0x89563f6900000000
	dq 0x8d54b07800000000, 0x6b51fb4800000000
	dq 0x3509b76300000000, 0x1b89a57b00000000
	dq 0x7fa1c21f00000000, 0xce05e28300000000
	dq 0x2a05dd4b00000000, 0xbb87ffa800000000
	dq 0x309b885f00000000, 0xd8535fc500000000
	dq 0xe0162a3100000000, 0x9738a98a00000000
	dq 0x27bccf7f00000000, 0x4156345500000000
	dq 0x4da8b3aa00000000, 0x46f40d3f00000000
	dq 0x0c55dc8c00000000, 0xa09d572900000000
	dq 0x276c32bf00000000, 0x10a7e82000000000
	dq 0xaa32d5de00000000, 0x3a4b2dc600000000
	dq 0x0f54c95300000000, 0xf0678f3500000000
	dq 0x3c27238a00000000, 0xf77ee15500000000
	dq 0xe03645ac00000000, 0xd91563de00000000
	dq 0x3cc3aca900000000, 0x5abf634600000000
	dq 0x3683557c00000000, 0x25a81dd200000000
	dq 0x9fb27bc500000000, 0x83c1d25b00000000
	dq 0x24137d0400000000, 0x8207f3c600000000
	dq 0xa049df0300000000, 0x0e16ef6500000000
	dq 0x2bb2227f00000000, 0x9853622300000000
	dq 0x5970067900000000, 0xf2a2bd1100000000
	dq 0xe3784d3700000000, 0xb88c25bf00000000
	dq 0x7a98f7f000000000, 0xf66015eb00000000
	dq 0x5db1a3ab00000000, 0xce7ce37500000000
	dq 0x75cf1c1b00000000, 0x4526bfdd00000000
	dq 0x10c74a3100000000, 0x0acabf2b00000000
	dq 0x012e67d700000000, 0xcdfea02b00000000
	dq 0x382237d400000000, 0x470de33200000000
	dq 0xa17b8c3100000000, 0xbc5ee8da00000000
	dq 0xcbf1acda00000000, 0x04c11db700000000
	dq 0xa1a9f9ad00000000, 0xeb8f3c4900000000
	dq 0xe14bfd1b00000000, 0xe9d0ddec00000000
	dq 0xeef8fc4e00000000, 0x9e4ebc8200000000
	dq 0xf2e7dcd300000000, 0x3e66436300000000
	dq 0x1fd59c4a00000000, 0xa542211000000000
	dq 0xb362f8bf00000000, 0xe06c3a2100000000
	dq 0xf1b7679b00000000, 0x68ad293500000000
	dq 0xfe0c995100000000, 0x6b93343800000000
	dq 0x9895def300000000, 0xad7ce6dd00000000
	dq 0xb9afc49b00000000, 0x777b78e100000000
	dq 0x8e68512800000000, 0x607cfd7200000000
	dq 0x4a44b97000000000, 0x4fe6978a00000000
	dq 0xaa7a421300000000, 0xace69baf00000000
	dq 0x0063192400000000, 0xdaed9d5600000000
	dq 0x54231f6600000000, 0xf93beafd00000000
	dq 0x50a5cf8500000000, 0x09bc485a00000000
	dq 0x4c7bfed200000000, 0x2a5fc29700000000
	dq 0x37fdf68600000000, 0xa8374dfa00000000
	dq 0x3e5c69ad00000000, 0x9f302f2700000000
	dq 0x8f8d8beb00000000, 0xc03c2eea00000000
	dq 0x3074e68700000000, 0xc061161300000000
	dq 0x72aea15000000000, 0x994d516100000000
	dq 0x5ff4491000000000, 0x7ebd01ec00000000
	dq 0xb44b972c00000000, 0xe78ca3dc00000000
	dq 0xe76de17100000000, 0x8e3ecbe300000000
	dq 0xabefc9bd00000000, 0xe62b813900000000
	dq 0x8c05709500000000, 0x58388f9200000000
	dq 0x11b3f6f000000000, 0x5e52676a00000000
	dq 0x7de6502e00000000, 0xe9cb250400000000
	dq 0xae4427a900000000, 0xad62851400000000
	dq 0x9c90a45600000000, 0x376f9fd100000000
	dq 0xc209d48400000000, 0xce02089b00000000
	dq 0x21a3219f00000000, 0x77fa8a9100000000
	dq 0x0b8ecca600000000, 0xbd047cf900000000
	dq 0xcab482b000000000, 0x808dea9200000000
	dq 0xfe23cb7100000000, 0x6b1a7aeb00000000
	dq 0xe5ab08e000000000, 0x3f38a11700000000
	dq 0xd6d6dc2a00000000, 0xe95c78e500000000
	dq 0xef6db0d200000000, 0xf2b86da400000000
	dq 0x30b3640c00000000, 0x95ad8d5100000000
	dq 0xeb7036f200000000, 0x87204e9f00000000
	dq 0x2974193e00000000, 0x5486fa9f00000000
	dq 0x3ddc7e0800000000, 0xce93af5e00000000
	dq 0xd21c2af600000000, 0xcff960c600000000
	dq 0x908bb71900000000, 0xeaeeea9a00000000
	dq 0x32282eef00000000, 0xa73520e100000000
	dq 0x506c3a0000000000, 0x3422fc8400000000
	dq 0x9bfa9a1b00000000, 0x7399d37a00000000
	dq 0x77d70d7100000000, 0x1c1b87c100000000
	dq 0x27b4e19500000000, 0x02554eeb00000000
	dq 0x9af9aa6100000000, 0x724c308c00000000
	dq 0x2bb9e9d700000000, 0xf4fdf4ca00000000
	dq 0x18c4b78600000000, 0x7cfac1fa00000000
	dq 0x8ac653a000000000, 0x41e882da00000000
	dq 0x7930710b00000000, 0x6263a70a00000000
	dq 0x805f7dfd00000000, 0x9311cf7d00000000
	dq 0x9baca76c00000000, 0xab0487d100000000
	dq 0x0d1e592600000000, 0xc2357c9b00000000
	dq 0xd698a9fd00000000, 0xa3bedf0500000000
	dq 0xf9b307fb00000000, 0x38d5d16200000000
	dq 0x12c29af100000000, 0x031335e900000000
	dq 0x71278b2400000000, 0x45cabe3a00000000
	dq 0xd40c5cfa00000000, 0xc1e5bbc800000000
%else
	dq 0x00000000b66b1fa6, 0x0000000000000001
	dq 0x000000001d9513d7, 0x000000008f352d95
	dq 0x00000000910eeec1, 0x0000000033fff533
	dq 0x00000000f5e48c85, 0x00000000596c8d81
	dq 0x00000000e95c1271, 0x00000000ce3371cb
	dq 0x00000000cec97417, 0x000000000e9bd5cc
	dq 0x00000000d31343ea, 0x000000002ce423f1
	dq 0x00000000cd669a40, 0x00000000ad0d2bb2
	dq 0x000000000c30f51d, 0x000000001072db28
	dq 0x0000000021aa2b26, 0x00000000b685328b
	dq 0x00000000c352f6de, 0x00000000fbca503a
	dq 0x00000000e95c7216, 0x000000009fab948c
	dq 0x000000001d6708a0, 0x000000001423c53a
	dq 0x0000000000eba0c8, 0x00000000926b64ad
	dq 0x000000009a1b53c8, 0x0000000049d96241
	dq 0x00000000d8af8e46, 0x000000006614cd66
	dq 0x00000000bbf2f6d6, 0x00000000f891f16f
	dq 0x00000000ce26786c, 0x00000000df139b29
	dq 0x00000000db3839f3, 0x000000005a1338a0
	dq 0x000000004117915b, 0x000000003ab3d1c2
	dq 0x00000000b8e0e4a8, 0x00000000b8b4266a
	dq 0x00000000b46f7cff, 0x000000001166afe2
	dq 0x000000003e9a43cd, 0x0000000036ac0ec0
	dq 0x00000000753c81ff, 0x000000004aa04fc9
	dq 0x000000001753ab84, 0x00000000ca073d7f
	dq 0x000000000925d861, 0x00000000de1c1bf3
	dq 0x000000006044fbb0, 0x00000000bd7487b3
	dq 0x0000000002072e24, 0x00000000857b4db5
	dq 0x000000003fc33de4, 0x000000007a23ac44
	dq 0x000000001af62fb8, 0x00000000d57df140
	dq 0x000000003796455c, 0x00000000cc842d9f
	dq 0x0000000054d42691, 0x000000000b7b8ffb
	dq 0x000000007b4aa8b7, 0x0000000088b6ba63
	dq 0x0000000013a13e18, 0x00000000a202db88
	dq 0x000000000c21f835, 0x0000000093bc1235
	dq 0x0000000049f6e102, 0x0000000071da6ae2
	dq 0x000000003954de39, 0x000000005a50a058
	dq 0x000000005fcc917f, 0x00000000e0a67861
	dq 0x000000006cb21510, 0x00000000429daa70
	dq 0x000000004987e684, 0x00000000e1570655
	dq 0x00000000cae68461, 0x0000000003a06621
	dq 0x00000000c6556c1d, 0x0000000091be8654
	dq 0x00000000632d78c5, 0x000000008861cc6a
	dq 0x00000000b020cdfd, 0x00000000decff2f2
	dq 0x0000000041e7589c, 0x0000000006c75cad
	dq 0x000000001357a093, 0x00000000d06fb561
	dq 0x000000000c46baec, 0x000000004c8b24b0
	dq 0x00000000d5b3742b, 0x0000000061701bca
	dq 0x00000000a0decef3, 0x00000000b5d515b6
	dq 0x00000000c758316b, 0x00000000387dac5a
	dq 0x0000000053deb236, 0x00000000a3cfcc37
	dq 0x00000000cbd9efdf, 0x0000000081212663
	dq 0x00000000efb0a128, 0x000000007b01e242
	dq 0x00000000f41a4f4b, 0x00000000d94d0c4b
	dq 0x00000000e9c09bb0, 0x000000000f154f7b
	dq 0x00000000f27468a8, 0x0000000063b6b251
	dq 0x0000000048d72bb1, 0x00000000b05e8440
	dq 0x00000000796266e0, 0x00000000da487051
	dq 0x00000000c07331b3, 0x0000000032cee816
	dq 0x00000000e73ec869, 0x0000000094d2176e
	dq 0x00000000d51917a4, 0x00000000e9b2e84c
	dq 0x0000000080d24d2a, 0x00000000a37a6791
	dq 0x000000005e04b9a5, 0x000000004bf2d027
	dq 0x00000000cdfb6384, 0x0000000003ff18e8
	dq 0x00000000b66b1fa6, 0x0000000000000001
	dq 0x0000000068c0a2c5, 0x000000005ad8a92c
	dq 0x000000002339d155, 0x00000000b566f6e2
	dq 0x000000004470c029, 0x000000008c7515be
	dq 0x00000000e0ee5efe, 0x000000009d9129bf
	dq 0x00000000f5ddd5ad, 0x00000000e6c70e63
	dq 0x0000000064f34a05, 0x00000000572f9e69
	dq 0x00000000ee2d04bb, 0x00000000530581b4
	dq 0x0000000029c2448b, 0x000000002ee43a2c
	dq 0x00000000fa8a435d, 0x00000000066ff6b1
	dq 0x00000000b2263f5c, 0x00000000e7508e6d
	dq 0x00000000fc0e4453, 0x00000000dc237963
	dq 0x0000000049893408, 0x00000000e395e5dc
	dq 0x00000000a981caaf, 0x00000000b3db0f7f
	dq 0x00000000b2f1596a, 0x0000000043af81df
	dq 0x00000000900f3e84, 0x000000000f7dc0ea
	dq 0x000000004b912f53, 0x000000001b4511ee
	dq 0x000000004be41fe7, 0x00000000ba75f093
	dq 0x0000000083f00fc3, 0x00000000e1b5e366
	dq 0x00000000bb91b6a2, 0x000000001ae06688
	dq 0x00000000ba846a87, 0x00000000a96ed913
	dq 0x00000000cf6402e0, 0x00000000ec6f060e
	dq 0x000000000faa6cdd, 0x00000000ebd9e005
	dq 0x0000000044ae1ce1, 0x0000000061615d52
	dq 0x00000000454c93be, 0x00000000f5d5b51a
	dq 0x00000000efb49651, 0x000000003e4bb133
	dq 0x0000000051ad3a11, 0x000000001cf32fe2
	dq 0x0000000059a486bc, 0x00000000dd11e262
	dq 0x0000000017d3e814, 0x000000008ed4f2b2
	dq 0x000000006a699284, 0x00000000535fdd4b
	dq 0x000000009a470f55, 0x000000006265882e
	dq 0x00000000ed387670, 0x000000001b85f22f
	dq 0x0000000079d78d2c, 0x00000000bce15202
	dq 0x00000000424b8737, 0x0000000085157f9e
	dq 0x0000000029f738c8, 0x00000000f689d363
	dq 0x000000003c1a5562, 0x0000000025bf15ac
	dq 0x0000000056aa2719, 0x00000000663a25f1
	dq 0x000000002bf60dbd, 0x0000000059fe46a7
	dq 0x000000007e0646e9, 0x000000002bffc3ba
	dq 0x000000002f52b459, 0x000000009c849277
	dq 0x00000000c3d9d64f, 0x00000000a32a39d2
	dq 0x0000000026977d89, 0x000000008f29d345
	dq 0x00000000ab9a2b64, 0x0000000022115885
	dq 0x0000000062775460, 0x00000000f2a4744b
	dq 0x0000000021e96b89, 0x00000000082fca10
	dq 0x00000000f75698aa, 0x00000000c769a4b8
	dq 0x000000004e5753a1, 0x000000008292ca5f
	dq 0x00000000a389c878, 0x000000008e7ffb9f
	dq 0x000000006b44d80e, 0x00000000f78d5136
	dq 0x00000000f11a8039, 0x00000000c58dfab4
	dq 0x000000007d5582d8, 0x0000000097702b48
	dq 0x000000009ccd9db3, 0x000000006fe601c3
	dq 0x00000000417d9048, 0x00000000c79fc082
	dq 0x000000005a86224b, 0x00000000969fd6a1
	dq 0x0000000027f99de9, 0x0000000053fc9273
	dq 0x00000000e7b11b75, 0x00000000ca0b8cdf
	dq 0x0000000002153bcf, 0x000000002039647b
	dq 0x000000001fde32bc, 0x0000000074210a9f
	dq 0x0000000070fa1d35, 0x0000000086ff7aa7
	dq 0x000000006b00e11d, 0x00000000ac8bcf05
	dq 0x00000000c3d4c051, 0x00000000728ba0e1
	dq 0x000000000cbdef41, 0x00000000737bf927
	dq 0x0000000057d88838, 0x00000000998f61c4
	dq 0x00000000c312bb4b, 0x00000000b62ef47a
	dq 0x00000000b66b1fa6, 0x0000000000000001
	dq 0x00000000b04e2d4b, 0x00000000ff08e5ef
	dq 0x000000006a0ea34f, 0x000000006f76172e
	dq 0x00000000e47e3eee, 0x00000000827ae4f2
	dq 0x000000004d06c8df, 0x0000000056f5cab9
	dq 0x00000000a47357f0, 0x000000001108854a
	dq 0x00000000214f8bdb, 0x00000000d3c96a4f
	dq 0x0000000068bcdd5f, 0x0000000082586c6d
	dq 0x00000000ce4366bf, 0x00000000385993ac
	dq 0x0000000045865473, 0x00000000adbf7b2b
	dq 0x000000006936ed7b, 0x00000000d54cbb9d
	dq 0x0000000029142ce2, 0x000000009d7e7c0c
	dq 0x000000001d3a44a4, 0x00000000a3d2cfe4
	dq 0x000000004bf5baeb, 0x0000000030c3c82b
	dq 0x0000000049318c00, 0x00000000d5736eb6
	dq 0x00000000cdf18854, 0x00000000a5debf7f
	dq 0x0000000098964c55, 0x00000000b4247b20
	dq 0x0000000096ffbc64, 0x0000000009f6f2e9
	dq 0x00000000c2df7fd8, 0x00000000bf65d82a
	dq 0x00000000b05d72b9, 0x0000000012991fb3
	dq 0x0000000074d265a3, 0x00000000aee87806
	dq 0x0000000019bf5a59, 0x000000004ba00a47
	dq 0x00000000150aea9c, 0x00000000d6646373
	dq 0x0000000011245ff4, 0x000000006f017afc
	dq 0x0000000069d3a45a, 0x00000000778a63ce
	dq 0x00000000c67e6b8f, 0x0000000054d7fea3
	dq 0x00000000a056e9eb, 0x00000000e272ae8f
	dq 0x00000000896c4623, 0x0000000093e23834
	dq 0x000000001edf9b10, 0x00000000adcc94f4
	dq 0x00000000e814cf7c, 0x000000004149a72e
	dq 0x00000000f0b942ab, 0x0000000051428d6a
	dq 0x00000000d44a1272, 0x00000000cc82eb99
	dq 0x0000000042572086, 0x00000000695186a7
	dq 0x0000000028788d49, 0x00000000c9d3b99d
	dq 0x00000000ca66e3a0, 0x00000000e50d473b
	dq 0x000000001a825aa6, 0x0000000092af6202
	dq 0x00000000c6d68ebf, 0x0000000075cdb7ed
	dq 0x000000000e21ab4e, 0x000000000a7b3fb9
	dq 0x00000000a876d6d6, 0x00000000954d736f
	dq 0x00000000961b6dee, 0x000000004b6c3a9e
	dq 0x00000000604d9a18, 0x00000000ce126d13
	dq 0x000000009ed81dae, 0x0000000029950f83
	dq 0x00000000f9305d28, 0x0000000029cfc415
	dq 0x0000000020fc7778, 0x00000000f5eb92e6
	dq 0x00000000dea87096, 0x00000000c60d3fe6
	dq 0x00000000eaaaa453, 0x00000000b2aeeeae
	dq 0x0000000035992ed9, 0x00000000d5785f8b
	dq 0x0000000000b86c14, 0x00000000427e8858
	dq 0x000000006bc3b9f3, 0x00000000bd97339c
	dq 0x00000000c610d19d, 0x00000000dcb2b631
	dq 0x00000000887f5d89, 0x00000000759452c1
	dq 0x00000000d7da38f3, 0x000000006218649c
	dq 0x000000000c5e3de9, 0x00000000a65f7e5e
	dq 0x00000000c3da4630, 0x00000000bf06be7c
	dq 0x000000000b94c6a2, 0x00000000b6822f04
	dq 0x000000007a6d1f7d, 0x00000000a1cb8c8c
	dq 0x00000000a40cf243, 0x00000000a69617d3
	dq 0x000000006dca6bb2, 0x00000000ccb347eb
	dq 0x00000000c934f160, 0x00000000690c5ec7
	dq 0x00000000a45b3497, 0x000000009a87fdcb
	dq 0x0000000064b09d7f, 0x000000008d175638
	dq 0x00000000c5c380d1, 0x00000000f42897c1
	dq 0x0000000049a3c91c, 0x00000000b8faa744
	dq 0x00000000be746056, 0x0000000027bb4f06
%endif
