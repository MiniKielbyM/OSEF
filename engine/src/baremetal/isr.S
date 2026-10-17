/* OSEF bare-metal: interrupt entry stubs for all 256 vectors.
 *
 * Every stub leaves the same frame on the stack (see InterruptFrame in
 * internal.h): CPU frame, error code (0 if the CPU pushed none), vector, then
 * rax..r15. The common path saves the SSE/x87 state with fxsave into a
 * 16-byte aligned area, calls osef_bm_isr_dispatch(InterruptFrame*), restores
 * everything and returns with iretq.
 */

    .text
    .code64

/* Vectors where the CPU pushes an error code: 8, 10-14, 17, 21, 29, 30. */
.macro ISR_NOERR vec
    .p2align 4
osef_bm_isr_\vec:
    pushq $0
    pushq $\vec
    jmp osef_bm_isr_common
.endm

.macro ISR_ERR vec
    .p2align 4
osef_bm_isr_\vec:
    pushq $\vec
    jmp osef_bm_isr_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47
/* 48-255: free for the OS (custom interrupts, software interrupts). */
ISR_NOERR 48
ISR_NOERR 49
ISR_NOERR 50
ISR_NOERR 51
ISR_NOERR 52
ISR_NOERR 53
ISR_NOERR 54
ISR_NOERR 55
ISR_NOERR 56
ISR_NOERR 57
ISR_NOERR 58
ISR_NOERR 59
ISR_NOERR 60
ISR_NOERR 61
ISR_NOERR 62
ISR_NOERR 63
ISR_NOERR 64
ISR_NOERR 65
ISR_NOERR 66
ISR_NOERR 67
ISR_NOERR 68
ISR_NOERR 69
ISR_NOERR 70
ISR_NOERR 71
ISR_NOERR 72
ISR_NOERR 73
ISR_NOERR 74
ISR_NOERR 75
ISR_NOERR 76
ISR_NOERR 77
ISR_NOERR 78
ISR_NOERR 79
ISR_NOERR 80
ISR_NOERR 81
ISR_NOERR 82
ISR_NOERR 83
ISR_NOERR 84
ISR_NOERR 85
ISR_NOERR 86
ISR_NOERR 87
ISR_NOERR 88
ISR_NOERR 89
ISR_NOERR 90
ISR_NOERR 91
ISR_NOERR 92
ISR_NOERR 93
ISR_NOERR 94
ISR_NOERR 95
ISR_NOERR 96
ISR_NOERR 97
ISR_NOERR 98
ISR_NOERR 99
ISR_NOERR 100
ISR_NOERR 101
ISR_NOERR 102
ISR_NOERR 103
ISR_NOERR 104
ISR_NOERR 105
ISR_NOERR 106
ISR_NOERR 107
ISR_NOERR 108
ISR_NOERR 109
ISR_NOERR 110
ISR_NOERR 111
ISR_NOERR 112
ISR_NOERR 113
ISR_NOERR 114
ISR_NOERR 115
ISR_NOERR 116
ISR_NOERR 117
ISR_NOERR 118
ISR_NOERR 119
ISR_NOERR 120
ISR_NOERR 121
ISR_NOERR 122
ISR_NOERR 123
ISR_NOERR 124
ISR_NOERR 125
ISR_NOERR 126
ISR_NOERR 127
ISR_NOERR 128
ISR_NOERR 129
ISR_NOERR 130
ISR_NOERR 131
ISR_NOERR 132
ISR_NOERR 133
ISR_NOERR 134
ISR_NOERR 135
ISR_NOERR 136
ISR_NOERR 137
ISR_NOERR 138
ISR_NOERR 139
ISR_NOERR 140
ISR_NOERR 141
ISR_NOERR 142
ISR_NOERR 143
ISR_NOERR 144
ISR_NOERR 145
ISR_NOERR 146
ISR_NOERR 147
ISR_NOERR 148
ISR_NOERR 149
ISR_NOERR 150
ISR_NOERR 151
ISR_NOERR 152
ISR_NOERR 153
ISR_NOERR 154
ISR_NOERR 155
ISR_NOERR 156
ISR_NOERR 157
ISR_NOERR 158
ISR_NOERR 159
ISR_NOERR 160
ISR_NOERR 161
ISR_NOERR 162
ISR_NOERR 163
ISR_NOERR 164
ISR_NOERR 165
ISR_NOERR 166
ISR_NOERR 167
ISR_NOERR 168
ISR_NOERR 169
ISR_NOERR 170
ISR_NOERR 171
ISR_NOERR 172
ISR_NOERR 173
ISR_NOERR 174
ISR_NOERR 175
ISR_NOERR 176
ISR_NOERR 177
ISR_NOERR 178
ISR_NOERR 179
ISR_NOERR 180
ISR_NOERR 181
ISR_NOERR 182
ISR_NOERR 183
ISR_NOERR 184
ISR_NOERR 185
ISR_NOERR 186
ISR_NOERR 187
ISR_NOERR 188
ISR_NOERR 189
ISR_NOERR 190
ISR_NOERR 191
ISR_NOERR 192
ISR_NOERR 193
ISR_NOERR 194
ISR_NOERR 195
ISR_NOERR 196
ISR_NOERR 197
ISR_NOERR 198
ISR_NOERR 199
ISR_NOERR 200
ISR_NOERR 201
ISR_NOERR 202
ISR_NOERR 203
ISR_NOERR 204
ISR_NOERR 205
ISR_NOERR 206
ISR_NOERR 207
ISR_NOERR 208
ISR_NOERR 209
ISR_NOERR 210
ISR_NOERR 211
ISR_NOERR 212
ISR_NOERR 213
ISR_NOERR 214
ISR_NOERR 215
ISR_NOERR 216
ISR_NOERR 217
ISR_NOERR 218
ISR_NOERR 219
ISR_NOERR 220
ISR_NOERR 221
ISR_NOERR 222
ISR_NOERR 223
ISR_NOERR 224
ISR_NOERR 225
ISR_NOERR 226
ISR_NOERR 227
ISR_NOERR 228
ISR_NOERR 229
ISR_NOERR 230
ISR_NOERR 231
ISR_NOERR 232
ISR_NOERR 233
ISR_NOERR 234
ISR_NOERR 235
ISR_NOERR 236
ISR_NOERR 237
ISR_NOERR 238
ISR_NOERR 239
ISR_NOERR 240
ISR_NOERR 241
ISR_NOERR 242
ISR_NOERR 243
ISR_NOERR 244
ISR_NOERR 245
ISR_NOERR 246
ISR_NOERR 247
ISR_NOERR 248
ISR_NOERR 249
ISR_NOERR 250
ISR_NOERR 251
ISR_NOERR 252
ISR_NOERR 253
ISR_NOERR 254
ISR_NOERR 255

    .p2align 4
