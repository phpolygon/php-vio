/*
 * php-vio - accessibility tree (DESKTOP-APP-PLAN Phase 5, AccessKit)
 *
 * One tree per window (vio_input_state.a11y). The application describes its UI
 * as nodes with string ids (vio_a11y_update); vio keeps them in C memory, since
 * AccessKit reads the tree from its own thread on some platforms, and hands
 * AccessKit only what changed. Actions from assistive technology are queued
 * and delivered on the PHP thread (vio_a11y_dispatch, from poll / wait).
 * Without HAVE_ACCESSKIT the contract is still checked, nothing is kept.
 */

#ifndef VIO_A11Y_H
#define VIO_A11Y_H

#include "php.h"

struct _vio_input_state;

/* 1 = this build has AccessKit and the active platform an adapter for it. */
int  vio_a11y_available(void);

/* Apply an update (['root' => id, 'focus' => id, 'nodes' => [...]]) to the
 * window's tree. Returns 1 when applied, 0 when accessibility is not available
 * here; -1 after throwing (contract). platform_window: the window it belongs to. */
int  vio_a11y_update(struct _vio_input_state *in, void *platform_window, HashTable *update, uint32_t arg);

/* PHP thread: deliver queued actions to the window's vio_on_a11y_action callback. */
void vio_a11y_dispatch(struct _vio_input_state *in);

/* The window gained / lost focus (from the input funnel). */
void vio_a11y_focus(struct _vio_input_state *in, int focused);

/* Free the window's tree and adapter (before its platform window goes). */
void vio_a11y_free(struct _vio_input_state *in);

#endif /* VIO_A11Y_H */
