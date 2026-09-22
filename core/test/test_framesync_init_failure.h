/**
 *
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef VMAFX_TEST_FRAMESYNC_INIT_FAILURE_H_
#define VMAFX_TEST_FRAMESYNC_INIT_FAILURE_H_

#include <pthread.h>

int vmafx_test_pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes);
int vmafx_test_pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes);
int vmafx_test_pthread_mutex_destroy(pthread_mutex_t *mutex);
int vmafx_test_pthread_cond_destroy(pthread_cond_t *condition);

#endif /* VMAFX_TEST_FRAMESYNC_INIT_FAILURE_H_ */