osef_bm_isr_common:
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rbx            /* rbx = InterruptFrame* (callee-saved) */
    subq $512, %rsp
    andq $-16, %rsp            /* 16-byte aligned fxsave area; also ABI call alignment */
    fxsave (%rsp)
    movq %rbx, %rdi
    call osef_bm_isr_dispatch
    fxrstor (%rsp)
    movq %rbx, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp             /* vector + error code */
    iretq

/* Vectors 48-255 are never programmed (no APIC, no software interrupts), but
 * a stray one -- e.g. a local-APIC spurious interrupt, whose vector the
 * firmware chose (often 0xFF) -- must not hit a not-present gate (#GP ->
 * panic). Spurious interrupts need no EOI, so simply return. */
    .p2align 4
    .globl osef_bm_isr_ignore
osef_bm_isr_ignore:
    iretq

/* Table of stub addresses, indexed by vector (used by idt.cpp). */
    .section .rodata
    .p2align 3
    .globl osef_bm_isr_stubs
osef_bm_isr_stubs:
    .quad osef_bm_isr_0
    .quad osef_bm_isr_1
    .quad osef_bm_isr_2
    .quad osef_bm_isr_3
    .quad osef_bm_isr_4
    .quad osef_bm_isr_5
    .quad osef_bm_isr_6
    .quad osef_bm_isr_7
    .quad osef_bm_isr_8
    .quad osef_bm_isr_9
    .quad osef_bm_isr_10
    .quad osef_bm_isr_11
    .quad osef_bm_isr_12
    .quad osef_bm_isr_13
    .quad osef_bm_isr_14
    .quad osef_bm_isr_15
    .quad osef_bm_isr_16
    .quad osef_bm_isr_17
    .quad osef_bm_isr_18
    .quad osef_bm_isr_19
    .quad osef_bm_isr_20
    .quad osef_bm_isr_21
    .quad osef_bm_isr_22
    .quad osef_bm_isr_23
    .quad osef_bm_isr_24
    .quad osef_bm_isr_25
    .quad osef_bm_isr_26
    .quad osef_bm_isr_27
    .quad osef_bm_isr_28
    .quad osef_bm_isr_29
    .quad osef_bm_isr_30
    .quad osef_bm_isr_31
    .quad osef_bm_isr_32
    .quad osef_bm_isr_33
    .quad osef_bm_isr_34
    .quad osef_bm_isr_35
    .quad osef_bm_isr_36
    .quad osef_bm_isr_37
    .quad osef_bm_isr_38
    .quad osef_bm_isr_39
    .quad osef_bm_isr_40
    .quad osef_bm_isr_41
    .quad osef_bm_isr_42
    .quad osef_bm_isr_43
    .quad osef_bm_isr_44
    .quad osef_bm_isr_45
    .quad osef_bm_isr_46
    .quad osef_bm_isr_47
    .quad osef_bm_isr_48
    .quad osef_bm_isr_49
    .quad osef_bm_isr_50
    .quad osef_bm_isr_51
    .quad osef_bm_isr_52
    .quad osef_bm_isr_53
    .quad osef_bm_isr_54
    .quad osef_bm_isr_55
    .quad osef_bm_isr_56
    .quad osef_bm_isr_57
    .quad osef_bm_isr_58
    .quad osef_bm_isr_59
    .quad osef_bm_isr_60
    .quad osef_bm_isr_61
    .quad osef_bm_isr_62
    .quad osef_bm_isr_63
    .quad osef_bm_isr_64
    .quad osef_bm_isr_65
    .quad osef_bm_isr_66
    .quad osef_bm_isr_67
    .quad osef_bm_isr_68
    .quad osef_bm_isr_69
    .quad osef_bm_isr_70
    .quad osef_bm_isr_71
    .quad osef_bm_isr_72
    .quad osef_bm_isr_73
    .quad osef_bm_isr_74
    .quad osef_bm_isr_75
    .quad osef_bm_isr_76
    .quad osef_bm_isr_77
    .quad osef_bm_isr_78
    .quad osef_bm_isr_79
    .quad osef_bm_isr_80
    .quad osef_bm_isr_81
    .quad osef_bm_isr_82
    .quad osef_bm_isr_83
    .quad osef_bm_isr_84
    .quad osef_bm_isr_85
    .quad osef_bm_isr_86
    .quad osef_bm_isr_87
    .quad osef_bm_isr_88
    .quad osef_bm_isr_89
    .quad osef_bm_isr_90
    .quad osef_bm_isr_91
    .quad osef_bm_isr_92
    .quad osef_bm_isr_93
    .quad osef_bm_isr_94
    .quad osef_bm_isr_95
    .quad osef_bm_isr_96
    .quad osef_bm_isr_97
    .quad osef_bm_isr_98
    .quad osef_bm_isr_99
    .quad osef_bm_isr_100
    .quad osef_bm_isr_101
    .quad osef_bm_isr_102
    .quad osef_bm_isr_103
    .quad osef_bm_isr_104
    .quad osef_bm_isr_105
    .quad osef_bm_isr_106
    .quad osef_bm_isr_107
    .quad osef_bm_isr_108
    .quad osef_bm_isr_109
    .quad osef_bm_isr_110
    .quad osef_bm_isr_111
    .quad osef_bm_isr_112
    .quad osef_bm_isr_113
    .quad osef_bm_isr_114
    .quad osef_bm_isr_115
    .quad osef_bm_isr_116
    .quad osef_bm_isr_117
    .quad osef_bm_isr_118
    .quad osef_bm_isr_119
    .quad osef_bm_isr_120
    .quad osef_bm_isr_121
    .quad osef_bm_isr_122
    .quad osef_bm_isr_123
    .quad osef_bm_isr_124
    .quad osef_bm_isr_125
    .quad osef_bm_isr_126
    .quad osef_bm_isr_127
    .quad osef_bm_isr_128
    .quad osef_bm_isr_129
    .quad osef_bm_isr_130
    .quad osef_bm_isr_131
    .quad osef_bm_isr_132
    .quad osef_bm_isr_133
    .quad osef_bm_isr_134
    .quad osef_bm_isr_135
    .quad osef_bm_isr_136
    .quad osef_bm_isr_137
    .quad osef_bm_isr_138
    .quad osef_bm_isr_139
    .quad osef_bm_isr_140
    .quad osef_bm_isr_141
    .quad osef_bm_isr_142
    .quad osef_bm_isr_143
    .quad osef_bm_isr_144
    .quad osef_bm_isr_145
    .quad osef_bm_isr_146
    .quad osef_bm_isr_147
    .quad osef_bm_isr_148
    .quad osef_bm_isr_149
    .quad osef_bm_isr_150
    .quad osef_bm_isr_151
    .quad osef_bm_isr_152
    .quad osef_bm_isr_153
    .quad osef_bm_isr_154
    .quad osef_bm_isr_155
    .quad osef_bm_isr_156
    .quad osef_bm_isr_157
    .quad osef_bm_isr_158
    .quad osef_bm_isr_159
    .quad osef_bm_isr_160
    .quad osef_bm_isr_161
    .quad osef_bm_isr_162
    .quad osef_bm_isr_163
    .quad osef_bm_isr_164
    .quad osef_bm_isr_165
    .quad osef_bm_isr_166
    .quad osef_bm_isr_167
    .quad osef_bm_isr_168
    .quad osef_bm_isr_169
    .quad osef_bm_isr_170
    .quad osef_bm_isr_171
    .quad osef_bm_isr_172
    .quad osef_bm_isr_173
    .quad osef_bm_isr_174
    .quad osef_bm_isr_175
    .quad osef_bm_isr_176
    .quad osef_bm_isr_177
    .quad osef_bm_isr_178
    .quad osef_bm_isr_179
    .quad osef_bm_isr_180
    .quad osef_bm_isr_181
    .quad osef_bm_isr_182
    .quad osef_bm_isr_183
    .quad osef_bm_isr_184
    .quad osef_bm_isr_185
    .quad osef_bm_isr_186
    .quad osef_bm_isr_187
    .quad osef_bm_isr_188
    .quad osef_bm_isr_189
    .quad osef_bm_isr_190
    .quad osef_bm_isr_191
    .quad osef_bm_isr_192
    .quad osef_bm_isr_193
    .quad osef_bm_isr_194
    .quad osef_bm_isr_195
    .quad osef_bm_isr_196
    .quad osef_bm_isr_197
    .quad osef_bm_isr_198
    .quad osef_bm_isr_199
    .quad osef_bm_isr_200
    .quad osef_bm_isr_201
    .quad osef_bm_isr_202
    .quad osef_bm_isr_203
    .quad osef_bm_isr_204
    .quad osef_bm_isr_205
    .quad osef_bm_isr_206
    .quad osef_bm_isr_207
    .quad osef_bm_isr_208
    .quad osef_bm_isr_209
    .quad osef_bm_isr_210
    .quad osef_bm_isr_211
    .quad osef_bm_isr_212
    .quad osef_bm_isr_213
    .quad osef_bm_isr_214
    .quad osef_bm_isr_215
    .quad osef_bm_isr_216
    .quad osef_bm_isr_217
    .quad osef_bm_isr_218
    .quad osef_bm_isr_219
    .quad osef_bm_isr_220
    .quad osef_bm_isr_221
    .quad osef_bm_isr_222
    .quad osef_bm_isr_223
    .quad osef_bm_isr_224
    .quad osef_bm_isr_225
    .quad osef_bm_isr_226
    .quad osef_bm_isr_227
    .quad osef_bm_isr_228
    .quad osef_bm_isr_229
    .quad osef_bm_isr_230
    .quad osef_bm_isr_231
    .quad osef_bm_isr_232
    .quad osef_bm_isr_233
    .quad osef_bm_isr_234
    .quad osef_bm_isr_235
    .quad osef_bm_isr_236
    .quad osef_bm_isr_237
    .quad osef_bm_isr_238
    .quad osef_bm_isr_239
    .quad osef_bm_isr_240
    .quad osef_bm_isr_241
    .quad osef_bm_isr_242
    .quad osef_bm_isr_243
    .quad osef_bm_isr_244
    .quad osef_bm_isr_245
    .quad osef_bm_isr_246
    .quad osef_bm_isr_247
    .quad osef_bm_isr_248
    .quad osef_bm_isr_249
    .quad osef_bm_isr_250
    .quad osef_bm_isr_251
    .quad osef_bm_isr_252
    .quad osef_bm_isr_253
    .quad osef_bm_isr_254
    .quad osef_bm_isr_255

