/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GPU front end: the channels and their engines' registers, guest memory as the GPU sees it, the GPFIFO queue
   and its GPU thread, and the structures the 2D engine and VIC hand the renderer */
#ifndef SNX_GPU_H
#define SNX_GPU_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/gpu/pushbuffer.h"

/* The pushbuffer's decode state (Eden's DmaPusher::DmaState): the command header whose data words are still coming.
   A header's count can run past the end of its GPFIFO entry into the next one, so the state belongs to the channel,
   not to the entry */
typedef struct {
    u32 mode;         /* the header's PB_MODE_* while words are owed: INCREASING, NON_INCREASING or INCREASE_ONCE */
    u32 subchannel;   /* its subchannel */
    u32 method;       /* the method the next word goes to */
    u32 method_count; /* the words still owed */
} PbState;

/* The macro engine (mme.c) of a channel, kept between steps: a method a macro sends can make the stream wait
   (gpu_park), and the macro goes on from here (mme_resume) once the wait ends */
typedef struct {
    u32 regs[8];     /* r0 reads as 0 */
    u32 carry;       /* of the last add or subtract */
    u32 method;      /* where the next send goes */
    u32 incr;        /* how far the method moves after a send */
    u32 pc;          /* the next instruction, a word index into the channel's macro memory */
    u32 target;      /* where a taken branch goes once its delay slot has run */
    u32 np, next;    /* the parameters of the call (in the channel's mme_params) and the next one to fetch */
    u32 steps;       /* instructions run in this call, against a macro that never ends */
    int running;     /* a call started and not ended: a wait stopped it */
    int branch;      /* a branch was taken: after the next instruction, its delay slot, pc becomes target */
    int slot;        /* the next instruction is a delay slot, whose exit bit doesn't count */
    int exiting;     /* the call ends after the next instruction, the exit's delay slot */
} GpuMme;

/* Inline-to-memory (Eden's engine_upload.h): the methods at 0x60-0x6D of the 3D, compute and inline-to-memory
   classes (maxwell/inline_to_memory_regs.h). A launch (exec_upload, i2m_launch) latches the registers' destination,
   and the data words (data_upload, i2m_data) fill the buffer; the last byte writes it to memory */
typedef struct { /* Eden's Upload::Registers, as the launch found them */
    u32 line_length_in, line_count; /* bytes a line, and lines */
    struct {
        u64 address;
        u32 pitch;                     /* pitch linear: bytes from a line to the next */
        u32 block_height, block_depth; /* block linear: the block, log2 GOBs high and deep */
        u32 width, height, layer;      /* the surface's width in bytes, height in rows, and the slice written */
        u32 x, y;                      /* the origin, x in bytes */
    } dest;
} GpuUploadRegs;
typedef struct { /* Eden's Upload::State */
    GpuUploadRegs regs;
    u8* inner_buffer;
    u32 capacity;     /* bytes inner_buffer has room for */
    u32 copy_size;    /* the bytes the launch wants */
    u32 write_offset; /* and those that came */
    int is_linear;    /* the destination is pitch linear (exec_upload's linear bit), else block linear */
} GpuI2m;

/* The register files a channel keeps of each engine, by method index (byte offset / 4) */
#define GPU_ENGINE_METHODS 0x1000 /* a command header's 12-bit method (pushbuffer.h) reaches all of them */
#define PULLER_NUM_REGS PULLER_NON_PULLER_METHODS /* Eden's Puller::Regs::NUM_REGS */
#define MAXWELL3D_NUM_REGS 0xE00 /* Eden's Maxwell3D::Regs::NUM_REGS: the registers, and the macro calls above */
#define KEPLER_COMPUTE_METHODS GPU_ENGINE_METHODS /* Eden's NUM_REGS is 0xCF8: compute_method takes all 12 bits */
#define FERMI2D_METHODS 0x400 /* the 2D engine's methods kept (Eden's Fermi2D::Regs::NUM_REGS is 0x258) */
#define MAXWELL_DMA_METHODS 0x200 /* the copy engine's methods kept (its registers end below 0x1D0) */
#define KEPLER_I2M_METHODS 0x80   /* the inline-to-memory class's (Eden's KeplerMemory::Regs::NUM_REGS is 0x7F) */
#define GPU_CHANNELS 128 /* channels, one per syncpoint id: nvdrv hands out ids below 128 (NvRmStream's tables) */

