/*
 * t3d_n64.c - Tiny3D on the RSP, alongside the PSX pipeline.
 *
 * Stage 0 of the native-renderer rewrite: prove that Tiny3D's ucode loads,
 * transforms vertices on the RSP and rasterises through the same rdpq frame
 * the PSX path draws into. The spike is a shaded spinning quad drawn at
 * GpuNv2a_FrameEnd, after the OT walk's content, so both renderers appear in
 * one frame.
 *
 * Compiled -std=gnu2x (t3d headers carry C23 [[deprecated]] attributes);
 * the gate has a per-file override for this TU.
 *
 * State discipline: t3d_frame_start() reprograms rdpq modes for 3D. That is
 * safe HERE because the PSX batch was flushed and nothing 2D draws after us
 * this frame; next frame's GpuNv2a_FrameBegin resets the mode memo
 * (s_appliedTex/s_appliedBlend) and rdpq_attach re-establishes 2D state. If
 * this call ever moves earlier in the frame, the memo must be invalidated
 * afterwards -- same class as GpuNv2a_TryBlitQuad.
 */
#include <libdragon.h>

#include <stdlib.h>

#include <t3d/t3d.h>
#include <t3d/t3dmath.h>

#include "sh_log.h"
#include "sh_t3d.h"

static int s_up;                 /* init succeeded, spike may draw       */
static int s_initTried;
static T3DViewport   s_viewport;
static T3DMat4FP*    s_modelMatFP;   /* uncached, DMA'd by the RSP       */
static T3DVertPacked* s_verts;       /* uncached, 4 verts = 2 structs    */
static float         s_rotAngle;

/* COP0 ticks -> us on this console (46.875 ticks/us). */
#define T3D_TICKS_TO_US(t) ((unsigned)((t) * 8ull / 375ull))

/* 0 now that the spike proved itself (title screenshot 20260902_030517 +
 * 1600 stable frames): the quad only obscures the world work. Flip to 1 for
 * a quick "is t3d alive" check. */
#define SH_T3D_SPIKE 0

int ShT3d_Ready(void)
{
    if (!s_up && !s_initTried)
    {
        static int s_warm;
        /* Same deferral as the spike: never init during the boot window. */
        if (s_warm++ >= 120)
            ShT3d_Init();
    }
    return s_up;
}

void ShT3d_Init(void)
{
    uint16_t norm;

    if (s_up)
        return;
    s_initTried = 1;

    /* Defaults: 8-deep matrix stack, both ucodes lazily loaded. Small heap
     * cost (~2 KB state + matrix stack), checked allocations below are ours. */
    t3d_init((T3DInitParams){});

    s_modelMatFP = malloc_uncached(sizeof(T3DMat4FP));
    s_verts      = malloc_uncached(sizeof(T3DVertPacked) * 2);
    if (s_modelMatFP == NULL || s_verts == NULL)
    {
        SH_DBG("[T3D] init FAILED: uncached alloc (%p %p)", (void*)s_modelMatFP, (void*)s_verts);
        return;
    }

    norm = t3d_vert_pack_normal(&(T3DVec3){{0, 0, 1}});
    s_verts[0] = (T3DVertPacked){
        .posA = {-16, -16, 0}, .rgbaA = 0xFF0000FF, .normA = norm,
        .posB = { 16, -16, 0}, .rgbaB = 0x00FF00FF, .normB = norm,
    };
    s_verts[1] = (T3DVertPacked){
        .posA = { 16,  16, 0}, .rgbaA = 0x0000FFFF, .normA = norm,
        .posB = {-16,  16, 0}, .rgbaB = 0xFFFF00FF, .normB = norm,
    };

    s_viewport = t3d_viewport_create();
    s_up = 1;
    SH_DBG("[T3D] up: ucode registered, %u B vert buf, %u B matrix",
           (unsigned)(sizeof(T3DVertPacked) * 2), (unsigned)sizeof(T3DMat4FP));
}