/* Software-interrupt thunks: osef_bm_int_thunks[n] executes `int $n` and
 * returns (the vector of `int` is an immediate, hence one thunk per vector). */
    .text
    .p2align 3
osef_bm_int_48:
    int $48
    ret
    .p2align 3
osef_bm_int_49:
    int $49
    ret
    .p2align 3
osef_bm_int_50:
    int $50
    ret
    .p2align 3
osef_bm_int_51:
    int $51
    ret
    .p2align 3
osef_bm_int_52:
    int $52
    ret
    .p2align 3
osef_bm_int_53:
    int $53
    ret
    .p2align 3
osef_bm_int_54:
    int $54
    ret
    .p2align 3
osef_bm_int_55:
    int $55
    ret
    .p2align 3
osef_bm_int_56:
    int $56
    ret
    .p2align 3
osef_bm_int_57:
    int $57
    ret
    .p2align 3
osef_bm_int_58:
    int $58
    ret
    .p2align 3
osef_bm_int_59:
    int $59
    ret
    .p2align 3
osef_bm_int_60:
    int $60
    ret
    .p2align 3
osef_bm_int_61:
    int $61
    ret
    .p2align 3
osef_bm_int_62:
    int $62
    ret
    .p2align 3
osef_bm_int_63:
    int $63
    ret
    .p2align 3
