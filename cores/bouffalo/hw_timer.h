#pragma once

/*
 * Ownership of the BL616CL general-purpose timers TIMER0 and TIMER1, shared by the Core's tone()
 * and the BLTimer library. Implemented in Tone.cpp. Call from task context, one task at a time.
 */
struct bflb_device_s;

/**
 * @brief Claim a free timer, TIMER0 first. A timed tone that has already ended gives its timer
 * back first. The timer number is the device's idx.
 * @return the timer device, or nullptr when both timers are held.
 */
struct bflb_device_s *hw_timer_claim(void);

/**
 * @brief Return a timer obtained from hw_timer_claim(); the caller has stopped it.
 */
void hw_timer_release(struct bflb_device_s *timer);