void ShT3d_SpikeDraw(void)
{
    unsigned long long t0;
    T3DMat4 modelMat;
    T3DVec3 rotAxis = {{-1.0f, 2.5f, 0.25f}};

    static const uint8_t colorAmbient[4] = {80, 80, 100, 0xFF};
    static const uint8_t colorDir[4]     = {0xFF, 0xFF, 0xFF, 0xFF};
    T3DVec3 lightDir = {{0.0f, 0.0f, 1.0f}};

    static int s_census;
    static int s_skip;
    int trace;

    if (!SH_T3D_SPIKE)
        return;

    /* LAZY init, ~2 s after frames start: with init at GpuNv2a_Init the boot
     * wedged at a DIFFERENT [BOOT] step per build with the spike never
     * reached, i.e. something in the boot path (the PSX-RAM/arena setup is
     * the suspect class) tramples early-boot heap allocations or the rspq
     * overlay this registers. Deferring until the game is presenting frames
     * sidesteps the window and makes the first-draw step trace meaningful. */
    if (!s_up)
    {
        if (s_initTried || s_skip++ < 120)
            return;
        ShT3d_Init();
        if (!s_up)
            return;
    }

    /* One-shot step trace on the FIRST invocation: the boot wedged somewhere
     * in this function; whichever marker is the last one in the log names the
     * dying call. */
    trace = (s_census == 0);
#define T3D_STEP(name) do { if (trace) SH_DBG("[T3D] step " name); } while (0)

    t0 = get_ticks();
    s_rotAngle += 0.06f;

    fm_vec3_norm(&rotAxis, &rotAxis);
    fm_mat4_from_axis_angle(&modelMat, &rotAxis, s_rotAngle);
    /* Park it upper-right so logos/menus behind it stay recognisable. */
    modelMat.m[3][0] = 7.0f;
    modelMat.m[3][1] = 5.0f;
    T3D_STEP("mat");
    t3d_mat4_to_fixed(s_modelMatFP, &modelMat);

    T3D_STEP("frame_start");
    t3d_frame_start();
    /* t3d_frame_start enables Z compare+write unconditionally, but this frame
     * has NO Z buffer (rdpq_attach(fb, NULL)) -- the RDP would read/write
     * depth at a stale Z-image address, i.e. scribble over random RDRAM.
     * That was the boot-garbage/delayed-crash class of the first spike runs.
     * Off until the world stage attaches a real Z buffer. */
    rdpq_mode_zbuf(false, false);

    T3D_STEP("viewport");
    t3d_viewport_set_projection(&s_viewport, T3D_DEG_TO_RAD(85.0f), 2.0f, 100.0f);
    t3d_viewport_look_at(&s_viewport, &(T3DVec3){{0, 0, 18}}, &(T3DVec3){{0, 0, 0}},
                         &(T3DVec3){{0, 1, 0}});
    t3d_viewport_attach(&s_viewport);

    T3D_STEP("combiner");
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);

    T3D_STEP("lights");
    t3d_light_set_ambient(colorAmbient);
    fm_vec3_norm(&lightDir, &lightDir);
    t3d_light_set_directional(0, colorDir, &lightDir);
    t3d_light_set_count(1);

    /* No T3D_FLAG_DEPTH: the frame has no Z buffer yet (rdpq_attach(fb, NULL)).
     * Depth arrives with the world stage, funded by the IPD render-strip. */
    t3d_state_set_drawflags(T3D_FLAG_SHADED);

    T3D_STEP("draw");
    t3d_matrix_push(s_modelMatFP);
    t3d_vert_load(s_verts, 0, 4);
    t3d_matrix_pop(1);
    t3d_tri_draw(0, 1, 2);
    t3d_tri_draw(2, 3, 0);
    t3d_tri_sync();
    T3D_STEP("done");

    if ((s_census++ & 127) == 0)
        SH_DBG("[T3D] spike alive f=%d cpu=%uus", s_census - 1,
               T3D_TICKS_TO_US(get_ticks() - t0));
#undef T3D_STEP
}
