// Copyright 2026 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Exercise the actual Wayland callbacks and queue without a compositor or VM.
#include <math.h>
#include "../src/display_wl.c"

static unsigned errors;
static bool guest_keys[KEY_STATE_COUNT];
static bool guest_buttons[POINTER_BUTTON_COUNT];
static char surface_cookies[2];

static struct wl_surface *surface(size_t index)
{
	return (struct wl_surface *)&surface_cookies[index];
}

static void count_error(const char *message)
{
	(void)message;
	errors++;
}

static struct dwl_context *fresh(void)
{
	memset(guest_keys, 0, sizeof(guest_keys));
	memset(guest_buttons, 0, sizeof(guest_buttons));
	errors = 0;
	struct dwl_context *c = dwl_context_new(count_error);
	assert(c);
	c->input.keyboard_input_surface = surface(0);
	c->input.pointer_input_surface = surface(0);
	return c;
}

static void consume_one(struct dwl_context *c)
{
	struct dwl_event e;
	dwl_context_next_event(c, &e);
	if (e.event_type == DWL_EVENT_TYPE_KEYBOARD_KEY) {
		assert((uint32_t)e.params[0] < KEY_STATE_COUNT);
		guest_keys[e.params[0]] = e.params[1] != 0;
	} else if (e.event_type == DWL_EVENT_TYPE_POINTER_BUTTON) {
		size_t index = pointer_button_index((uint32_t)e.params[0]);
		assert(index < POINTER_BUTTON_COUNT);
		guest_buttons[index] = e.params[1] != 0;
	}
}

static unsigned drain(struct dwl_context *c)
{
	unsigned n = 0;
	while (dwl_context_pending_events(c)) {
		assert(++n <= EVENT_BUF_SIZE + KEY_STATE_COUNT + POINTER_BUTTON_COUNT + 1);
		consume_one(c);
	}
	assert(c->event_count == 0 && c->deferred_event_count == 0);
	return n;
}

static void assert_synchronized(struct dwl_context *c)
{
	for (size_t i = 0; i < KEY_STATE_COUNT; i++)
		assert(guest_keys[i] == c->input.keyboard_keys[i]);
	for (size_t i = 0; i < POINTER_BUTTON_COUNT; i++)
		assert(guest_buttons[i] == !!(c->input.pointer_buttons & (1u << i)));
}

static void fill_wheel(struct dwl_context *c, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		wl_pointer_axis_discrete(c, NULL, WL_POINTER_AXIS_VERTICAL_SCROLL, 1);
}

static void key(struct dwl_context *c, uint32_t code, uint32_t state)
{
	wl_keyboard_key(c, NULL, 0, 0, code, state);
}

static void test_focus_loss(void)
{
	struct dwl_context *c = fresh();
	key(c, 56, WL_KEYBOARD_KEY_STATE_PRESSED); // Left Alt
	key(c, 15, WL_KEYBOARD_KEY_STATE_PRESSED); // Tab
	drain(c);
	assert(guest_keys[56] && guest_keys[15]);
	wl_keyboard_leave(c, NULL, 0, surface(0));
	drain(c);
	assert_synchronized(c);
	assert(!guest_keys[56] && !guest_keys[15] && errors == 0);
	uint32_t held[] = {56};
	struct wl_array keys = {.size = sizeof(held), .data = held};
	wl_keyboard_enter(c, NULL, 0, surface(1), &keys);
	drain(c);
	assert(guest_keys[56]);
	key(c, 56, WL_KEYBOARD_KEY_STATE_RELEASED);
	drain(c);
	assert_synchronized(c);
	dwl_context_destroy(&c);
}

static void test_pointer_focus_loss(void)
{
	struct dwl_context *c = fresh();
	for (size_t i = 0; i < POINTER_BUTTON_COUNT; i++)
		pointer_button_handler(c, NULL, 0, 0, POINTER_BUTTONS[i], 1);
	drain(c);
	pointer_leave_handler(c, NULL, 0, surface(0));
	drain(c);
	assert_synchronized(c);
	assert(c->input.pointer_buttons == 0 && errors == 0);
	dwl_context_destroy(&c);
}

static void test_motion_and_wraparound(void)
{
	struct dwl_context *c = fresh();
	for (int i = 0; i < 4096; i++)
		pointer_motion_handler(c, NULL, 0, wl_fixed_from_int(i), wl_fixed_from_int(1));
	assert(c->event_count == 1 && drain(c) == 1 && errors == 0);
	fill_wheel(c, EVENT_BUF_SIZE);
	assert(c->event_count == EVENT_BUF_SIZE && dwl_context_pending_events(c));
	assert(drain(c) == EVENT_BUF_SIZE);
	fill_wheel(c, 2);
	assert(drain(c) == 2);
	dwl_context_destroy(&c);
}

static void test_full_queue_key_release(void)
{
	struct dwl_context *c = fresh();
	key(c, 56, 1);
	drain(c);
	fill_wheel(c, EVENT_BUF_SIZE);
	key(c, 56, 0);
	assert(c->deferred_event_count == 1);
	drain(c);
	wl_keyboard_leave(c, NULL, 0, surface(0));
	struct wl_array empty = {0};
	wl_keyboard_enter(c, NULL, 0, surface(0), &empty);
	drain(c);
	assert_synchronized(c);
	assert(!guest_keys[56] && errors == 1);
	dwl_context_destroy(&c);
}

