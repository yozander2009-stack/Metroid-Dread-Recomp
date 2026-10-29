/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/dma_pusher.h, src/video_core/engines/puller.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
   SPDX-FileCopyrightText: 2022 yuzu Emulator Project */
/* The GPU's host interface as gpu.c decodes it: GPFIFO entries, the pushbuffer's command headers, the puller's
   methods (those below 0x40 on any subchannel) and the classes the subchannels bind. Method numbers here, as
   everywhere in the GPU front end, are method indices: the byte offset NVIDIA's class headers give, divided by 4.
   Field positions are NVIDIA's open-gpu-doc (cla16f.h, the Kepler host class the GM20B's matches) where Eden reads
   them differently, and Eden's otherwise */
#ifndef SNX_GPU_PUSHBUFFER_H
#define SNX_GPU_PUSHBUFFER_H
#include "common/cpu/aarch64/cpu.h"

/* ---- a GPFIFO entry (Eden's CommandListHeader): where a pushbuffer segment is and how many words it has */
#define GPFIFO_ADDR_MASK 0xFFFFFFFFFCull /* the segment's GPU address, bits 2-39: word aligned, a 40-bit address */
#define GPFIFO_SIZE_SHIFT 42             /* its length in words, bits 42-62 */
#define GPFIFO_SIZE_MASK 0x1FFFFFu
#define GPFIFO_SYNC_SHIFT 63 /* SYNC, bit 63: the commands from here on are fetched only when the stream gets here */
static inline u64 gpfifo_addr(u64 e) { return e & GPFIFO_ADDR_MASK; }
static inline u32 gpfifo_size(u64 e) { return (u32)((e >> GPFIFO_SIZE_SHIFT) & GPFIFO_SIZE_MASK); }
static inline int gpfifo_sync(u64 e) { return (int)(e >> GPFIFO_SYNC_SHIFT); }

/* ---- a pushbuffer command header (Eden's CommandHeader): a method, its subchannel, how many data words follow and
   how they map to methods. The method is bits 11:0 (cla16f's NVA16F_DMA_METHOD_ADDRESS 11:0; Eden reads 13 bits,
   bit 12 included), the subchannel 15:13, the count 28:16, the mode 31:29 */
#define PB_HEADER_METHOD_MASK 0xFFFu
#define PB_HEADER_SUBCHANNEL_SHIFT 13
#define PB_HEADER_SUBCHANNEL_MASK 7u
#define PB_HEADER_COUNT_SHIFT 16
#define PB_HEADER_COUNT_MASK 0x1FFFu
#define PB_HEADER_MODE_SHIFT 29
static inline u32 pb_header_method(u32 w) { return w & PB_HEADER_METHOD_MASK; }
static inline u32 pb_header_subchannel(u32 w) { return (w >> PB_HEADER_SUBCHANNEL_SHIFT) & PB_HEADER_SUBCHANNEL_MASK; }
static inline u32 pb_header_count(u32 w) { return (w >> PB_HEADER_COUNT_SHIFT) & PB_HEADER_COUNT_MASK; }
static inline u32 pb_header_mode(u32 w) { return w >> PB_HEADER_MODE_SHIFT; }
/* the header's mode (Eden's SubmissionMode). Increasing, NonIncreasing and IncreaseOnce are followed by count data
   words; Inline carries its one data word in the count field. The old modes (Fermi's) carry no data words in this
   stream */
enum {
    PB_MODE_INCREASING_OLD = 0,
    PB_MODE_INCREASING = 1, /* word k goes to method + k */
    PB_MODE_NON_INCREASING_OLD = 2,
    PB_MODE_NON_INCREASING = 3, /* every word goes to method */
    PB_MODE_INLINE = 4,         /* the count field is the data (IMMD) */
    PB_MODE_INCREASE_ONCE = 5   /* the first word goes to method, the rest to method + 1 */
};

/* ---- subchannels and the classes they bind (Eden's EngineID): BindObject names the class of its subchannel */
#define GPU_SUBCHANNELS 8 /* Eden's DmaPusher max_subchannels */
enum {
    CLASS_FERMI_TWOD_A = 0x902D,              /* the 2D engine (fermi2d.c) */
    CLASS_MAXWELL_B = 0xB197,                 /* the 3D engine (engines.c) */
    CLASS_KEPLER_COMPUTE_B = 0xB1C0,          /* compute (compute.c) */
    CLASS_KEPLER_INLINE_TO_MEMORY_B = 0xA140, /* inline to memory (compute.c, gpu.c) */
    CLASS_MAXWELL_DMA_COPY_A = 0xB0B5         /* the copy engine (dma.c) */
};
#define CLASS_ID_MASK 0xFFFFu /* BindObject's class, bits 15:0 (cla16f's NVA16F_SET_OBJECT_NVCLASS 15:0) */