/* Groups of the 3D engine's registers, a bit each in GpuChannel.r3d_dirty: maxwell3d_method sets a group's bit when
   a method changes the value of one of its registers (g_m3d_group in maxwell/engines.c), after the shadow RAM's
   replay. A group holds the registers one thing the renderer derives is read from, so that thing is made again only
   when its groups changed. A register may be in several groups. A write that leaves a value as it was sets nothing */
enum {
    R3D_SHADER = 1u << 0,   /* the programs: code address 0x582-0x583, the slots 0x800-0x85F, early fragment tests
                               0x084 */
    R3D_VATTR = 1u << 1,    /* the vertex attributes 0x458-0x477 */
    R3D_VFORMAT = 1u << 2,  /* the vertex streams' stride and enable (0x700 + 4 * i), divisor (0x703 + 4 * i), and
                               whether each is per instance (0x620-0x63F): not their addresses or limits */
    R3D_RTFORMAT = 1u << 3, /* RT_CONTROL 0x487 and each color target's format (0x204 + 0x10 * i) */
    R3D_BLEND = 1u << 4,    /* blending 0x4B9, 0x4D0-0x4DF, 0x780-0x7BF, color masks 0x3E4 and
                               0x680-0x687, sRGB writes 0x56E, the logic operation 0x671-0x672 */
    R3D_PIPEMISC = 1u << 5, /* the rest of the pipeline key and the programs' keys: polygon mode 0x36B, patch size
                               0x373, tessellation parameters 0xC8, user clip enable 0x544, the geometry passthrough
                               mask 0x490-0x497, the provoking vertex 0x5A1, the clip mode 0x64F */
    R3D_XFB = 1u << 6       /* transform feedback: enable 0x1D1, buffer controls 0x1C0-0x1CF, layouts 0xA00-0xA7F */
};

/* A GPU channel: an nvhost-gpu fd's state, named by its syncpoint id. It is made at the channel's first submission
   and dropped when nvdrv closes it (gpu_channel_close) */
