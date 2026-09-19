; SPDX-License-Identifier: MIT
;
; Pass-through thunks for the NvTool* exports of nvEncodeAPI64.dll.
;
; The real driver DLL exports ten functions: the two NvEncodeAPI* entry points
; this shim actually cares about, plus eight NvTool* functions whose signatures
; are not public. Dropping them would change the DLL's export surface, so they
; are forwarded here in assembly: a register-exact tail jump works for any
; signature, which a C wrapper with a guessed prototype would not.
;
; A plain DEF-file export forwarder cannot be used - a forwarder names its
; target module by base name, and "nvEncodeAPI64" resolves straight back to this
; proxy.
;
; Each thunk jumps through g_vnf_tool_fns[idx]. That slot is NULL until the C
; side has loaded the real DLL, so the slow path saves the argument registers,
; calls vnf_resolve_tool(idx), restores them and tail-jumps to the result.

OPTION CASEMAP:NONE

EXTERN g_vnf_tool_fns:QWORD          ; void* g_vnf_tool_fns[8]
EXTERN vnf_resolve_tool:PROC         ; void* vnf_resolve_tool(unsigned idx)

.CODE

; Stack frame (136 bytes, keeps RSP 16-byte aligned at the CALL):
;   [rsp+00h..1Fh]  shadow space for vnf_resolve_tool
;   [rsp+20h..3Fh]  saved rcx, rdx, r8, r9
;   [rsp+40h..7Fh]  saved xmm0..xmm3
;   [rsp+80h..87h]  padding
TOOLTHUNK MACRO fname:REQ, idx:REQ
    LOCAL ready, failed
PUBLIC fname
fname PROC FRAME
    sub     rsp, 88h
    .ALLOCSTACK 88h
    .ENDPROLOG

    mov     rax, QWORD PTR [g_vnf_tool_fns + (idx) * 8]
    test    rax, rax
    jnz     ready                     ; fast path: arg registers untouched

    mov     QWORD PTR [rsp+20h], rcx
    mov     QWORD PTR [rsp+28h], rdx
    mov     QWORD PTR [rsp+30h], r8
    mov     QWORD PTR [rsp+38h], r9
    movdqa  XMMWORD PTR [rsp+40h], xmm0
    movdqa  XMMWORD PTR [rsp+50h], xmm1
    movdqa  XMMWORD PTR [rsp+60h], xmm2
    movdqa  XMMWORD PTR [rsp+70h], xmm3

    mov     ecx, idx
    call    vnf_resolve_tool

    movdqa  xmm3, XMMWORD PTR [rsp+70h]
    movdqa  xmm2, XMMWORD PTR [rsp+60h]
    movdqa  xmm1, XMMWORD PTR [rsp+50h]
    movdqa  xmm0, XMMWORD PTR [rsp+40h]
    mov     r9,  QWORD PTR [rsp+38h]
    mov     r8,  QWORD PTR [rsp+30h]
    mov     rdx, QWORD PTR [rsp+28h]
    mov     rcx, QWORD PTR [rsp+20h]

    test    rax, rax
    jz      failed

ready:
    add     rsp, 88h
    jmp     rax                       ; tail call: caller's stack args intact

failed:
    add     rsp, 88h
    xor     eax, eax                  ; real DLL unavailable
    ret
fname ENDP
ENDM

; Indices must match the enum in nvenc_shim.c.
    TOOLTHUNK NvToolCreateInterface,     0
    TOOLTHUNK NvToolDestroyInterface,    1
    TOOLTHUNK NvToolGetApiFunctionCount, 2
    TOOLTHUNK NvToolGetApiID,            3
    TOOLTHUNK NvToolGetApiNames,         4
    TOOLTHUNK NvToolGetInterface,        5
    TOOLTHUNK NvToolSetApiID,            6
    TOOLTHUNK NvToolSetInterface,        7

END