static void test_nearly_full_queue_focus_loss(void)
{
	struct dwl_context *c = fresh();
	key(c, 56, 1);
	key(c, 15, 1);
	drain(c);
	fill_wheel(c, EVENT_BUF_SIZE - 1);
	wl_keyboard_leave(c, NULL, 0, surface(0));
	drain(c);
	assert_synchronized(c);
	assert(!guest_keys[56] && !guest_keys[15] && errors == 1);
	dwl_context_destroy(&c);
}

static void test_nearly_full_queue_button_release(void)
{
	for (size_t i = 0; i < POINTER_BUTTON_COUNT; i++) {
		struct dwl_context *c = fresh();
		pointer_button_handler(c, NULL, 0, 0, POINTER_BUTTONS[i], 1);
		drain(c);
		fill_wheel(c, EVENT_BUF_SIZE - 1);
		pointer_button_handler(c, NULL, 0, 0, POINTER_BUTTONS[i], 0);
		drain(c);
		pointer_leave_handler(c, NULL, 0, surface(0));
		drain(c);
		assert_synchronized(c);
		assert(!guest_buttons[i] && errors == 1);
		dwl_context_destroy(&c);
	}
}

static void test_deferred_transitions_cannot_be_overtaken(void)
{
	struct dwl_context *c = fresh();
	fill_wheel(c, EVENT_BUF_SIZE);
	key(c, 56, 1);
	consume_one(c); // A free FIFO slot must not let a later release overtake the press.
	key(c, 56, 0);
	assert(c->event_count == EVENT_BUF_SIZE - 1);
	assert(c->deferred_event_count == 1);
	drain(c);
	assert_synchronized(c);
	assert(!guest_keys[56]);
	dwl_context_destroy(&c);
}

static void test_extended_keys_and_maximum_state(void)
{
	struct dwl_context *c = fresh();
	key(c, 352, 1);
	drain(c);
	wl_keyboard_leave(c, NULL, 0, surface(0));
	drain(c);
	assert(!guest_keys[352]);
	c->input.keyboard_input_surface = surface(0);
	key(c, KEY_STATE_COUNT, 1);
	key(c, UINT32_MAX, 1);
	assert(!dwl_context_pending_events(c));
	for (uint32_t i = 0; i < KEY_STATE_COUNT; i++)
		key(c, i, 1);
	for (size_t i = 0; i < POINTER_BUTTON_COUNT; i++)
		pointer_button_handler(c, NULL, 0, 0, POINTER_BUTTONS[i], 1);
	wl_keyboard_leave(c, NULL, 0, surface(0));
	pointer_leave_handler(c, NULL, 0, surface(0));
	assert(c->event_count <= EVENT_BUF_SIZE);
	assert(c->deferred_event_count <= KEY_STATE_COUNT + POINTER_BUTTON_COUNT + 1);
	drain(c);
	assert_synchronized(c);
	dwl_context_destroy(&c);
}

static void test_surface_input_reset(void)
{
	struct dwl_context *c = fresh();
	key(c, 56, 1);
	pointer_button_handler(c, NULL, 0, 0, BTN_SIDE, 1);
	drain(c);
	fill_wheel(c, EVENT_BUF_SIZE);
	release_surface_input(c, surface(1)); // Unrelated surface must not release input.
	assert(c->input.keyboard_keys[56]);
	release_surface_input(c, surface(0)); // Same helper used by dwl_surface_destroy.
	assert(c->input.keyboard_input_surface == NULL);
	assert(c->input.pointer_input_surface == NULL);
	drain(c);
	assert_synchronized(c);
	assert(!guest_keys[56] && !guest_buttons[3]);
	uint32_t held[] = {56};
	struct wl_array keys = {.size = sizeof(held), .data = held};
	wl_keyboard_enter(c, NULL, 0, surface(1), &keys);
	drain(c);
	assert(guest_keys[56]);
	struct wl_array empty = {0};
	wl_keyboard_enter(c, NULL, 0, surface(1), &empty);
	drain(c);
	assert(!guest_keys[56]);
	dwl_context_destroy(&c);
}

static uint32_t rng_state = 0x12345678;
static uint32_t random_value(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static void test_bursts_with_interleaved_drain(void)
{
	struct dwl_context *c = fresh();
	for (unsigned round = 0; round < 100; round++) {
		fill_wheel(c, EVENT_BUF_SIZE);
		for (unsigned i = 0; i < 1000; i++) {
			uint32_t r = random_value();
			if (r & 1)
				key(c, (r >> 8) % KEY_STATE_COUNT, (r >> 1) & 1);
			else
				pointer_button_handler(c, NULL, 0, 0,
					POINTER_BUTTONS[(r >> 8) % POINTER_BUTTON_COUNT], (r >> 1) & 1);
			if ((r & 15) == 0 && dwl_context_pending_events(c))
				consume_one(c);
			assert(c->event_count <= EVENT_BUF_SIZE);
			assert(c->deferred_event_count <= KEY_STATE_COUNT + POINTER_BUTTON_COUNT + 1);
		}
		drain(c);
		assert_synchronized(c);
	}
	release_surface_input(c, surface(0));
	drain(c);
	assert_synchronized(c);
	dwl_context_destroy(&c);
}

int main(void)
{
#define RUN(test) do { test(); puts("PASS: " #test); } while (0)
	RUN(test_focus_loss);
	RUN(test_pointer_focus_loss);
	RUN(test_motion_and_wraparound);
	RUN(test_full_queue_key_release);
	RUN(test_nearly_full_queue_focus_loss);
	RUN(test_nearly_full_queue_button_release);
	RUN(test_deferred_transitions_cannot_be_overtaken);
	RUN(test_extended_keys_and_maximum_state);
	RUN(test_surface_input_reset);
	RUN(test_bursts_with_interleaved_drain);
	return 0;
}