typedef struct GpuChannel {
    u32 id;
    u32 sub_class[GPU_SUBCHANNELS]; /* the class bound to each subchannel (PULLER_BIND_OBJECT) */
    PbState pb;
    u32 puller[PULLER_NUM_REGS]; /* the puller's registers (PULLER_*) */
    /* the engines' registers, by method */
    u32 r3d[GPU_ENGINE_METHODS]; /* the 3D engine's. MAXWELL3D_NUM_REGS up are the macro calls' */
    /* the register groups (R3D_*) a method changed a register of since the renderer last took them (all of them
       after a reset): what it derived from the other groups' registers still holds */
    u32 r3d_dirty;
    u32 r3d_shadow[MAXWELL3D_NUM_REGS]; /* the MME shadow RAM (SET_MME_SHADOW_RAM_CONTROL, 3D 0x49) */
    u32 shadow_mode;       /* 0 or 1 track, 2 passthrough, 3 replay */
    u32 rcomp[KEPLER_COMPUTE_METHODS]; /* compute's (maxwell/kepler_compute_regs.h) */
    u32 r2d[FERMI2D_METHODS];
    u32 rdma[MAXWELL_DMA_METHODS];
    u32 ri2m[KEPLER_I2M_METHODS];
    GpuI2m i2m;
    /* macros: the code and start addresses uploaded (3D 0x45-0x48), and the call the stream is collecting the
       parameters of (0xE00 up): a call runs at its batch's last word */
    u32 mme_code[0x2000];
    u32 mme_start[0x80];
    /* a macro's native implementation is chosen by the hash of its code from its start to the end of the upload batch
       that wrote it (mme.c): the batch each code word came in (0 never uploaded; batches count 0x45 writes), and per
       slot 0 when not chosen since the code or the starts changed, 1 for the interpreter, else a native one's index + 2 */
    u32 mme_batch;
    u32 mme_code_batch[0x2000];
    u8 mme_choice[0x80];
    u64 mme_hash[0x80]; /* the hash mme_choose found for each slot, for the interpreted calls' statistics */
    u32 mme_upload, mme_start_ptr;
    u32 mme_params[0x10000];
    u32 mme_nparams;
    int mme_collecting;
    u32 mme_index;
    GpuMme mme; /* the macro running, or one a wait stopped after the method that asked for it */
    /* transform feedback, a bit per buffer: its write pointer was loaded (3D 0xE4 + 8 * i) since its last capture,
       and a capture since that left a byte count in the renderer's counter to resume from */
    u8 xfb_reload, xfb_started;
    /* constant buffer data (3D 0x8E4-0x8F3) a batch is still sending: written to memory in one go */
    u64 cb_addr;
    u32 cb_n;
    u32 cb_words[256];
    /* the constant buffers bound to each stage (3D 0x904 + 8 * stage), address and size, 0 when unbound. The renderer
       reads the drawing channel's: another channel binds its own */
    u64 cbind[5][18];
    u32 cbind_size[5][18];
    int render_off; /* SetRenderEnable (3D 0x554-0x556) or its override (0x651) turned drawing off */
    u32 render_pred; /* the render enable's comparison is made on the GPU: its draws and clears run on this predicate
                        (renderer.h render_predicate), 0 for none */
    /* counter reports (3D 0x6C3 operation 2): g_draws at each selector's last reset, and the selectors reset */
    u64 counter_base[32];
    u32 counter_reset;
    u32 instance_count; /* the instance the next draw is: BEGIN's next-instance bit and the SUBSEQUENT methods count */
    u32* inline_indices; /* the 3D inline index methods' indices, widened to u32 (host numbers), until END draws them */
    u32 inline_n, inline_cap;
    /* the copy engine's launches of the job running: a submission-time copy of a linear source belongs to one */
    u64 dma_job_id;
    u32 dma_launch_index;
    /* what the stream waits for before its next method (gpu_park). The GPU thread runs other channels meanwhile, as
       the GPU's host interface (PFIFO) switches channels on an acquire that fails */
    u32 park;      /* GPU_PARK_*, or 0 */
    u32 park_val;  /* the value compared with, or the syncpoint's threshold */
    int park_geq;  /* reach park_val (a wrapping compare) instead of equalling it */
    u64 park_addr; /* the semaphore's GPU address, or the syncpoint id */
    u64 park_end;  /* the plat_ns() past which the stream goes on anyway */
} GpuChannel;
enum {
    GPU_PARK_SEMAPHORE = 1, /* the puller's SemaphoreOperation acquire */
    GPU_PARK_REPORT,        /* the 3D or compute engine's report semaphore acquire */
    GPU_PARK_SYNCPOINT,     /* the puller's syncpoint wait */
    GPU_PARK_FENCE          /* a submission's fence, waited for before its first entry */
};
typedef struct {
    GpuChannel* chan[GPU_CHANNELS]; /* by syncpoint id */
    int nchan;
} GpuState;
extern GpuState g_gpu;

/* a 2D engine surface, the source or destination of a blit (2D 0x80-0x89 and 0x8C-0x95) */
typedef struct {
    u32 format, linear, bh, depth, layer, pitch, width, height;
    u64 addr;
} Surf2D;

/* the VIC composition (gpu/video/vic.c): a surface plane as VIC addresses it (pitch linear rows or block linear
   GOBs), a slot's resolved rectangles and color matrix, and a job's planes and slots. The renderer's vic_compose
   runs the composition as a compute shader (gpu/vk/vic.c); when it returns 0 the caller composes in guest memory */
