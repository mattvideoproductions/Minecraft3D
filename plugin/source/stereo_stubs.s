@ ARM entry stubs for the v9.12 flat-parallax hooks (stereo_flat.cpp).
@ Both are reached from CTRPF WRAP_SUB wrappers (hooks.cpp SubWrapHook): the
@ wrapper restores r0-r3/lr to the game's values, then `blx`es the before
@ callback, the original sub, then the after callback, with sp untouched.

    .arm
    .fpu    vfp
    .section .text.mc3d_stubs, "ax", %progbits
    .align 2

@ After-callback of `bl CopyTexture2DResourceBlockToLinear` at 0x4F8918
@ (GxDevice::present, top RT -> left display buffer). At that point the game
@ keeps r4 = GxDevice* and r6 = display-buffer index (0..2) (0x4F88FC
@ `add sl, r4, r6, lsl #2`, 0x4F8A2C `str r6, [r4, #0x194]`), both callee-saved.
@ r0-r3/r12 are dead there: 0x4F891C..0x4F8944 reload all of them.
    .global mc3d_stub_after_top_b2l
    .type   mc3d_stub_after_top_b2l, %function
mc3d_stub_after_top_b2l:
    mov     r0, r6                      @ display index
    mov     r1, r4                      @ GxDevice*
    b       mc3d_after_top_b2l          @ C, returns to the wrapper through lr

@ Profiler tick stubs (v0.1.1). Used as WRAP_SUB before/after callbacks on
@ plain `bl` sites. They preserve r0-r3, r12, lr and d0-d7 (24 + 64 bytes,
@ sp stays 8-aligned), so the game sees exactly its own registers, including
@ the sub's return value in r0 for the after-callbacks.
    .macro TICKSTUB name, id
    .global \name
    .type   \name, %function
\name:
    push    {r0-r3, r12, lr}
    vpush   {d0-d7}
    mov     r0, #\id
    bl      mc3d_ptick
    vpop    {d0-d7}
    pop     {r0-r3, r12, lr}
    bx      lr
    .endm

    TICKSTUB mc3d_tick_flush_b, 0
    TICKSTUB mc3d_tick_flush_a, 1
    TICKSTUB mc3d_tick_wait_b, 2
    TICKSTUB mc3d_tick_wait_a, 3
    TICKSTUB mc3d_tick_run_b, 4
    TICKSTUB mc3d_tick_run_a, 5
    TICKSTUB mc3d_tick_job_b, 6
    TICKSTUB mc3d_tick_job_a, 7

@ v0.3: after-callback of `bl 0x221BB8` (level render) at 0x31DEF8 inside
@ the world render 0x31DD8C. Everything the game might still use is kept
@ (r0-r3, r12, lr, d0-d7, flags are redefined by `cmp sl,#0` at 0x31DEFC).
    .global mc3d_stub_after_level_render
    .type   mc3d_stub_after_level_render, %function
mc3d_stub_after_level_render:
    push    {r0-r3, r12, lr}
    vpush   {d0-d7}
    bl      mc3d_level_render_returned
    vpop    {d0-d7}
    pop     {r0-r3, r12, lr}
    bx      lr

@ v0.3: callback of the instruction hook at 0x3C5ACC in LevelRenderer::
@ startFrame. CTRPF has just executed the original `ldr r0, [r6, #0x20]`
@ (r0 = render radius the frame asks for, in blocks; r4 = LevelRenderer,
@ r6 = frame context). Returns r0 = min(r0, clamp) when a clamp is set;
@ r1-r3 are preserved. Flags, r12 and lr are dead at the site (the `bl
@ 0x3C5778` right before clobbers them; 0x3C5AD4 `cmp` redefines the flags)
@ and CTRPF restores lr itself. mc3d_rd = { clamp, lastCtx, lr, ctx, frames }.
    .global mc3d_rd_clamp_cb
    .type   mc3d_rd_clamp_cb, %function
mc3d_rd_clamp_cb:
    push    {r1, r2}
    ldr     r1, =mc3d_rd
    str     r0, [r1, #4]                @ lastCtx (unclamped)
    str     r4, [r1, #8]                @ LevelRenderer*
    str     r6, [r1, #12]               @ ctx*
    ldr     r2, [r1, #16]
    add     r2, r2, #1
    str     r2, [r1, #16]               @ frames seen
    ldr     r2, [r1]                    @ clamp (blocks), <= 0 = off
    cmp     r2, #0
    ble     1f
    cmp     r0, r2
    movgt   r0, r2
1:  pop     {r1, r2}
    bx      lr
    .ltorg

@ Before-callback of the top `bl nngxSwapBuffersByAddress` at 0x4F934C
@ (cmdlist-done callback 0x4F9308): r0 = 0x400, r1 = left address,
@ r2 = 0 (right address), r3 = 0xF0, [sp] = format. Keep r0, r3 and the
@ stack argument; r2 = mc3d_swap_eyes(left, &saved_r1), which may replace
@ the saved r1 (v0.2: left eye from the plugin's VRAM buffer).
    .global mc3d_stub_before_top_swap
    .type   mc3d_stub_before_top_swap, %function
mc3d_stub_before_top_swap:
    push    {r0, r1, r3, lr}            @ 16 bytes: keeps sp 8-byte aligned; r1 at [sp, #4]
    mov     r0, r1
    add     r1, sp, #4
    bl      mc3d_swap_eyes
    mov     r2, r0
    pop     {r0, r1, r3, lr}
    bx      lr
