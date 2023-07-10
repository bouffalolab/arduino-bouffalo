/**
 * @file bflb_ringbuffer.h
 * @brief
 *
 * Copyright (c) 2021 Bouffalolab team
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 */

#ifndef _BFLB_RINGBUFFER_H
#define _BFLB_RINGBUFFER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t in;
    uint32_t out;
    uint32_t mask;
    void *pool;
} bflb_ringbuffer_t;

extern int bflb_ringbuffer_init(bflb_ringbuffer_t *rb, void *pool, uint32_t size);
extern void bflb_ringbuffer_reset(bflb_ringbuffer_t *rb);
extern void bflb_ringbuffer_reset_write(bflb_ringbuffer_t *rb);
extern void bflb_ringbuffer_reset_read(bflb_ringbuffer_t *rb);

extern uint32_t bflb_ringbuffer_get_size(bflb_ringbuffer_t *rb);
extern uint32_t bflb_ringbuffer_get_used(bflb_ringbuffer_t *rb);
extern uint32_t bflb_ringbuffer_get_free(bflb_ringbuffer_t *rb);

extern bool bflb_ringbuffer_check_full(bflb_ringbuffer_t *rb);
extern bool bflb_ringbuffer_check_empty(bflb_ringbuffer_t *rb);

extern bool bflb_ringbuffer_write_byte(bflb_ringbuffer_t *rb, uint8_t byte);
extern bool bflb_ringbuffer_overwrite_byte(bflb_ringbuffer_t *rb, uint8_t byte);
extern bool bflb_ringbuffer_peek_byte(bflb_ringbuffer_t *rb, uint8_t *byte);
extern bool bflb_ringbuffer_read_byte(bflb_ringbuffer_t *rb, uint8_t *byte);
extern bool bflb_ringbuffer_drop_byte(bflb_ringbuffer_t *rb);

extern uint32_t bflb_ringbuffer_write(bflb_ringbuffer_t *rb, void *data, uint32_t size);
extern uint32_t bflb_ringbuffer_overwrite(bflb_ringbuffer_t *rb, void *data, uint32_t size);
extern uint32_t bflb_ringbuffer_peek(bflb_ringbuffer_t *rb, void *data, uint32_t size);
extern uint32_t bflb_ringbuffer_read(bflb_ringbuffer_t *rb, void *data, uint32_t size);
extern uint32_t bflb_ringbuffer_drop(bflb_ringbuffer_t *rb, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