typedef struct VicPlane {
    u8* p;   /* the plane's bytes in guest memory */
    u64 lim; /* bytes p may touch */
    u32 pitch, h, blk, bh;
} VicPlane;
typedef struct VicSlot {
    int on;                               /* the slot composes */
    u32 sx0, sy0, dx0, dy0, w, h;         /* the resolved rectangles: source start, destination start and size */
    int mat[3][4], mat_shift, mat_enable; /* the color matrix, out = (R*c0 + G*c1 + B*c2) >> shift, + c3, >> 8 */
    u32 alpha, clamp_lo, clamp_hi;        /* the planar alpha and the soft clamp range (10 bit) */
    u32 vu; /* input format 0x43 (Y8___U8V8_N420): V in each chroma pair's even byte, U in the odd one */
    VicPlane iy, ic;
} VicSlot;
typedef struct VicJob {
    u32 fmt, sw, sh;
    u32 tx0, ty0, tw, th; /* the target rectangle the background colour fills, in the surface; tw 0: none. The
                             slots lie inside it (vic.c's slot_resolve clips them) */
    u32 bg[4];            /* the background colour: R, G, B, A, 10 bit */
    VicPlane oy, oc;      /* oc.p is NULL when the format has no chroma */
    VicSlot slots[8];
} VicJob;
/* An NVDEC picture for its surfaces (gpu/video/nvdec.c): FFmpeg's planar 4:2:0 rows, w x h of them written into NV12
   surfaces, block linear, at guest CPU addresses luma and chroma: widths pitch_y and pitch_c bytes, the luma surface
   surf_h rows high (the chroma's half of that, rounded up), blocks 2^bh GOBs high. The surfaces' extents (whole GOBs
   wide, whole blocks high) are ext_y and ext_c bytes. Only the picture's bytes are written: in a row's last 16
   bytes, those past its width keep their values. A picture Vulkan Video decoded (the vulkan_video setting) is FFmpeg's
   AVFrame of format AV_PIX_FMT_VULKAN in hw instead, its NV12 planes in a GPU image, and plane is unused */
typedef struct NvdecPic {
    const u8* plane[3]; /* Y, U, V */
    int stride[3];
    u32 w, h;
    u64 luma, chroma, ext_y, ext_c;
    u32 pitch_y, pitch_c, surf_h, bh;
    const void* hw;
} NvdecPic;
/* A launch of the DMA copy engine (maxwell/dma.c), for the renderer, which does it on the GPU: lines of width
   elements from src to dst, each side pitch linear (rows pitch bytes apart from addr) or block linear (a surface wb
   bytes wide and h rows high, blocks 2^bh GOBs high and 2^bd deep, slice z, from the origin ox bytes in, oy rows
   down; rows from h on read as zero and take no writes). With remap, an element is ns components of cs bytes in and
   nd out, component k of it from sel[k] (0-3 a source component, 4 constant A, 5 constant B, 6 and 7 the
   destination's own bytes); fill: nothing is read. snap: the source's bytes as read at submission (pitch linear, its
   span), or NULL */
typedef struct {
    u64 addr;
    int bl;
    u32 pitch, wb, h, z, bh, bd, ox, oy;
} GpuDmaSide;
typedef struct {
    int multi, remap, fill, shuffle, keep;
    u32 cs, ns, nd, sel[4], const_a, const_b;
    u32 width, lines, line_in, line_out; /* elements a line, lines, a line's bytes in and out */
    GpuDmaSide src, dst;
    const u8* snap;
} GpuDmaOp;
/* A 2D engine blit (maxwell/fermi2d.c) the renderer does on the GPU: each destination pixel of columns x0 to x1 - 1
   and rows y0 to y1 - 1 the source pixel at column scol[x - x0] and row srow[y - y0] (nearest sampled, the tables
   the engine works out), bpp bytes each. The sides are surface planes as the DMA engine's are (no origin). fmt 0
   copies the pixels' bytes; else both are float formats (GPU_BLIT_FLOAT) and each component is converted */
