/*
 * Copyright 2019 Intel Corporation
 * Copyright 2022 Nuvoton Technology Corporation.
 * Copyright 2023 Google LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#define LOG_LEVEL CONFIG_INPUT_LOG_LEVEL
LOG_MODULE_REGISTER(input_kbd_matrix);

#include "input_kbd_matrix.h"

#define INPUT_KBD_MATRIX_ROW_MASK UINT8_MAX

void input_kbd_matrix_poll_start(const struct device *dev)
{
	struct input_kbd_matrix_common_data *data = dev->data;

	k_sem_give(&data->poll_lock);
}

static bool input_kbd_matrix_ghosting(const struct device *dev)
{
	const struct input_kbd_matrix_common_config *cfg = dev->config;
	const uint8_t *state = cfg->matrix_new_state;

	/*
	 * Matrix keyboard designs are suceptible to ghosting.
	 * An extra key appears to be pressed when 3 keys belonging to the same
	 * block are pressed. For example, in the following block:
	 *
	 * . . w . q .
	 * . . . . . .
	 * . . . . . .
	 * . . m . a .
	 *
	 * the key m would look as pressed if the user pressed keys w, q and a
	 * simultaneously. A block can also be formed, with not adjacent
	 * columns.
	 */
	for (int c = 0; c < cfg->col_size; c++) {
		if (!state[c]) {
			continue;
		}

		for (int c_next = c + 1; c_next < cfg->col_size; c_next++) {
			/*
			 * We AND the columns to detect a "block". This is an
			 * indication of ghosting, due to current flowing from
			 * a key which was never pressed. In our case, current
			 * flowing is a bit set to 1 as we flipped the bits
			 * when the matrix was scanned. Now we OR the colums
			 * using z&(z-1) which is non-zero only if z has more
			 * than one bit set.
			 */
			uint8_t common_row_bits = state[c] & state[c_next];

			if (common_row_bits & (common_row_bits - 1)) {
				return true;
			}
		}
	}

	return false;
}

static bool input_kbd_matrix_scan(const struct device *dev)
{
	const struct input_kbd_matrix_common_config *cfg = dev->config;
	const struct input_kbd_matrix_api *api = &cfg->api;
	int row;
	uint8_t key_event = 0U;

	for (int col = 0; col < cfg->col_size; col++) {
		api->drive_column(dev, col);

		/* Allow the matrix to stabilize before reading it */
		k_busy_wait(cfg->settle_time_us);

		row = api->read_row(dev) & INPUT_KBD_MATRIX_ROW_MASK;
		cfg->matrix_new_state[col] = row;
		key_event |= row;
	}

	api->drive_column(dev, INPUT_KBD_MATRIX_COLUMN_DRIVE_NONE);

	return key_event != 0U;
}

static void input_kbd_matrix_update_state(const struct device *dev)
{
	const struct input_kbd_matrix_common_config *cfg = dev->config;
	struct input_kbd_matrix_common_data *data = dev->data;
	uint8_t *matrix_new_state = cfg->matrix_new_state;
	uint32_t cycles_now = k_cycle_get_32();
	uint8_t row_changed;
	uint8_t deb_col;

	data->scan_clk_cycle[data->scan_cycles_idx] = cycles_now;

	/*
	 * The intent of this loop is to gather information related to key
	 * changes.
	 */
	for (int c = 0; c < cfg->col_size; c++) {
		/* Check if there was an update from the previous scan */
		row_changed = matrix_new_state[c] ^ cfg->matrix_previous_state[c];

		if (!row_changed) {
			continue;
		}

		for (int r = 0; r < cfg->row_size; r++) {
			uint8_t cyc_idx = c * cfg->row_size + r;

			/*
			 * Index all they keys that changed for each row in
			 * order to debounce each key in terms of it
			 */
			if (row_changed & BIT(r)) {
				cfg->scan_cycle_idx[cyc_idx] = data->scan_cycles_idx;
			}
		}

		cfg->matrix_unstable_state[c] |= row_changed;
		cfg->matrix_previous_state[c] = matrix_new_state[c];
	}

	for (int c = 0; c < cfg->col_size; c++) {
		deb_col = cfg->matrix_unstable_state[c];

		if (!deb_col) {
			continue;
		}

		/* Debouncing for each row key occurs here */
		for (int r = 0; r < cfg->row_size; r++) {
			uint8_t mask = BIT(r);
			uint8_t row_bit = matrix_new_state[c] & mask;

			/* Continue if we already debounce a key */
			if (!(deb_col & mask)) {
				continue;
			}

			uint8_t cyc_idx = c * cfg->row_size + r;
			uint8_t scan_cyc_idx = cfg->scan_cycle_idx[cyc_idx];
			uint8_t scan_clk_cycle = data->scan_clk_cycle[scan_cyc_idx];

			/* Convert the clock cycle differences to usec */
			uint32_t debt = k_cyc_to_us_floor32(cycles_now - scan_clk_cycle);

			/* Does the key requires more time to be debounced? */
			if (debt < (row_bit ? cfg->debounce_down_ms : cfg->debounce_up_ms)) {
				/* Need more time to debounce */
				continue;
			}

			cfg->matrix_unstable_state[c] &= ~row_bit;

			/* Check if there was a change in the stable state */
			if ((cfg->matrix_stable_state[c] & mask) == row_bit) {
				/* Key state did not change */
				continue;
			}

			/*
			 * The current row has been debounced, therefore update
			 * the stable state. Then, proceed to notify the
			 * application about the keys pressed.
			 */
			cfg->matrix_stable_state[c] ^= mask;

			input_report_abs(dev, INPUT_ABS_X, c, false, K_FOREVER);
			input_report_abs(dev, INPUT_ABS_Y, r, false, K_FOREVER);
			input_report_key(dev, INPUT_BTN_TOUCH, row_bit, true, K_FOREVER);
		}
	}
}

