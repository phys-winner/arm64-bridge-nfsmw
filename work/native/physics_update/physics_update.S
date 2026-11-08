@ Stage 8 frame-rate work (ST-3): replacement body of PhysicsSubSystem::Update, libapp 0x3AC1B8
@ ("PhysicsUpdate", sub_3AC1B8), assembled at that address by work/tools/build_physics_update.py.
@
@ Original: stepSimulation(scale*ms, maxSubSteps 1, fixedTimeStep scale/30) with the time step truncated to
@ whole ms, i.e. the scene changes 30 times per second at any display rate, the game runs at 0.88 instead of
@ GameTimeScale 0.92 at 60/120 Hz and slow motion (crash slowdown 0.25, race-end cutscenes) nearly stops.
@ This body takes the engine's own variable-step path (PhysicsSubSystem+0x45 / quality bit3, never set on
@ Android): one Bullet step per rendered frame, frames over 40 ms fall back to one fixed 0.04 s step. The step
@ is a whole number of ms, as the physics tick listeners get it ((int)(dt*1000+0.5)), and the fraction is
@ carried in the object's padding halfword +0x46 (1/65536 ms), so physics, race logic and timers advance
@ exactly scale * elapsed time. A frame with no whole ms yet (deep slow motion) makes no Bullet step: a
@ zero-length step would divide by the time step in the contact solver.
@ The profiling scope of the original ("PhysicsUpdate" marker) is not kept.
@
@ Stage 14: +0x70 holds the real time this frame's step stands for. Engine code advances its own timers by +0x70
@ once per physics tick or per frame: the crash cutscene ("Knockdown"/takedown camera: Duration and the slowdown
@ curve, CrashCutscene 0x13E118, its crash impulse 0x13E2E0), the crash camera shake (0x96028, 0x97914) and the
@ race layer letterbox tween (0x1FC8A4). In the original a tick was 1/30 s of real time at any time scale (one
@ fixed step of scale/30 per 1/30 s), so the constant 1/30 there meant real time; with one step per frame the
@ constant made those scenes 2x (60 fps) to 4x (120 fps) too fast. The value is the step's whole scaled ms
@ divided by the time scale, so with the carried fraction it sums to the elapsed real time; 0 for a frame
@ without a step, the frame's own ms while paused or with the time scale at 0, at most 0.1 s (loading stalls).
@
@ r0 = PhysicsSubSystem*: +0x04 physics world wrapper, +0x44 paused, +0x46 carry (new), +0x6C time scale,
@ +0x70 real seconds of this step (was the constant fixed step 1/30 s). r1 = const Timestep* (int ms).
@ sub_6866C0(world, const int* ms, maxSubSteps, fixedTimeStep): unchanged callee, converts the ms to seconds and
@ calls btDynamicsWorld::stepSimulation (fixedTimeStep is unused with maxSubSteps 0).
    .syntax unified
    .arm
    .fpu vfpv3-d16
    .text
    .global physics_update
    .type physics_update, %function
physics_update:
    push    {r4, r5, r11, lr}
    add     r11, sp, #8
    sub     sp, sp, #8
    mov     r4, r0
    mov     r5, #0
    vldr    s0, [r1]
    vcvt.f32.s32 s0, s0             @ frame ms
    vmov.f32 s3, s0                 @ paused: the frame's own ms
    ldrb    r2, [r4, #0x44]
    cmp     r2, #0
    bne     .Lshare
    vldr    s1, [r4, #0x6C]
    vmul.f32 s2, s0, s1             @ scaled ms
    ldrh    r2, [r4, #0x46]
    vmov    s3, r2
    vcvt.f32.u32 s3, s3
    vldr    s4, .Lk_inv65536
    vmla.f32 s2, s3, s4             @ + carried fraction
    vcvt.u32.f32 s3, s2             @ whole ms (round toward zero, value >= 0)
    vmov    r5, s3
    vcvt.f32.u32 s3, s3
    vsub.f32 s2, s2, s3             @ new fraction [0, 1)
    vldr    s4, .Lk_65536
    vmul.f32 s2, s2, s4
    vcvt.u32.f32 s2, s2
    vmov    r2, s2
    strh    r2, [r4, #0x46]
    vldr    s4, .Lk_min_scale
    vcmpe.f32 s1, s4
    vmrs    APSR_nzcv, fpscr
    vdivge.f32 s3, s3, s1           @ whole scaled ms / time scale = real ms of this step
    vmovlt.f32 s3, s0               @ time scale (near) 0 or NaN: the frame's own ms
.Lshare:
    mov     r2, #0
    vmov    s5, r2
    vcmpe.f32 s3, s5
    vmrs    APSR_nzcv, fpscr
    vmovlt.f32 s3, s5               @ never negative
    vldr    s4, .Lk_100
    vcmpe.f32 s3, s4
    vmrs    APSR_nzcv, fpscr
    vmovgt.f32 s3, s4               @ at most 100 ms
    vldr    s4, .Lk_0p001
    vmul.f32 s3, s3, s4
    vstr    s3, [r4, #0x70]         @ real seconds this step stands for
    cmp     r5, #0
    beq     .Ldone
    str     r5, [sp]
    vldr    s4, .Lk_40
    vcmpe.f32 s0, s4
    vmrs    APSR_nzcv, fpscr
    movle   r2, #0                  @ <= 40 ms: variable step (maxSubSteps 0)
    ldrle   r3, .Lk_1_30
    movgt   r2, #1                  @ > 40 ms: one fixed 0.04 s step
    ldrgt   r3, .Lk_0p04
    mov     r1, sp
    ldr     r0, [r4, #4]
    bl      sub_6866C0
.Ldone:
    sub     sp, r11, #8
    pop     {r4, r5, r11, pc}
    .p2align 2
.Lk_inv65536:
    .float 1.52587890625e-05
.Lk_65536:
    .float 65536.0
.Lk_40:
    .float 40.0
.Lk_0p04:
    .float 0.04
.Lk_1_30:
    .word 0x3D088889                @ the engine's own fixed step (PhysicsSubSystem constructor 0x3ABF90)
.Lk_min_scale:
    .float 0.0009765625
.Lk_100:
    .float 100.0
.Lk_0p001:
    .float 0.001
    .size physics_update, . - physics_update