osef_bm_int_64:
    int $64
    ret
    .p2align 3
osef_bm_int_65:
    int $65
    ret
    .p2align 3
osef_bm_int_66:
    int $66
    ret
    .p2align 3
osef_bm_int_67:
    int $67
    ret
    .p2align 3
osef_bm_int_68:
    int $68
    ret
    .p2align 3
osef_bm_int_69:
    int $69
    ret
    .p2align 3
osef_bm_int_70:
    int $70
    ret
    .p2align 3
osef_bm_int_71:
    int $71
    ret
    .p2align 3
osef_bm_int_72:
    int $72
    ret
    .p2align 3
osef_bm_int_73:
    int $73
    ret
    .p2align 3
osef_bm_int_74:
    int $74
    ret
    .p2align 3
osef_bm_int_75:
    int $75
    ret
    .p2align 3
osef_bm_int_76:
    int $76
    ret
    .p2align 3
osef_bm_int_77:
    int $77
    ret
    .p2align 3
osef_bm_int_78:
    int $78
    ret
    .p2align 3
osef_bm_int_79:
    int $79
    ret
    .p2align 3
osef_bm_int_80:
    int $80
    ret
    .p2align 3
osef_bm_int_81:
    int $81
    ret
    .p2align 3
osef_bm_int_82:
    int $82
    ret
    .p2align 3