typedef struct {
    GpuDmaSide src, dst;
    u32 bpp, x0, x1, y0, y1;
    const u32* scol;
    const u32* srow;
    u32 fmt;
} GpuBlit;
/* a blit's fmt between float formats: the source's components (1 to 4) of sb bytes (2 or 4), the destination's (dn
   of db bytes), sx when the source's fourth component is padding. A component the source lacks is 0, its alpha 1.
   bpp is the source's pixel, sn * sb bytes. texconv.glsl's TC_BLIT_FLOAT takes it as it is */
#define GPU_BLIT_FLOAT(sn, sb, dn, db, sx) ((sn) | (sb) << 4 | (dn) << 8 | (db) << 12 | (u32)(sx) << 16)
/* the bytes of a blit's destination pixel */
#define GPU_BLIT_DST_BPP(b)                ((b)->fmt ? ((b)->fmt >> 8 & 15) * ((b)->fmt >> 12 & 15) : (b)->bpp)

/* ---- gpu.c: guest memory through the GPU's address space. A read lands the renderer's pending results over the
   range first, a write lets it land what would overwrite the write later, and records the write (gpu_mem_written).
   Unmapped pages read as zero and take no writes; the int calls return -1 when any page was unmapped */
int gmem_read(u64 gva, void* dst, u64 size);
int gmem_write(u64 gva, const void* src, u64 size);
/* gmem_read and gmem_write in halves, for a copy made of many small runs (a block linear row's 16 byte runs): the
   hooks once over the span the runs fall in (gmem_need before reading it, gmem_before_write before writing it and
   gpu_mem_written after), then the runs without them */
void gmem_need(u64 gva, u64 size);
void gmem_read_raw(u64 gva, void* dst, u64 size);
void gmem_before_write(u64 gva, u64 size);
void gmem_write_raw(u64 gva, const void* src, u64 size);
/* a shader program: up to size bytes, stopping at the first page with no guest memory behind it, zeros after */
void gmem_read_code(u64 gva, void* dst, u64 size);
/* the host address of [gva, gva + size) when it is contiguous in guest memory, after landing pending results over
   it; NULL otherwise */
u8* gmem_ptr(u64 gva, u64 size);
/* the same without landing anything: only for comparing a pending result's original guest bytes */
u8* gmem_ptr_untracked(u64 gva, u64 size);
/* the host address of [gva, gva + size) when contiguous, nothing landed and nothing acquired: only to name the range
   (a write-watch scan), never to read or write through, as GPU-owned bytes there are trapped (memory/own.c) */
u8* gmem_host(u64 gva, u64 size);
/* a word of guest memory as a number (little-endian there) */
u32 gmem_r32(u64 gva);
void gmem_w32(u64 gva, u32 v);
/* the byte offset of (x_bytes, y, z) in a block linear surface width_bytes wide and height rows high, with blocks of
   2^bh_log2 GOBs high and 2^bd_log2 deep (the caller shrinks them for small surfaces, as the hardware does). The
   layout is texture/blocklinear.h's */
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2);
/* an inline-to-memory launch from an engine's registers (Eden's ProcessExec: regs[base] is the upload block,
   I2M_UPLOAD_REGS, so regs[base + UPLOAD_EXEC] is the launch), and its data words (ProcessData) */
void i2m_launch(GpuChannel* ch, u32* regs, int base);
void i2m_data(GpuChannel* ch, u32 v);
u32 gpu_color_bpp(u32 format); /* bytes per pixel of a color format (render targets and 2D surfaces share them) */
void gpu_signal_point(void);   /* the GPU is about to do something the game can observe */
/* a semaphore or report payload the game can poll: it happens once everything before it in the stream is done. data is
   the bytes as guest memory gets them, so its numbers are little-endian already */
void gpu_release_write(u64 gva, const void* data, u32 len);