static bool input_kbd_matrix_check_key_events(const struct device *dev)
{
	const struct input_kbd_matrix_common_config *cfg = dev->config;
	struct input_kbd_matrix_common_data *data = dev->data;
	bool key_pressed;

	if (++data->scan_cycles_idx >= INPUT_KBD_MATRIX_SCAN_OCURRENCES) {
		data->scan_cycles_idx = 0U;
	}

	/* Scan the matrix */
	key_pressed = input_kbd_matrix_scan(dev);

	for (int c = 0; c < cfg->col_size; c++) {
		LOG_DBG("U%x, P%x, N%x",
			cfg->matrix_unstable_state[c],
			cfg->matrix_previous_state[c],
			cfg->matrix_new_state[c]);
	}

	/* Abort if ghosting is detected */
	if (cfg->ghostkey_check && input_kbd_matrix_ghosting(dev)) {
		return key_pressed;
	}

	input_kbd_matrix_update_state(dev);

	return key_pressed;
}

static void input_kbd_matrix_poll(const struct device *dev)
{
	const struct input_kbd_matrix_common_config *cfg = dev->config;
	k_timepoint_t poll_time_end = sys_timepoint_calc(K_MSEC(cfg->poll_timeout_ms));
	uint32_t current_cycles;
	uint32_t cycles_diff;
	uint32_t wait_period_us;

	while (true) {
		uint32_t start_period_cycles = k_cycle_get_32();

		if (input_kbd_matrix_check_key_events(dev)) {
			poll_time_end = sys_timepoint_calc(K_MSEC(cfg->poll_timeout_ms));
		} else if (sys_timepoint_expired(poll_time_end)) {
			break;
		}

		/*
		 * Subtract the time invested from the sleep period in order to
		 * compensate for the time invested in debouncing a key
		 */
		current_cycles = k_cycle_get_32();
		cycles_diff = current_cycles - start_period_cycles;
		wait_period_us = cfg->poll_period_us - k_cyc_to_us_floor32(cycles_diff);

		/* Wait for at least 1ms */
		if (wait_period_us < USEC_PER_MSEC) {
			wait_period_us = USEC_PER_MSEC;
		}

		/*
		 * Wait period results in a larger number when current cycles
		 * counter wrap. In this case, the whole poll period is used
		 */
		if (wait_period_us > cfg->poll_period_us) {
			LOG_DBG("wait_period_us: %u", wait_period_us);

			wait_period_us = cfg->poll_period_us;
		}

		/* Allow other threads to run while we sleep */
		k_usleep(wait_period_us);
	}
}

static void input_kbd_matrix_polling_thread(void *arg1, void *unused2, void *unused3)
{
	const struct device *dev = arg1;
	const struct input_kbd_matrix_common_config *cfg = dev->config;
	const struct input_kbd_matrix_api *api = &cfg->api;
	struct input_kbd_matrix_common_data *data = dev->data;

	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	while (true) {
		api->drive_column(dev, INPUT_KBD_MATRIX_COLUMN_DRIVE_ALL);
		api->set_detect_mode(dev, true);

		k_sem_take(&data->poll_lock, K_FOREVER);
		LOG_DBG("Start KB scan");

		/* Disable interrupt of KSI pins and start polling */
		api->set_detect_mode(dev, false);

		input_kbd_matrix_poll(dev);
	}
}

int input_kbd_matrix_common_init(const struct device *dev)
{
	struct input_kbd_matrix_common_data *data = dev->data;

	k_sem_init(&data->poll_lock, 0, 1);

	k_thread_create(&data->thread, data->thread_stack,
			CONFIG_INPUT_KBD_MATRIX_THREAD_STACK_SIZE,
			input_kbd_matrix_polling_thread, (void *)dev, NULL, NULL,
			K_PRIO_COOP(4), 0, K_NO_WAIT);

	k_thread_name_set(&data->thread, dev->name);

	return 0;
}