osef_bm_int_83:
    int $83
    ret
    .p2align 3
osef_bm_int_84:
    int $84
    ret
    .p2align 3
osef_bm_int_85:
    int $85
    ret
    .p2align 3
osef_bm_int_86:
    int $86
    ret
    .p2align 3
osef_bm_int_87:
    int $87
    ret
    .p2align 3
osef_bm_int_88:
    int $88
    ret
    .p2align 3
osef_bm_int_89:
    int $89
    ret
    .p2align 3
osef_bm_int_90:
    int $90
    ret
    .p2align 3
osef_bm_int_91:
    int $91
    ret
    .p2align 3
osef_bm_int_92:
    int $92
    ret
    .p2align 3
osef_bm_int_93:
    int $93
    ret
    .p2align 3
osef_bm_int_94:
    int $94
    ret
    .p2align 3
osef_bm_int_95:
    int $95
    ret
    .p2align 3
osef_bm_int_96:
    int $96
    ret
    .p2align 3
osef_bm_int_97:
    int $97
    ret
    .p2align 3
osef_bm_int_98:
    int $98
    ret
    .p2align 3
osef_bm_int_99:
    int $99
    ret
    .p2align 3
osef_bm_int_100:
    int $100
    ret
    .p2align 3
osef_bm_int_101:
    int $101
    ret
    .p2align 3
osef_bm_int_102:
    int $102
    ret
    .p2align 3
osef_bm_int_103:
    int $103
    ret
    .p2align 3
osef_bm_int_104:
    int $104
    ret
    .p2align 3
osef_bm_int_105:
    int $105
    ret
    .p2align 3
osef_bm_int_106:
    int $106
    ret
    .p2align 3
osef_bm_int_107:
    int $107
    ret
    .p2align 3
osef_bm_int_108:
    int $108
    ret
    .p2align 3
osef_bm_int_109:
    int $109
    ret
    .p2align 3
osef_bm_int_110:
    int $110
    ret
    .p2align 3
osef_bm_int_111:
    int $111
    ret
    .p2align 3
osef_bm_int_112:
    int $112
    ret
    .p2align 3
osef_bm_int_113:
    int $113
    ret
    .p2align 3
osef_bm_int_114:
    int $114
    ret
    .p2align 3
osef_bm_int_115:
    int $115
    ret
    .p2align 3
osef_bm_int_116:
    int $116
    ret
    .p2align 3
osef_bm_int_117:
    int $117
    ret
    .p2align 3
osef_bm_int_118:
    int $118
    ret
    .p2align 3
osef_bm_int_119:
    int $119
    ret
    .p2align 3
osef_bm_int_120:
    int $120
    ret
    .p2align 3
osef_bm_int_121:
    int $121
    ret
    .p2align 3
osef_bm_int_122:
    int $122
    ret
    .p2align 3
osef_bm_int_123:
    int $123
    ret
    .p2align 3
osef_bm_int_124:
    int $124
    ret
    .p2align 3
osef_bm_int_125:
    int $125
    ret
    .p2align 3
osef_bm_int_126:
    int $126
    ret
    .p2align 3
osef_bm_int_127:
    int $127
    ret
    .p2align 3
osef_bm_int_128:
    int $128
    ret
    .p2align 3
osef_bm_int_129:
    int $129
    ret
    .p2align 3
osef_bm_int_130:
    int $130
    ret
    .p2align 3
osef_bm_int_131:
    int $131
    ret
    .p2align 3
osef_bm_int_132:
    int $132
    ret
    .p2align 3
osef_bm_int_133:
    int $133
    ret
    .p2align 3
osef_bm_int_134:
    int $134
    ret
    .p2align 3
osef_bm_int_135:
    int $135
    ret
    .p2align 3
osef_bm_int_136:
    int $136
    ret
    .p2align 3
osef_bm_int_137:
    int $137
    ret
    .p2align 3
osef_bm_int_138:
    int $138
    ret
    .p2align 3
osef_bm_int_139:
    int $139
    ret
    .p2align 3
osef_bm_int_140:
    int $140
    ret
    .p2align 3
osef_bm_int_141:
    int $141
    ret
    .p2align 3
osef_bm_int_142:
    int $142
    ret
    .p2align 3
osef_bm_int_143:
    int $143
    ret
    .p2align 3
osef_bm_int_144:
    int $144
    ret
    .p2align 3
osef_bm_int_145:
    int $145
    ret
    .p2align 3
osef_bm_int_146:
    int $146
    ret
    .p2align 3
osef_bm_int_147:
    int $147
    ret
    .p2align 3
osef_bm_int_148:
    int $148
    ret
    .p2align 3
osef_bm_int_149:
    int $149
    ret
    .p2align 3