/* the engines' methods (engines.c, compute.c, fermi2d.c, dma.c): m is the method, last marks a batch's last word */
void maxwell3d_method(GpuChannel* ch, u32 m, u32 v, int last);
void compute_method(GpuChannel* ch, u32 m, u32 v, int last);
void kepler_i2m_method(GpuChannel* ch, u32 m, u32 v);
void fermi2d_method(GpuChannel* ch, u32 m, u32 v);
void dma_method(GpuChannel* ch, u32 m, u32 v);
/* dma.c: the linear source of a LAUNCH_DMA (v) with registers r, read now for the channel's copy to use when it runs:
   called in the channel's submit order, for every launch the stream will run (gpu_submit) */
void dma_snap_launch(u32 channel, u64 job_id, u32 launch_index, const u32* r, u32 v);
void dma_snap_finish(u32 channel, u64 job_id); /* the job ended: what it left goes */
void dma_snap_reset(u32 channel);              /* the channel closed: what it queued goes */
/* save states (runtime/state.c), with the GPU thread held: the channels, the queue, the scans and the copy snapshots
   into the state (0, or -1). A load reads them first into a staged copy (NULL when the section is damaged), which a
   refused load frees, and puts it in place once the running game is gone, every channel and queued item there was
   dropped first */
struct StateIo;
typedef struct GpuStaged GpuStaged;
int gpu_state_save(struct StateIo* io);
GpuStaged* gpu_state_parse(struct StateIo* io);
void gpu_state_install(GpuStaged* s);
void gpu_state_staged_free(GpuStaged* s);
/* the layout of what gpu_state_save writes as raw bytes beyond GpuChannel, into the save state's schema hash at h, dump
   taking each struct's walk (state_io.h's StateSchemaFn) */
void gpu_state_schema(int (*dump)(u64* h, const char* fmt, ...), u64* h);
typedef struct DmaStaged DmaStaged;
int dma_snap_save(struct StateIo* io);
DmaStaged* dma_snap_parse(struct StateIo* io);
void dma_snap_install(DmaStaged* d);
void dma_snap_staged_free(DmaStaged* d);

/* maxwell3d.c: the record of GPU-side writes. Each write stamps a new generation on the pages it covers, by GPU
   address and by the guest pages behind them */
void gpu_mem_written(u64 gva, u64 size);
void gpu_mem_written_shader(u64 gva, u64 size); /* a shader's result landing in guest memory */
/* guest CPU bytes [cpu, cpu + size) written by an engine that doesn't go through the GPU's address space (VIC's
   picture, published over them, memory/own.c): what is built from them checks them again */
void gpu_mem_written_cpu(u64 cpu, u64 size);
u64 gpu_mem_generation(void);                   /* the last generation fully published */
void gpu_mem_written_all(void); /* a save state's load: every page written, after anything asked before */
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz); /* a GPU-side write after since touched [a, a + sz) */

/* ---- gpu.c: GPFIFO submission and the GPU thread's queue */
void gpu_init(void);
/* queues GPFIFO entries for the GPU thread, which waits for the fence (wait_id ~0: none) as the stream's first
   method, runs them on channel (the channel's syncpoint id), then increments the syncpoint incs times, after which it
   should be at expect (the fence nvdrv promised). The entries are u64s at any alignment. A channel's jobs run in
   order, and another channel's may run before them while they wait (gpu_park) */
void gpu_submit(const void* entries, u32 n, u32 channel, u32 incs, u32 wait_id, u32 wait_val, u32 expect);
/* calls fn(arg) on the GPU thread in order with the other calls, once the jobs queued before it on channels
   chans[0..nchans) (n at most 4) are done. Other channels' jobs, queued before or after it, don't hold it back */
void gpu_call(void (*fn)(void*), void* arg, const u32* chans, u32 nchans);
void gpu_channel_close(u32 channel);  /* nvdrv closed the channel: a new one with the same id starts from reset */
void gpu_release_retire(u32 channel); /* complete pending syncpoint releases before nvdrv reuses its id */
/* the channel's stream waits for the u32 at GPU address addr (a syncpoint's id for the syncpoint kinds) to equal val,
   or reach it (geq), before its next method. Met already, nothing waits. Otherwise the method that asked returns and
   the stream stops after it (a macro after the method too, mme_resume), and the GPU thread goes on with other
   channels until it's met */
