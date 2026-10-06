/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The producer side of the VMAFx SYCL lane tests (RC4 WP3, ADR-2091): a
 * SYCL queue on the first Level Zero GPU, in that device's default context
 * (the context of a VMAFx SYCL device opened by index), behind a C ABI the C
 * tests call (vmafx_sycl_producer.cpp, compiled by the SYCL compiler).
 */

#ifndef VMAFX_SYCL_PRODUCER_H
#define VMAFX_SYCL_PRODUCER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NOLINTBEGIN(modernize-use-using): C header included by C and C++ translation units. ADR-1138. */
typedef struct VsProducer VsProducer;
typedef struct VsLoad VsLoad;
/* NOLINTEND(modernize-use-using) */

/* An in-order queue on Level Zero GPU 0, or NULL without one. */
VsProducer *vs_producer_open(void);
/* Drain and free. */
void vs_producer_close(VsProducer *p);
/* The queue (a sycl::queue *), for VmafxDeviceDesc.external[0]. */
uintptr_t vs_producer_queue(VsProducer *p);

/* Device USM of the producer's context, or NULL. */
void *vs_alloc(VsProducer *p, size_t bytes);
/* Shared / host USM of the producer's context, or NULL. */
void *vs_alloc_shared(VsProducer *p, size_t bytes);
void *vs_alloc_host(VsProducer *p, size_t bytes);
/* Device USM whose memory is exported as a dma-buf: its descriptor in
 * `*fd` (the caller closes it) and the allocation's size in `*size`, or
 * NULL when the driver does not export. */
void *vs_alloc_dmabuf(VsProducer *p, size_t bytes, int *fd, uint64_t *size);
void vs_free(VsProducer *p, void *ptr);

/* Enqueue `rows` rows of `row` bytes from `src` (host or USM, `src_pitch`)
 * to `dst` (`dst_pitch`). 0 or -1. */
int vs_copy_2d(VsProducer *p, void *dst, size_t dst_pitch, const void *src, size_t src_pitch,
               size_t row, size_t rows);
/* Enqueue a fill of `bytes` bytes of `value`. */
int vs_fill(VsProducer *p, void *dst, uint8_t value, size_t bytes);
/* Hold the queue: a host task that sleeps `us` microseconds. Work that
 * depends on a host task is resolved by the SYCL scheduler on the thread
 * that submits it, which then waits for the host task (DPC++ 2026.0). */
int vs_hold(VsProducer *p, unsigned us);
/* Hold the queue on the device: one work-item spinning for about `us`
 * microseconds (calibrated once), so work that depends on it is queued
 * without a host wait, as behind a decoder's GPU work. 0 or -1. */
int vs_hold_device(VsProducer *p, unsigned us);
/* The queue waits on `event` (a sycl::event *) before its next command. */
int vs_after(VsProducer *p, uintptr_t event);
/* Wait until the queue is idle. 0 or -1. */
int vs_finish(VsProducer *p);
/* The event of the queue's last command, as a sycl::event * the caller frees
 * with vs_event_free() (0 when the queue had none). */
uintptr_t vs_last_event(VsProducer *p);
void vs_event_free(uintptr_t event);
/* Store the event of the queue's last command into the sycl::event at
 * `handle` (a SYCL_EVENT fence of vmafx_fence_create()). 0 or -1. */
int vs_store_last_event(VsProducer *p, uintptr_t handle);
/* 1 when `event` completed, 0 when not, -1 on an error. */
int vs_event_done(uintptr_t event);

/* Device load: a thread that keeps `in_flight` busy kernels queued on a
 * queue of its own until stopped. NULL without a device. */
VsLoad *vs_load_start(unsigned in_flight);
void vs_load_stop(VsLoad *load);

#ifdef __cplusplus
}
#endif

#endif /* VMAFX_SYCL_PRODUCER_H */