osef_bm_int_150:
    int $150
    ret
    .p2align 3
osef_bm_int_151:
    int $151
    ret
    .p2align 3
osef_bm_int_152:
    int $152
    ret
    .p2align 3
osef_bm_int_153:
    int $153
    ret
    .p2align 3
osef_bm_int_154:
    int $154
    ret
    .p2align 3
osef_bm_int_155:
    int $155
    ret
    .p2align 3
osef_bm_int_156:
    int $156
    ret
    .p2align 3
osef_bm_int_157:
    int $157
    ret
    .p2align 3
osef_bm_int_158:
    int $158
    ret
    .p2align 3
osef_bm_int_159:
    int $159
    ret
    .p2align 3
osef_bm_int_160:
    int $160
    ret
    .p2align 3
osef_bm_int_161:
    int $161
    ret
    .p2align 3
osef_bm_int_162:
    int $162
    ret
    .p2align 3
osef_bm_int_163:
    int $163
    ret
    .p2align 3
osef_bm_int_164:
    int $164
    ret
    .p2align 3
osef_bm_int_165:
    int $165
    ret
    .p2align 3
osef_bm_int_166:
    int $166
    ret
    .p2align 3
osef_bm_int_167:
    int $167
    ret
    .p2align 3
osef_bm_int_168:
    int $168
    ret
    .p2align 3
osef_bm_int_169:
    int $169
    ret
    .p2align 3
osef_bm_int_170:
    int $170
    ret
    .p2align 3
osef_bm_int_171:
    int $171
    ret
    .p2align 3
osef_bm_int_172:
    int $172
    ret
    .p2align 3
osef_bm_int_173:
    int $173
    ret
    .p2align 3
osef_bm_int_174:
    int $174
    ret
    .p2align 3
osef_bm_int_175:
    int $175
    ret
    .p2align 3
osef_bm_int_176:
    int $176
    ret
    .p2align 3
osef_bm_int_177:
    int $177
    ret
    .p2align 3
osef_bm_int_178:
    int $178
    ret
    .p2align 3
osef_bm_int_179:
    int $179
    ret
    .p2align 3
osef_bm_int_180:
    int $180
    ret
    .p2align 3
osef_bm_int_181:
    int $181
    ret
    .p2align 3
osef_bm_int_182:
    int $182
    ret
    .p2align 3
osef_bm_int_183:
    int $183
    ret
    .p2align 3
osef_bm_int_184:
    int $184
    ret
    .p2align 3
osef_bm_int_185:
    int $185
    ret
    .p2align 3
osef_bm_int_186:
    int $186
    ret
    .p2align 3
osef_bm_int_187:
    int $187
    ret
    .p2align 3
osef_bm_int_188:
    int $188
    ret
    .p2align 3
osef_bm_int_189:
    int $189
    ret
    .p2align 3
osef_bm_int_190:
    int $190
    ret
    .p2align 3
osef_bm_int_191:
    int $191
    ret
    .p2align 3
osef_bm_int_192:
    int $192
    ret
    .p2align 3
osef_bm_int_193:
    int $193
    ret
    .p2align 3
osef_bm_int_194:
    int $194
    ret
    .p2align 3
osef_bm_int_195:
    int $195
    ret
    .p2align 3
osef_bm_int_196:
    int $196
    ret
    .p2align 3
osef_bm_int_197:
    int $197
    ret
    .p2align 3
osef_bm_int_198:
    int $198
    ret
    .p2align 3
osef_bm_int_199:
    int $199
    ret
    .p2align 3
osef_bm_int_200:
    int $200
    ret
    .p2align 3
osef_bm_int_201:
    int $201
    ret
    .p2align 3
osef_bm_int_202:
    int $202
    ret
    .p2align 3
osef_bm_int_203:
    int $203
    ret
    .p2align 3
osef_bm_int_204:
    int $204
    ret
    .p2align 3
osef_bm_int_205:
    int $205
    ret
    .p2align 3
osef_bm_int_206:
    int $206
    ret
    .p2align 3
osef_bm_int_207:
    int $207
    ret
    .p2align 3
osef_bm_int_208:
    int $208
    ret
    .p2align 3
osef_bm_int_209:
    int $209
    ret
    .p2align 3
osef_bm_int_210:
    int $210
    ret
    .p2align 3
osef_bm_int_211:
    int $211
    ret
    .p2align 3
osef_bm_int_212:
    int $212
    ret
    .p2align 3
osef_bm_int_213:
    int $213
    ret
    .p2align 3
osef_bm_int_214:
    int $214
    ret
    .p2align 3
osef_bm_int_215:
    int $215
    ret
    .p2align 3
osef_bm_int_216:
    int $216
    ret
    .p2align 3
osef_bm_int_217:
    int $217
    ret
    .p2align 3
osef_bm_int_218:
    int $218
    ret
    .p2align 3
osef_bm_int_219:
    int $219
    ret
    .p2align 3