void gpu_park(GpuChannel* ch, u32 kind, u64 addr, u32 val, int geq);
/* something a parked channel may wait for changed (a syncpoint increment, on the GPU thread or off it): look again.
   Any thread */
void gpu_kick(void);
/* a game pause, with no guest thread left to submit: the GPU thread runs what is queued up to now (channels waiting on
   the CPU stay parked), the renderer finishes (Renderer.idle), and the thread waits until gpu_release. 0, or -1 when it
   didn't get there within timeout_ms (and won't stop when it does) */
int gpu_hold(int timeout_ms);
void gpu_release(void);
extern volatile u64 g_gpu_submits;
/* the blocking wait the GPU thread is in, for the stall report: what it waits for (a string that lives on) and a number
   that names it, before the wait, and NULL after. Ignored on other threads */
void gpu_wait_note(const char* what, u64 value);
/* the stall watchdog's report of what the GPU thread and each channel with queued jobs are doing. Any thread */
void gpu_report_stall(void);
/* guest memory [cpu_addr, cpu_addr + len) changed without a write the host's write tracking sees: decommitted pages,
   which now read as zero (memory/mem.c). Caches over that memory check it again. Any thread */
void gpu_mem_changed(u64 cpu_addr, u64 len);
extern void (*g_gpu_mem_changed)(u64 cpu_addr, u64 len); /* the renderer's side of it, set when it starts */
/* the renderer lands pending shader results before the stream goes on past them: before a write of memory those
   results overlap (size > 0), and before anything the game can observe (gpu_signal_point, size 0) */
extern void (*g_gpu_before_write)(u64 gva, u64 size);
extern int g_gpu_releases; /* the renderer holds releases waiting to happen (renderer.h release_need) */
/* the renderer's memory lock held across a change of backing. full is for CPU guest pages, whose GPU aliases are
   unknown; GPU address space changes give the range. Begin is taken before the memory manager's own lock */
extern void (*g_gpu_mapping_begin)(u64 gva, u64 size, int full);
extern void (*g_gpu_mapping_end)(void);

/* ---- engines.c: the 3D engine's methods and the report semaphores */
u64 gpu_ticks(void); /* the GPU's timestamp: 614.4 MHz ticks of the guest clock */
/* draws that differ only in their instance are merged into one instanced draw while they come in. The merged draw
   is sent before anything that could see it is missing (another method, another channel, the stream's end) */
void r3d_pending_flush(void);
int r3d_pending_on(void);
/* mme.c: a macro a wait stopped (ch->mme.running) goes on, until it ends or waits again */
void mme_resume(GpuChannel* ch);

/* ---- maxwell3d.c: the 3D engine's side of the renderer */
void r3d_clear(GpuChannel* ch, u32 v);
/* a clear is cut to scissor 0 when CLEAR_FLAGS (3D 0x43E) has SCISSOR, bit 8, and that scissor is enabled. Bit 4,
   CLEAR_RECT, is another bound (nouveau names the bits; Eden reads the scissor from bit 8 too) */
static inline int r3d_clear_scissored(const u32* r) { return (r[0x43E] & 0x100) && (r[0x380] & 1); }
void r3d_draw_texture(GpuChannel* ch); /* DRAW_TEXTURE (3D 0x420-0x42B) */
/* a draw: indexed 0 none, 1 the index buffer, 3 the channel's inline indices */
void r3d_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance,
              u32 instances);
void r3d_cb_bind(GpuChannel* ch, int stage, u32 v); /* records the binding in ch->cbind */
void r3d_const_upload(GpuChannel* ch, u32 v, int last);
void r3d_const_flush(GpuChannel* ch); /* writes the constant buffer data r3d_const_upload is holding */
int r3d_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
             double sdy);
#endif