/* ---- the puller's methods (Eden's BufferMethods, cla16f.h), on every subchannel below NonPullerMethods */
enum {
    PULLER_BIND_OBJECT = 0x0,
    PULLER_ILLEGAL = 0x1,
    PULLER_NOP = 0x2,
    PULLER_SEMAPHORE_ADDRESS_HIGH = 0x4, /* SEMAPHOREA: the address's bits 39:32, in bits 7:0 */
    PULLER_SEMAPHORE_ADDRESS_LOW = 0x5,  /* SEMAPHOREB: its bits 31:0 (2:0 zero) */
    PULLER_SEMAPHORE_SEQUENCE_PAYLOAD = 0x6, /* SEMAPHOREC: the value released or compared with */
    PULLER_SEMAPHORE_OPERATION = 0x7,    /* SEMAPHORED: the operation, on the three before */
    PULLER_NON_STALL_INTERRUPT = 0x8,
    PULLER_WRCACHE_FLUSH = 0x9,
    PULLER_MEM_OP_A = 0xA,
    PULLER_MEM_OP_B = 0xB,
    PULLER_MEM_OP_C = 0xC,
    PULLER_MEM_OP_D = 0xD,
    PULLER_REF_CNT = 0x14,
    PULLER_SEMAPHORE_ACQUIRE = 0x1A,
    PULLER_SEMAPHORE_RELEASE = 0x1B,
    PULLER_SYNCPOINT_PAYLOAD = 0x1C,   /* SYNCPOINTA: the threshold a syncpoint wait waits for */
    PULLER_SYNCPOINT_OPERATION = 0x1D, /* SYNCPOINTB: a syncpoint's increment or wait (Eden's fence_action) */
    PULLER_WAIT_FOR_IDLE = 0x1E,
    PULLER_CRC_CHECK = 0x1F,
    PULLER_YIELD = 0x20,
    PULLER_NON_PULLER_METHODS = 0x40 /* methods from here on go to the subchannel's engine */
};
#define PULLER_SEMAPHORE_ADDRESS_HIGH_MASK 0xFFu /* SEMAPHOREA's OFFSET_UPPER 7:0 */
/* SEMAPHORED: the operation, bits 4:0 (Eden's GpuSemaphoreOperation, cla16f's NVA16F_SEMAPHORED_OPERATION) */
#define PULLER_SEMAPHORE_OP_MASK 0x1Fu
enum {
    PULLER_SEMAPHORE_OP_ACQUIRE_EQUAL = 0x1,  /* wait until the semaphore equals the payload */
    PULLER_SEMAPHORE_OP_RELEASE = 0x2,        /* write the payload (Eden's WriteLong) */
    PULLER_SEMAPHORE_OP_ACQUIRE_GEQUAL = 0x4, /* wait until it reaches the payload, a wrapping compare */
    PULLER_SEMAPHORE_OP_ACQUIRE_MASK = 0x8,   /* wait until it ANDs with the payload to non-zero (ACQ_AND) */
    PULLER_SEMAPHORE_OP_REDUCTION = 0x10      /* an atomic operation on it (cla16f's REDUCTION) */
};
/* SEMAPHORED's RELEASE_SIZE, bit 24: 1 writes the payload's 4 bytes, 0 the 16 byte report (payload, 0, timestamp) */
#define PULLER_SEMAPHORE_RELEASE_SIZE_4BYTE (1u << 24)
/* SYNCPOINTB (Eden's FenceAction): the operation in bit 0, the syncpoint in bits 19:8 (cla16f's
   NVA16F_SYNCPOINTB_SYNCPT_INDEX 19:8; Eden reads 24 bits) */
#define PULLER_SYNCPOINT_OP_INCREMENT 1u /* Eden's FenceOperation::Increment; 0 is Acquire, a wait */
#define PULLER_SYNCPOINT_ID_SHIFT 8
#define PULLER_SYNCPOINT_ID_MASK 0xFFFu
static inline int puller_syncpoint_increment(u32 v) { return (int)(v & PULLER_SYNCPOINT_OP_INCREMENT); }
static inline u32 puller_syncpoint_id(u32 v) { return (v >> PULLER_SYNCPOINT_ID_SHIFT) & PULLER_SYNCPOINT_ID_MASK; }

#endif