osef_bm_int_220:
    int $220
    ret
    .p2align 3
osef_bm_int_221:
    int $221
    ret
    .p2align 3
osef_bm_int_222:
    int $222
    ret
    .p2align 3
osef_bm_int_223:
    int $223
    ret
    .p2align 3
osef_bm_int_224:
    int $224
    ret
    .p2align 3
osef_bm_int_225:
    int $225
    ret
    .p2align 3
osef_bm_int_226:
    int $226
    ret
    .p2align 3
osef_bm_int_227:
    int $227
    ret
    .p2align 3
osef_bm_int_228:
    int $228
    ret
    .p2align 3
osef_bm_int_229:
    int $229
    ret
    .p2align 3
osef_bm_int_230:
    int $230
    ret
    .p2align 3
osef_bm_int_231:
    int $231
    ret
    .p2align 3
osef_bm_int_232:
    int $232
    ret
    .p2align 3
osef_bm_int_233:
    int $233
    ret
    .p2align 3
osef_bm_int_234:
    int $234
    ret
    .p2align 3
osef_bm_int_235:
    int $235
    ret
    .p2align 3
osef_bm_int_236:
    int $236
    ret
    .p2align 3
osef_bm_int_237:
    int $237
    ret
    .p2align 3
osef_bm_int_238:
    int $238
    ret
    .p2align 3
osef_bm_int_239:
    int $239
    ret
    .p2align 3
osef_bm_int_240:
    int $240
    ret
    .p2align 3
osef_bm_int_241:
    int $241
    ret
    .p2align 3
osef_bm_int_242:
    int $242
    ret
    .p2align 3
osef_bm_int_243:
    int $243
    ret
    .p2align 3
osef_bm_int_244:
    int $244
    ret
    .p2align 3
osef_bm_int_245:
    int $245
    ret
    .p2align 3
osef_bm_int_246:
    int $246
    ret
    .p2align 3
osef_bm_int_247:
    int $247
    ret
    .p2align 3
osef_bm_int_248:
    int $248
    ret
    .p2align 3
osef_bm_int_249:
    int $249
    ret
    .p2align 3
osef_bm_int_250:
    int $250
    ret
    .p2align 3
osef_bm_int_251:
    int $251
    ret
    .p2align 3
osef_bm_int_252:
    int $252
    ret
    .p2align 3
osef_bm_int_253:
    int $253
    ret
    .p2align 3
osef_bm_int_254:
    int $254
    ret
    .p2align 3
osef_bm_int_255:
    int $255
    ret

    .section .rodata
    .p2align 3
    .globl osef_bm_int_thunks
osef_bm_int_thunks:
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad osef_bm_int_48
    .quad osef_bm_int_49
    .quad osef_bm_int_50
    .quad osef_bm_int_51
    .quad osef_bm_int_52
    .quad osef_bm_int_53
    .quad osef_bm_int_54
    .quad osef_bm_int_55
    .quad osef_bm_int_56
    .quad osef_bm_int_57
    .quad osef_bm_int_58
    .quad osef_bm_int_59
    .quad osef_bm_int_60
    .quad osef_bm_int_61
    .quad osef_bm_int_62
    .quad osef_bm_int_63
    .quad osef_bm_int_64
    .quad osef_bm_int_65
    .quad osef_bm_int_66
    .quad osef_bm_int_67
    .quad osef_bm_int_68
    .quad osef_bm_int_69
    .quad osef_bm_int_70
    .quad osef_bm_int_71
    .quad osef_bm_int_72
    .quad osef_bm_int_73
    .quad osef_bm_int_74
    .quad osef_bm_int_75
    .quad osef_bm_int_76
    .quad osef_bm_int_77
    .quad osef_bm_int_78
    .quad osef_bm_int_79
    .quad osef_bm_int_80
    .quad osef_bm_int_81
    .quad osef_bm_int_82
    .quad osef_bm_int_83
    .quad osef_bm_int_84
    .quad osef_bm_int_85
    .quad osef_bm_int_86
    .quad osef_bm_int_87
    .quad osef_bm_int_88
    .quad osef_bm_int_89
    .quad osef_bm_int_90
    .quad osef_bm_int_91
    .quad osef_bm_int_92
    .quad osef_bm_int_93
    .quad osef_bm_int_94
    .quad osef_bm_int_95
    .quad osef_bm_int_96
    .quad osef_bm_int_97
    .quad osef_bm_int_98
    .quad osef_bm_int_99
    .quad osef_bm_int_100
    .quad osef_bm_int_101
    .quad osef_bm_int_102
    .quad osef_bm_int_103
    .quad osef_bm_int_104
    .quad osef_bm_int_105
    .quad osef_bm_int_106
    .quad osef_bm_int_107
    .quad osef_bm_int_108
    .quad osef_bm_int_109
    .quad osef_bm_int_110
    .quad osef_bm_int_111
    .quad osef_bm_int_112
    .quad osef_bm_int_113
    .quad osef_bm_int_114
    .quad osef_bm_int_115
    .quad osef_bm_int_116
    .quad osef_bm_int_117
    .quad osef_bm_int_118
    .quad osef_bm_int_119
    .quad osef_bm_int_120
    .quad osef_bm_int_121
    .quad osef_bm_int_122
    .quad osef_bm_int_123
    .quad osef_bm_int_124
    .quad osef_bm_int_125
    .quad osef_bm_int_126
    .quad osef_bm_int_127
    .quad osef_bm_int_128
    .quad osef_bm_int_129
    .quad osef_bm_int_130
    .quad osef_bm_int_131
    .quad osef_bm_int_132
    .quad osef_bm_int_133
    .quad osef_bm_int_134
    .quad osef_bm_int_135
    .quad osef_bm_int_136
    .quad osef_bm_int_137
    .quad osef_bm_int_138
    .quad osef_bm_int_139
    .quad osef_bm_int_140
    .quad osef_bm_int_141
    .quad osef_bm_int_142
    .quad osef_bm_int_143
    .quad osef_bm_int_144
    .quad osef_bm_int_145
    .quad osef_bm_int_146
    .quad osef_bm_int_147
    .quad osef_bm_int_148
    .quad osef_bm_int_149
    .quad osef_bm_int_150
    .quad osef_bm_int_151
    .quad osef_bm_int_152
    .quad osef_bm_int_153
    .quad osef_bm_int_154
    .quad osef_bm_int_155
    .quad osef_bm_int_156
    .quad osef_bm_int_157
    .quad osef_bm_int_158
    .quad osef_bm_int_159
    .quad osef_bm_int_160
    .quad osef_bm_int_161
    .quad osef_bm_int_162
    .quad osef_bm_int_163
    .quad osef_bm_int_164
    .quad osef_bm_int_165
    .quad osef_bm_int_166
    .quad osef_bm_int_167
    .quad osef_bm_int_168
    .quad osef_bm_int_169
    .quad osef_bm_int_170
    .quad osef_bm_int_171
    .quad osef_bm_int_172
    .quad osef_bm_int_173
    .quad osef_bm_int_174
    .quad osef_bm_int_175
    .quad osef_bm_int_176
    .quad osef_bm_int_177
    .quad osef_bm_int_178
    .quad osef_bm_int_179
    .quad osef_bm_int_180
    .quad osef_bm_int_181
    .quad osef_bm_int_182
    .quad osef_bm_int_183
    .quad osef_bm_int_184
    .quad osef_bm_int_185
    .quad osef_bm_int_186
    .quad osef_bm_int_187
    .quad osef_bm_int_188
    .quad osef_bm_int_189
    .quad osef_bm_int_190
    .quad osef_bm_int_191
    .quad osef_bm_int_192
    .quad osef_bm_int_193
    .quad osef_bm_int_194
    .quad osef_bm_int_195
    .quad osef_bm_int_196
    .quad osef_bm_int_197
    .quad osef_bm_int_198
    .quad osef_bm_int_199
    .quad osef_bm_int_200
    .quad osef_bm_int_201
    .quad osef_bm_int_202
    .quad osef_bm_int_203
    .quad osef_bm_int_204
    .quad osef_bm_int_205
    .quad osef_bm_int_206
    .quad osef_bm_int_207
    .quad osef_bm_int_208
    .quad osef_bm_int_209
    .quad osef_bm_int_210
    .quad osef_bm_int_211
    .quad osef_bm_int_212
    .quad osef_bm_int_213
    .quad osef_bm_int_214
    .quad osef_bm_int_215
    .quad osef_bm_int_216
    .quad osef_bm_int_217
    .quad osef_bm_int_218
    .quad osef_bm_int_219
    .quad osef_bm_int_220
    .quad osef_bm_int_221
    .quad osef_bm_int_222
    .quad osef_bm_int_223
    .quad osef_bm_int_224
    .quad osef_bm_int_225
    .quad osef_bm_int_226
    .quad osef_bm_int_227
    .quad osef_bm_int_228
    .quad osef_bm_int_229
    .quad osef_bm_int_230
    .quad osef_bm_int_231
    .quad osef_bm_int_232
    .quad osef_bm_int_233
    .quad osef_bm_int_234
    .quad osef_bm_int_235
    .quad osef_bm_int_236
    .quad osef_bm_int_237
    .quad osef_bm_int_238
    .quad osef_bm_int_239
    .quad osef_bm_int_240
    .quad osef_bm_int_241
    .quad osef_bm_int_242
    .quad osef_bm_int_243
    .quad osef_bm_int_244
    .quad osef_bm_int_245
    .quad osef_bm_int_246
    .quad osef_bm_int_247
    .quad osef_bm_int_248
    .quad osef_bm_int_249
    .quad osef_bm_int_250
    .quad osef_bm_int_251
    .quad osef_bm_int_252
    .quad osef_bm_int_253
    .quad osef_bm_int_254
    .quad osef_bm_int_255

    .section .note.GNU-stack,"",@progbits
