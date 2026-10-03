// SPDX-License-Identifier: GPL-2.0
/*
 * NebulaOS backlight final controller - kernel-owned, exclusive owner of
 * GPC0 (as GPIO), GPC0/PWM-channel-0 (as PWM), and the candidate
 * enable-GPIO PC22 (DISPLAY-B-FINAL, 2026-08-02+)
 *
 * THIS DRIVER EXISTS BECAUSE OF A REAL LIVE INCIDENT, NOT A HYPOTHETICAL
 * ONE. On 2026-08-01/02 a compile-only diagnostic
 * (CONFIG_NEBULAOS_BACKLIGHT_PROBE_DIAG, nebulaos_backlight_probe_diag.c)
 * was flashed to the real printer with its DT node's PWM channel 0 wired
 * up, which required changing the SHARED &pwm controller node's own
 * pinctrl-0 from <&pwm1_pc> to <&pwm0_pc> (later <&pwm0_pc &pwm1_pc>) so
 * PWM channel 0 would physically reach GPC0 (global GPIO 64, this board's
 * "backlight_pwm0" pin). The screen went dark from boot and stayed dark.
 * Root cause, confirmed via live investigation on the real device:
 *
 *   1. A device's pinctrl-names="default"/pinctrl-0 property is applied
 *      AUTOMATICALLY by the driver core, before that device's own probe()
 *      even runs - see drivers/base/dd.c really_probe() ->
 *      pinctrl_bind_pins() (drivers/base/pinctrl.c): it calls
 *      devm_pinctrl_get(dev), looks up the state named PINCTRL_STATE_DEFAULT
 *      ("default"), and pinctrl_select_state()s it unconditionally if
 *      found - entirely independent of whether the driver's own C code
 *      ever calls a single pinctrl API itself. pwm-ingenic-v2.c contains
 *      ZERO pinctrl calls (grep confirms this) - the &pwm node's own
 *      pinctrl-0 = <&pwm0_pc> was doing 100% of the work automatically,
 *      at early boot, regardless of whether any consumer ever actually
 *      requested channel 0.
 *   2. Before that edit, NOTHING in the kernel device tree ever touched
 *      GPC0 at all - it was left exactly as u-boot configured it, almost
 *      certainly a GPIO output driven HIGH (this is what was keeping the
 *      backlight on across every previously-qualified image).
 *   3. The instant &pwm's own default pinctrl state claimed GPC0 for the
 *      PWM peripheral function, that bootloader state was destroyed - and
 *      since channel 0 was never actually pwm_apply_state()'d enabled,
 *      the peripheral's inactive-channel output level is low, so the
 *      backlight went dark.
 *   4. Live-tested and PROVEN: manually forcing GPC0 to a GPIO output
 *      driven HIGH (via legacy /sys/class/gpio - export/direction=out/
 *      value=1) immediately restored the screen. This is the proven-safe
 *      restoration target this driver always converges on. That legacy
 *      sysfs path is explicitly forbidden here (and everywhere else in
 *      this driver/its tooling) because it ALSO triggered a real kernel
 *      warning ("pinctrl-ingenic.c: gpio functions has redefinition",
 *      export_store -> gpiod_request -> gpiod_request_commit ->
 *      ingenic_gpio_request) from GPC0 being claimed through two
 *      independent, uncoordinated paths at once (the PWM controller's own
 *      formal pinctrl mux claim, and a second, conflicting gpiolib
 *      request via legacy sysfs) - corrupting pinctrl-ingenic's internal
 *      bookkeeping and causing a later, unrelated PC22 probe to surface a
 *      fault. See ingenic_gpio_request()'s jzgc->used_pins_bitmap check
 *      in module_drivers/drivers/pinctrl/pinctrl-ingenic.c for exactly
 *      what that warning detects.
 *
 * DESIGN, DIRECTLY TARGETING EVERY ROOT CAUSE ABOVE:
 *
 *   (a) probe() below NEVER calls devm_gpiod_get()/gpiod_get()/
 *       devm_pwm_get()/pwm_get()/pinctrl_select_state() for GPC0, PC22,
 *       or PWM channel 0 - not even once, not even read-only. Those three
 *       resources are acquired ONLY inside the explicit state-transition
 *       functions below (nblc_enter_safe_on_locked(),
 *       nblc_enter_pwm_active_locked(), nblc_pc22_test_locked()), never
 *       from probe(), and never automatically. This is the single most
 *       important rule in this file - do not add a resource acquisition
 *       call anywhere else.
 *
 *   (b) The shared &pwm controller node's own pinctrl-0 is NEVER touched
 *       by this driver's patch or toggle script - verified by a dedicated
 *       test (tests/backlight-final-controller-variant-tests.sh) that
 *       greps for the exact pristine line and asserts it is byte-
 *       identical before and after the toggle script applies/reverts.
 *       Instead, THIS driver's own DT node carries its own
 *       pinctrl-names/pinctrl-0 pointing at the SAME <&pwm0_pc> group
 *       (see scripts/build/backlight-final-controller-variant.sh), but
 *       under a state named "pwm-active" - deliberately NOT "default" or
 *       "init" (PINCTRL_STATE_DEFAULT/PINCTRL_STATE_INIT), the only two
 *       names pinctrl_bind_pins() ever auto-selects. Because of that,
 *       pinctrl_bind_pins() still runs automatically before THIS driver's
 *       own probe() too (every platform device gets this), but
 *       pinctrl_lookup_state(p, "default") fails to find "pwm-active", so
 *       it does nothing but build-then-immediately-release a pinctrl
 *       handle (see drivers/base/pinctrl.c pinctrl_bind_pins()'s
 *       cleanup_get: path) - a genuine no-op with zero hardware effect.
 *       The "pwm-active" state is selected ONLY by this driver's own code,
 *       ONLY inside nblc_enter_pwm_active_locked(), ONLY when an explicit
 *       "pwm-active-25/50/75" debugfs command is accepted from safe-on.
 *
 *   (c) Exiting pwm-active always converges back to GPIO-high via
 *       gpiod_get(dev, "backlight", GPIOD_OUT_HIGH) - which, on this
 *       board's pinctrl-ingenic.c, UNCONDITIONALLY reprograms the pin's
 *       function to GPIO_OUTPUT0 regardless of whatever peripheral
 *       function it was previously muxed to: ingenic_gpio_direction_output()
 *       -> pinctrl_gpio_direction_output() -> pinmux_ops.gpio_set_direction
 *       == ingenic_pinmux_gpio_set_dir() -> ingenic_gpio_set_func(), which
 *       writes the function-select register directly, with no dependency
 *       on the pin's current/previous mux_setting bookkeeping. This is the
 *       EXACT mechanism that live-restored the display during tonight's
 *       incident (the legacy sysfs "direction=out" write goes through this
 *       identical code path) - this driver just does it through the real
 *       gpiod consumer API instead of forbidden sysfs.
 *
 *       This does NOT leave stale pinctrl-core mux bookkeeping behind
 *       either: this driver calls pinctrl_put() on the "pwm-active"
 *       pinctrl handle as part of every exit from pwm-active, BEFORE
 *       re-acquiring GPC0 as a GPIO. pinctrl_put() -> pinctrl_release() ->
 *       pinctrl_free() calls pinctrl_free_setting(true, setting) for
 *       whatever state is currently active on that handle, which for a
 *       PIN_MAP_TYPE_MUX_GROUP setting calls pinmux_disable_setting() ->
 *       pin_free() for every pin in the group - cleanly clearing GPC0's
 *       mux_owner/mux_usecount in the generic pinctrl core (see
 *       drivers/pinctrl/core.c pinctrl_free_setting()/pinctrl_free(), and
 *       drivers/pinctrl/pinmux.c pinmux_disable_setting()/pin_free()).
 *       So the release is genuinely symmetric and leak-free - not merely
 *       "hardware ends up correct despite stale bookkeeping".
 *
 *   (d) Entering pwm-active always releases the GPIO claim on GPC0
 *       (gpiod_put()) BEFORE selecting the "pwm-active" pinctrl state, and
 *       exiting always disables+releases the PWM channel and releases the
 *       pinctrl claim BEFORE re-acquiring the GPIO claim - so GPC0 is
 *       never simultaneously claimed through two different subsystems at
 *       once (the exact "owned two ways" condition that corrupted
 *       pinctrl-ingenic's bookkeeping in the real incident, see (4) above,
 *       though there specifically between a formal PWM claim and a
 *       LEGACY SYSFS claim - a combination that cannot occur here at all
 *       because this driver never uses sysfs and is GPC0's sole consumer).
 *
 *   (e) Never uses legacy /sys/class/gpio sysfs anywhere - only the real
 *       gpiod_get()/gpiod_put()/gpiod_direction_output()/
 *       gpiod_get_value() consumer API, matching how
 *       nebulaos_backlight_probe_diag.c already did its own GPIO access
 *       correctly (that part of its design was fine; its eager
 *       probe()-time acquisition combined with the shared &pwm node's
 *       pinctrl-0 edit is what actually caused the incident, not gpiod
 *       usage itself).
 *
 *   (f) This driver is the DT-declared exclusive consumer of all three
 *       resources - no other node in halley5_v30.dts references GPC0,
 *       PC22, or PWM channel 0 (verified: the &pwm node's own pinctrl-0
 *       stays untouched per (b), and grep confirms no other consumer of
 *       "gpc 0" / "gpc 22" exists in the tree this patch ships against).
 *       Every operation is serialized through a single mutex (nblc.lock);
 *       only one state transition/bounded test may be in flight at a
 *       time (nblc.active_op != NBLC_OP_NONE rejects a second one with
 *       -EBUSY).
 *
 *   (g) The proven-safe restoration target is GPC0 = GPIO output HIGH -
 *       never a PWM-derived state, never "whatever it happened to be
 *       before" (there is no meaningful "before" for a PWM channel that
 *       was never previously active - pwm-ingenic-v2.c's cached
 *       pwm_device state for an unused channel is a synthetic zero value,
 *       not a real hardware readback, unless CONFIG_PWM_INGENIC_V2_GET_STATE
 *       is selected AND even then that only tells you the CURRENT state,
 *       not a trustworthy prior one). Every restore path in this file -
 *       explicit "restore"/"disarm", the watchdog timeout, an error return
 *       from any kernel API called while entering pwm-active/safe-off-test,
 *       and driver remove() - converges on the identical
 *       nblc_converge_gpc0_safe_on_locked() routine, which re-verifies the
 *       readback (gpiod_get_value()) after driving high rather than
 *       assuming the write succeeded, and surfaces the honest result via
 *       nblc.safe_on_verified / nblc.restore_failure_count /
 *       nblc.last_restore_reason.
 *
 * STATE MACHINE (Phase 9):
 *
 *   boot-preserve   - automatic at probe(), and the ONLY state reachable
 *                     without an explicit transition. Zero claim on GPC0,
 *                     PC22, or PWM0. Bootloader's own GPC0 configuration
 *                     is left completely untouched. Reachable again later
 *                     only via the explicit "disarm" command, which first
 *                     forces a full safe-on convergence (see (g) above)
 *                     and then releases the GPIO claim - so "returning to
 *                     boot-preserve" after having actively driven the pin
 *                     does NOT claim to restore the bootloader's literal
 *                     original value (that information no longer exists
 *                     once this driver has driven the pin itself); it
 *                     honestly means "this driver is no longer actively
 *                     managing the pin", left at the last known-safe
 *                     (HIGH) level.
 *   safe-on         - GPC0 acquired as GPIO, driven HIGH, readback
 *                     verified. Every other active state's every exit
 *                     path (normal completion, timeout, error, explicit
 *                     disarm) converges back here.
 *   safe-off-test   - only reachable FROM safe-on. GPC0 driven LOW for a
 *                     bounded duration (default 1000ms, module param
 *                     safe_off_test_ms, hard-capped at
 *                     NBLC_WATCHDOG_MAX_MS regardless of the param value).
 *   pwm-active      - only reachable FROM safe-on (never directly from
 *                     boot-preserve or safe-off-test - enforced by
 *                     nblc_enter_pwm_active_locked()'s state check).
 *                     Releases the GPIO claim, selects the "pwm-active"
 *                     pinctrl state (muxes GPC0 to the PWM peripheral
 *                     function), acquires PWM channel 0, and applies a
 *                     fixed 25/50/75% duty at NBLC_PWM_PERIOD_NS (20000ns,
 *                     ~50kHz, matching stock's own pwm_backlight.sh
 *                     pwm_freq=50000 and the DISPLAY-B0-DIAG candidate).
 *                     Every exit (normal, timeout, error, disarm) disables
 *                     the PWM, releases the PWM channel, releases the
 *                     pinctrl claim (remuxing GPC0 back to GPIO function
 *                     per (c) above), and converges to safe-on - always,
 *                     never "restore to whatever it was before".
 *   pwm-committed   - only reachable FROM an in-flight pwm-active bounded
 *                     test (never directly from safe-on or any other
 *                     state - enforced by nblc_cmd_commit_pwm()'s state
 *                     AND active_op check), via the explicit "commit-pwm"
 *                     debugfs command. Every commit is therefore forced
 *                     through the exact same acquire/apply/verify sequence
 *                     (nblc_enter_pwm_active_locked()) the bounded
 *                     pwm-active path already uses - there is no, and will
 *                     never be, a direct safe-on -> pwm-committed
 *                     shortcut. Touches no hardware itself: n->pwm and
 *                     n->pwm_pinctrl are left exactly as the in-flight
 *                     pwm-active test left them, so committing causes no
 *                     visible flicker. See "PWM COMMITTED (Phase 13)"
 *                     below for the full design.
 *   asleep          - only reachable FROM safe-on or pwm-committed, via the
 *                     explicit "sleep" debugfs command (nblc_cmd_sleep()).
 *                     GPC0 held low (the proven safe-off-test mechanism,
 *                     just without the auto-revert) - never a PWM-duty-
 *                     based off. No automatic timeout; persists until an
 *                     explicit "wake". See "SLEEP (Phase 14)" below for the
 *                     full design.
 *
 * PC22 (Phase 10): accessed only through this same driver, same lazy/
 * deferred acquisition discipline, only via pc22-test-low/pc22-test-high
 * debugfs commands (drive low/high for a fixed 1000ms then restore to the
 * captured initial level - nothing else is exposed). The initial level is
 * captured exactly once, the first time PC22 is ever accessed, via
 * gpiod_get(dev, "enable", GPIOD_ASIS) (a non-destructive claim - GPIOD_ASIS
 * never changes direction/value) immediately followed by
 * gpiod_get_raw_value_cansleep() - a genuine live hardware readback, since
 * ingenic_gpio_get() reads the PxPIN register unconditionally on every
 * call (same fact nebulaos_backlight_probe_diag.c's own header already
 * documents for this same GPIO chip). The initial DIRECTION is honestly
 * reported as unknown/unavailable: struct ingenic_gpiolib_chip (see
 * pinctrl-ingenic.c's ingenic_gpiolib_chip definition) implements no
 * .get_direction callback at all, so gpiod_get_direction() unconditionally
 * returns -ENOTSUPP for every GPIO on this chip, regardless of claim
 * timing - this is not a "couldn't capture before claiming" limitation,
 * the underlying platform genuinely cannot report it either way. This
 * driver does not invent a direction value to fill that gap. If gpiod_get()
 * returns -EBUSY (or any other error) because PC22 is already owned by
 * another driver, the test is refused with that real error code and a
 * clear dev_warn() - never silently ignored, never crashed into.
 *
 * WATCHDOG (Phase 11): a struct delayed_work, armed via
 * schedule_delayed_work() BEFORE any hardware mutation begins for a given
 * operation (identical ordering principle to
 * nebulaos_backlight_probe_diag.c's own ARM-BEFORE-APPLY discipline - see
 * that file's header for the full rationale: a crash between "arm" and
 * "apply" only ever races a harmless not-yet-applied capture). Every
 * bounded operation is hard-capped at NBLC_WATCHDOG_MAX_MS (2000ms,
 * compile-time enforced via static_assert against every operation's
 * fixed/clamped duration below). The watchdog callback
 * (nblc_restore_work()) runs on the kernel's own workqueue, entirely
 * independent of any userspace process's lifetime or an active SSH
 * session - it converges GPC0 to safe-on (or PC22 to its captured level,
 * for a PC22 test) regardless of what triggered it. Restore is triggered
 * by: watchdog timeout, normal operation completion, any error return from
 * a kernel API this file calls while entering an active state, the
 * explicit "restore"/"disarm" commands, and driver remove(). Every one of
 * these calls the SAME nblc_converge_gpc0_safe_on_locked() (or the PC22
 * equivalent) - there is exactly one restore implementation, not one per
 * caller. A restore that itself fails (the post-drive readback does not
 * confirm HIGH) is a hard-stop condition: nblc.safe_on_verified is cleared,
 * nblc.restore_failure_count is incremented, and nblc.last_restore_reason
 * records why - surfaced via the status debugfs file, never silently
 * assumed successful.
 *
 * PWM (Phase 12): candidate settings are pwmchip0 channel 0 (GPC0), period
 * NBLC_PWM_PERIOD_NS = 20000ns (50kHz), duty 25/50/75% only - 0% and 100%
 * are deliberately unreachable through this interface (same rationale as
 * nebulaos_backlight_probe_diag.c: an unproven polarity/wiring assumption
 * must never be able to drive the candidate fully on or fully off through
 * this controller). When CONFIG_PWM_INGENIC_V2_GET_STATE is selected, this
 * driver makes a single best-effort, non-gating .get_state readback right
 * after applying the target duty, purely to log what the hardware reports
 * back for cross-checking during live qualification - it never gates
 * entry/exit on that readback and never assumes any particular answer
 * from it, because live qualification must always deterministically
 * terminate at safe-on regardless (there is no meaningful prior state to
 * "restore to" for a channel this driver only ever drives forward from
 * safe-on, per (g) above).
 *
 * SLEEP (Phase 14): backlight-only sleep/wake, added on top of the Phase 13
 * design above. A live investigation this project ran found NO proven PWM
 * "off" duty value (0% duty was deliberately never tested, per this
 * project's own "don't guess unproven values, don't test extremes first"
 * convention - see "PWM (Phase 12)" above) - but DID prove GPC0-GPIO-low as
 * a genuine, live-tested "screen off" mechanism (safe-off-test, tested
 * twice, both clean). So "sleep" here means: hold GPC0 low indefinitely -
 * the same physical action safe-off-test already performs, just without the
 * auto-revert - never a PWM-duty-based off. This is a deliberate
 * substitution for an unproven value, not a guess.
 *
 *   - NBLC_STATE_ASLEEP is reachable ONLY via the "sleep" debugfs command,
 *     and ONLY from NBLC_STATE_SAFE_ON or NBLC_STATE_PWM_COMMITTED
 *     (nblc_cmd_sleep()) - both represent "the backlight is genuinely on
 *     and stable", so sleeping from either is meaningful. There is no path
 *     from boot-preserve, safe-off-test, or a bare in-flight pwm-active
 *     bounded test - none of those are stable "on" states to begin with.
 *
 *   - "sleep" first captures the wake target BEFORE touching any hardware:
 *     plain safe-on, or pwm-committed at the exact duty percentage
 *     currently held (n->pwm_duty_pct) - see n->sleep_wake_target_is_pwm/
 *     n->sleep_wake_target_duty_pct. If entering from pwm-committed, it
 *     then releases the PWM claim and remuxes GPC0 back to GPIO via
 *     nblc_converge_gpc0_safe_on_locked() - the EXACT SAME routine every
 *     other exit from an active PWM state already uses (file header item
 *     (c)) - never a second, parallel "release PWM, remux to GPIO"
 *     implementation. Only once GPC0 is genuinely safe-on (verified) does
 *     it drive GPC0 low, via nblc_drive_gpc0_low_locked() - the same
 *     GPIO-low mechanic safe-off-test itself uses, factored out into a
 *     single shared helper rather than duplicated.
 *
 *   - unlike every other bounded operation in this file, sleep's own
 *     hardware transition is watchdog-armed (schedule_delayed_work() before
 *     the mutation, same ordering principle as everywhere else) but the
 *     RESULTING state has no auto-revert: once the transition into
 *     NBLC_STATE_ASLEEP completes successfully, the transition-bounding
 *     work is cancelled (cancel_delayed_work(), same non-sync in-lock
 *     pattern commit-pwm already uses to disarm its own bounding timer) -
 *     asleep persists until an explicit "wake", exactly like pwm-committed
 *     persists until an explicit "restore"/"enter-safe-on". If the
 *     transition itself hangs or crashes before completing, the armed
 *     watchdog still converges to safe-on on timeout, same as any other
 *     operation.
 *
 *   - "wake" (nblc_cmd_wake()) is reachable ONLY from NBLC_STATE_ASLEEP. It
 *     first drives GPC0 high and verifies, via nblc_converge_gpc0_safe_on_locked()
 *     - the identical universal convergence-to-safe-on routine every other
 *     transition into safe-on already uses (GPC0's GPIO claim was never
 *     released by sleep, only driven low, so this takes the "already held"
 *     branch and re-drives it). If the saved wake target was plain safe-on,
 *     wake is complete at this point. If the saved target was
 *     pwm-committed at duty X%, wake then calls
 *     nblc_enter_pwm_active_locked(n, X) - the SAME acquire/apply/verify
 *     sequence "pwm-active-X" itself uses, never a shortcut around
 *     pwm_apply_state()/readback - followed immediately by
 *     nblc_commit_pwm_locked(), the same bookkeeping-only commit transition
 *     "commit-pwm" itself performs (no hardware touched a second time).
 *     The end result is indistinguishable from having issued
 *     "pwm-active-X" then "commit-pwm" fresh, just automated as one atomic
 *     operation - it goes through the SAME real hardware-apply function
 *     calls those two commands use individually, not a shortcut that pokes
 *     state directly.
 *
 *   - failure anywhere in either direction (GPIO write fails, PWM apply
 *     fails, a readback mismatch) converges through the SAME
 *     nblc_converge_gpc0_safe_on_locked()/NBLC_STATE_FAULT discipline every
 *     other failure path in this file already uses - not a new, parallel
 *     error-handling path. Sleep/wake failures increment
 *     n->op_failure_count/n->wake_failure_count and record
 *     n->last_restore_reason exactly like every other convergence failure.
 *
 *   - NBLC_STATE_ASLEEP never holds n->pwm/n->pwm_pinctrl (both are always
 *     released, via nblc_converge_gpc0_safe_on_locked(), before the state
 *     becomes NBLC_STATE_ASLEEP - regardless of whether sleep was entered
 *     from safe-on, which never held them, or pwm-committed, which had them
 *     released as part of the sleep transition itself). Only n->gpc0_gpio
 *     remains held (driven low). This means every existing convergence/
 *     shutdown/disarm path that already handles "GPC0 held as GPIO, state
 *     is something other than safe-on/boot-preserve" - nblc_force_restore_now()'s
 *     generic active_op-idle fallback, nblc_cmd_enter_safe_on()'s
 *     unconditional converge call, and nblc_remove()'s unconditional
 *     non-boot-preserve converge - already handle NBLC_STATE_ASLEEP
 *     correctly with ZERO special-casing, the exact same way they already
 *     handle NBLC_STATE_PWM_COMMITTED (see "PWM COMMITTED (Phase 13)"
 *     above for why that generic invariant holds).
 *
 *   - status exposes: asleep (bool, via state), wake_target ("safe-on" or
 *     "pwm-committed-X"), asleep_duration_ms (a simple monotonic counter
 *     against n->asleep_since, same style as pwm_committed_duration_ms),
 *     sleep_count, wake_count, wake_failure_count.
 *
 *   - explicitly does NOT touch: framebuffer blanking, panel reset, DPU/
 *     panel powerdown, or PC22 in any way - sleep/wake only ever drive
 *     GPC0/PWM0, nothing else. Touch-wake (the userspace-polling half of
 *     this feature) is a separate piece of work built on top of this
 *     debugfs interface, not part of this driver.
 *
 * PWM COMMITTED (Phase 13): the bounded pwm-active test above proves
 * whether a given duty cycle is safe to drive at all - it is deliberately
 * incapable of holding that duty cycle indefinitely, always converging back
 * to safe-on within NBLC_PWM_TEST_MS. Day-to-day operation, once a duty has
 * been proven safe, needs the opposite: a SUSTAINED hold with no automatic
 * timeout. NBLC_STATE_PWM_COMMITTED provides exactly that, gated behind one
 * new rule and nothing else:
 *
 *   - reachable ONLY via the "commit-pwm" debugfs command, and ONLY while
 *     n->state == NBLC_STATE_PWM_ACTIVE *and* n->active_op ==
 *     NBLC_OP_PWM_ACTIVE - i.e. only while a pwm-active bounded test is
 *     genuinely still in flight (nblc_cmd_commit_pwm()). There is no path
 *     from safe-on, boot-preserve, safe-off-test, or a PC22 test directly
 *     into pwm-committed.
 *
 *   - on commit: cancel_delayed_work() disarms the in-flight test's fixed
 *     ~2s auto-revert timer (same in-lock, non-sync cancellation already
 *     used by every other command's own error/transition path in this
 *     file - see the n->lock comment on cancel_delayed_work_sync()), then
 *     n->active_op is cleared to NBLC_OP_NONE and n->state becomes
 *     NBLC_STATE_PWM_COMMITTED. No hardware is touched - n->pwm and
 *     n->pwm_pinctrl are left exactly as nblc_enter_pwm_active_locked()
 *     left them.
 *
 *   - DELIBERATELY no second, periodic watchdog is armed for the committed
 *     state. This is a considered choice, not an oversight: every
 *     convergence path already in this file - nblc_force_restore_now()
 *     (used by both "restore" and, via nblc_cmd_enter_safe_on(), by
 *     "enter-safe-on"/"disarm"), and nblc_remove() - forces a full
 *     nblc_converge_gpc0_safe_on_locked() convergence whenever
 *     n->active_op == NBLC_OP_NONE and n->state is anything other than
 *     NBLC_STATE_SAFE_ON/NBLC_STATE_BOOT_PRESERVE. NBLC_STATE_PWM_COMMITTED
 *     satisfies that condition unconditionally, so those already-tested
 *     generic paths converge it correctly with ZERO changes to their own
 *     code. That existing invariant guard - "active_op idle and state not
 *     already safe/boot-preserve means something must be converged" - IS
 *     this state's safety net; there is no way to leave this driver idle
 *     while pwm-committed without passing through one of those four
 *     convergence entry points (explicit "restore", explicit
 *     "enter-safe-on", explicit "disarm", or remove()) first. Adding a
 *     second, parallel periodic re-affirmation timer on top would duplicate
 *     a guarantee that already unconditionally holds, while introducing new
 *     ordering/interaction risk against the same lock/cancel discipline
 *     this driver's entire safety case rests on - not a trade worth making
 *     for a printer with a single remaining flash budget. If a future need
 *     for genuine periodic hardware re-verification (e.g. a get_state
 *     cross-check, matching the non-gating readback already described in
 *     "PWM (Phase 12)") arises, it should be added as its own diagnostic,
 *     non-gating log line, never as something the safety guarantee itself
 *     depends on.
 *
 *   - status (both debugfs status file and the dev_info command log)
 *     reports state as "pwm-committed" (nblc_state_name()), distinct from
 *     "pwm-active", plus pwm_committed / pwm_committed_duration_ms fields
 *     so a caller can tell a bounded, will-auto-revert test apart from a
 *     sustained, operator-confirmed hold, and see how long the current
 *     commitment has been held (jiffies delta against n->committed_since,
 *     a simple monotonic counter, no persistence needed).
 *
 *   - serialization is unchanged: since pwm-committed is not
 *     NBLC_STATE_SAFE_ON, the existing "only valid from safe-on" checks in
 *     nblc_cmd_safe_off_test()/nblc_cmd_pwm_active() already reject a new
 *     safe-off-test or a new pwm-active test while committed, with no
 *     changes to those functions. commit-pwm itself requires
 *     NBLC_STATE_PWM_ACTIVE exactly, so a second "commit-pwm" issued while
 *     already committed is likewise rejected (-EPERM) by the same check -
 *     no separate "already committed" branch needed.
 *
 *   - committing to a DIFFERENT duty cycle without first returning to
 *     safe-on is deliberately NOT supported. Doing so cleanly would require
 *     either a second concurrent PWM-apply path, or reusing
 *     nblc_enter_pwm_active_locked() from pwm-committed with new
 *     "currently committed, not currently in a bounded test" bookkeeping -
 *     both add real state-machine surface area for a convenience feature.
 *     The safe, already-tested way to change duty is: "restore" or
 *     "enter-safe-on" (converges cleanly to safe-on, releasing the PWM
 *     claim), then a fresh "pwm-active-25/50/75", then "commit-pwm" again.
 *
 * Bounded debugfs command interface (write to .../command, zero arguments,
 * any trailing token rejected outright - same whitelist-enforcement
 * discipline as nebulaos_backlight_probe_diag.c):
 *
 *   status              read-only, also dumped via dev_info()
 *   enter-safe-on       converge to safe-on from ANY state (the universal
 *                       safe fallback - always allowed)
 *   safe-off-test       bounded GPC0-low test, only from safe-on
 *   pc22-test-low       bounded PC22-low test (independent of GPC0 state)
 *   pc22-test-high      bounded PC22-high test
 *   pwm-active-25       enter pwm-active at 25% duty, only from safe-on
 *   pwm-active-50       enter pwm-active at 50% duty, only from safe-on
 *   pwm-active-75       enter pwm-active at 75% duty, only from safe-on
 *   commit-pwm          promote an in-flight pwm-active test to
 *                       pwm-committed (sustained, no auto-revert), only
 *                       while that test is still active - see "PWM
 *                       COMMITTED (Phase 13)" above
 *   sleep               hold GPC0 low indefinitely (no auto-revert), only
 *                       from safe-on or pwm-committed - see "SLEEP
 *                       (Phase 14)" above
 *   wake                restore whatever "sleep" saved as the wake target
 *                       (safe-on, or pwm-committed at the saved duty),
 *                       only from asleep - see "SLEEP (Phase 14)" above
 *   disarm              force safe-on convergence, then release the GPIO
 *                       claim and return to boot-preserve
 *   restore             cancel any active bounded operation and restore
 *                       immediately (the safety escape hatch, always
 *                       allowed, a no-op if nothing is active)
 *
 * NEVER enable CONFIG_NEBULAOS_BACKLIGHT_FINAL_CONTROLLER for a
 * production/active-slot build until live hardware testing confirms
 * correct behavior - see scripts/build/backlight-final-controller-variant.sh.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/pwm.h>
#include <linux/gpio/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/build_bug.h>
#include <linux/moduleparam.h>
#include <linux/stringify.h>

#define NBLC_NAME				"nebulaos_backlight_final"

/* Hard ceiling for ANY single bounded operation, per the mission's Phase 11
 * watchdog spec ("Maximum any single mutation may run: 2 seconds"). Every
 * operation-specific duration below is compile-time (static_assert) or
 * runtime-clamped against this. */
#define NBLC_WATCHDOG_MAX_MS			2000U

/* safe-off-test: configurable but hard-capped - see nblc_clamped_safe_off_ms(). */
#define NBLC_SAFE_OFF_TEST_MIN_MS		100U
#define NBLC_SAFE_OFF_TEST_DEFAULT_MS		1000U

/* PC22 test duration - fixed, not configurable (mission Phase 10: "drive
 * low for 1 second then restore, drive high for 1 second then restore -
 * nothing else"). */
#define NBLC_PC22_TEST_MS			1000U

/* pwm-active test duration - fixed. No mission-specified shorter default
 * exists for this state, so this uses the full watchdog ceiling; this is a
 * deliberate, documented judgment call, not a mission-mandated number. */
#define NBLC_PWM_TEST_MS			2000U

/* Candidate PWM period only - see backlight-path-analysis.txt. Matches
 * DISPLAY-B0-DIAG's own candidate and stock's pwm_backlight.sh
 * pwm_freq=50000. Never proven correct for this board's real backlight
 * circuit. */
#define NBLC_PWM_PERIOD_NS			20000U

/* sleep/wake transition-bounding windows - see file header "SLEEP
 * (Phase 14)". These bound only the transition itself (arm-before-mutate,
 * same as every other operation); the RESULTING NBLC_STATE_ASLEEP has no
 * auto-revert - the bounding work is cancelled once the transition
 * completes successfully. Use the full watchdog ceiling since the
 * pwm-committed side of both directions chains a full
 * nblc_converge_gpc0_safe_on_locked() and/or nblc_enter_pwm_active_locked()
 * call on top of the plain GPIO mutation. */
#define NBLC_SLEEP_TRANSITION_MS		NBLC_WATCHDOG_MAX_MS
#define NBLC_WAKE_TRANSITION_MS		NBLC_WATCHDOG_MAX_MS

static_assert(NBLC_SAFE_OFF_TEST_DEFAULT_MS >= NBLC_SAFE_OFF_TEST_MIN_MS &&
	      NBLC_SAFE_OFF_TEST_DEFAULT_MS <= NBLC_WATCHDOG_MAX_MS,
	      "NBLC_SAFE_OFF_TEST_DEFAULT_MS must stay within [MIN,WATCHDOG_MAX]");
static_assert(NBLC_PC22_TEST_MS <= NBLC_WATCHDOG_MAX_MS,
	      "NBLC_PC22_TEST_MS must not exceed NBLC_WATCHDOG_MAX_MS");
static_assert(NBLC_PWM_TEST_MS <= NBLC_WATCHDOG_MAX_MS,
	      "NBLC_PWM_TEST_MS must not exceed NBLC_WATCHDOG_MAX_MS");
static_assert(NBLC_SLEEP_TRANSITION_MS <= NBLC_WATCHDOG_MAX_MS,
	      "NBLC_SLEEP_TRANSITION_MS must not exceed NBLC_WATCHDOG_MAX_MS");
static_assert(NBLC_WAKE_TRANSITION_MS <= NBLC_WATCHDOG_MAX_MS,
	      "NBLC_WAKE_TRANSITION_MS must not exceed NBLC_WATCHDOG_MAX_MS");

static unsigned int safe_off_test_ms = NBLC_SAFE_OFF_TEST_DEFAULT_MS;
module_param(safe_off_test_ms, uint, 0644);
MODULE_PARM_DESC(safe_off_test_ms,
		 "safe-off-test bounded duration in ms (clamped to [" \
		 __stringify(NBLC_SAFE_OFF_TEST_MIN_MS) "," \
		 __stringify(NBLC_WATCHDOG_MAX_MS) "] at every use, "
		 "regardless of this value)");

/*
 * Cross-module capability query exported by pwm-ingenic-v2.c (NebulaOS PWM
 * state readback patch - scripts/build/patches/pwm-ingenic-v2-get-state.patch,
 * docs/NEBULAOS_PWM_STATE_READBACK_REPORT.md). Same direct-extern convention
 * nebulaos_backlight_probe_diag.c already uses for this symbol - no shared
 * header exists for it. Guarded by #ifdef so the reference itself compiles
 * out (not just dead-code-eliminates) when CONFIG_PWM_INGENIC_V2_GET_STATE
 * is unselected. Used here purely as an optional, non-gating diagnostic
 * readback - see the file header "PWM (Phase 12)" section. */
#ifdef CONFIG_PWM_INGENIC_V2_GET_STATE
extern bool ingenic_pwm_channel_get_state_is_exact(struct pwm_chip *chip, unsigned int channel);
#endif

enum nblc_state {
	NBLC_STATE_BOOT_PRESERVE = 0,
	NBLC_STATE_SAFE_ON,
	NBLC_STATE_SAFE_OFF_TEST,
	NBLC_STATE_PWM_ACTIVE,
	/* Sustained hold of an already-proven-safe PWM duty cycle - see the
	 * file header "PWM COMMITTED (Phase 13)" section. Reachable ONLY via
	 * "commit-pwm" issued while NBLC_STATE_PWM_ACTIVE is genuinely still
	 * in flight (nblc_cmd_commit_pwm()) - never directly from safe-on or
	 * any other state. No automatic timeout; every existing convergence
	 * path (nblc_force_restore_now(), and therefore both "restore" and
	 * "enter-safe-on"/"disarm", plus nblc_remove()) already forces
	 * convergence to safe-on for this state with zero changes to those
	 * functions - see the Phase 13 section for why. */
	NBLC_STATE_PWM_COMMITTED,
	/* Backlight-only sleep - see the file header "SLEEP (Phase 14)"
	 * section. Reachable ONLY via the "sleep" command
	 * (nblc_cmd_sleep()), and ONLY from NBLC_STATE_SAFE_ON or
	 * NBLC_STATE_PWM_COMMITTED - both represent a genuinely on, stable
	 * backlight. GPC0 held low (never PWM-duty-based); no automatic
	 * timeout. Like NBLC_STATE_PWM_COMMITTED, every existing
	 * convergence path (nblc_force_restore_now(), nblc_cmd_enter_safe_on(),
	 * nblc_remove()) already converges this state to safe-on with ZERO
	 * changes to those functions, because it never holds n->pwm/
	 * n->pwm_pinctrl - only n->gpc0_gpio (driven low) - see the Phase 14
	 * section for why. */
	NBLC_STATE_ASLEEP,
	/* Not one of the mission's named states - an internal safety
	 * rail. Entered ONLY if nblc_converge_gpc0_safe_on_locked() itself
	 * fails to re-establish and verify GPC0=GPIO-high (a genuine kernel
	 * API failure - gpiod_get()/gpiod_direction_output() returning an
	 * error, or the post-drive readback not confirming high). This
	 * exists to preserve the invariant every other function in this
	 * file relies on ("state == NBLC_STATE_SAFE_ON" always implies
	 * "n->gpc0_gpio is held and verified high") - without it, a failed
	 * convergence could silently leave state claiming safe-on while
	 * gpc0_gpio is actually NULL. safe-off-test/pwm-active both refuse
	 * to start from this state (same -EPERM path as any other non-
	 * safe-on state); "enter-safe-on"/"restore" remain valid from here
	 * and are the only way out - matching the mission's "a restore that
	 * itself fails is a hard-stop condition ... surfaced clearly" rule. */
	NBLC_STATE_FAULT,
};

enum nblc_op {
	NBLC_OP_NONE = 0,
	NBLC_OP_SAFE_OFF_TEST,
	NBLC_OP_PC22_TEST_LOW,
	NBLC_OP_PC22_TEST_HIGH,
	NBLC_OP_PWM_ACTIVE,
	/* Transition-bounding only - see file header "SLEEP (Phase 14)".
	 * Cleared to NBLC_OP_NONE as soon as the transition into/out of
	 * NBLC_STATE_ASLEEP completes (success or failure); never left
	 * armed for the resulting settled state itself. */
	NBLC_OP_SLEEP_TRANSITION,
	NBLC_OP_WAKE_TRANSITION,
};

struct nblc {
	struct device		*dev;

	/* Serializes every command handler, the watchdog callback, and
	 * remove() against each other. Never held across
	 * cancel_delayed_work_sync() - see nblc_cmd_restore()/_remove() for
	 * why (the watchdog callback also takes this lock; calling the
	 * synchronous cancel while holding it could deadlock against a
	 * concurrently-running watchdog). */
	struct mutex		lock;
	struct delayed_work	restore_work;

	enum nblc_state		state;
	enum nblc_op		active_op;	/* NBLC_OP_NONE unless a bounded op is in flight */
	unsigned int		active_timeout_ms;
	unsigned long		deadline;	/* jiffies, valid only while active_op != NONE */

	/* GPC0 resources - each acquired/released strictly at transition
	 * boundaries. Exactly one of gpc0_gpio/pwm_pinctrl+pwm may be
	 * non-NULL at a time; both NULL means GPC0 is currently unclaimed
	 * by this driver (boot-preserve, or mid-transition). */
	struct gpio_desc	*gpc0_gpio;	/* non-NULL only while GPC0 is claimed as GPIO */
	struct pinctrl		*pwm_pinctrl;	/* non-NULL only while GPC0 is muxed to PWM */
	struct pwm_device	*pwm;		/* non-NULL only while PWM channel 0 is claimed */
	unsigned int		pwm_duty_pct;	/* valid only while pwm != NULL */

	/* Valid only while state == NBLC_STATE_PWM_COMMITTED - the jiffies
	 * timestamp commit-pwm was accepted at. A simple monotonic counter,
	 * not persisted - see file header "PWM COMMITTED (Phase 13)". */
	unsigned long		committed_since;

	/* Sleep/wake bookkeeping - see file header "SLEEP (Phase 14)".
	 * sleep_wake_target_is_pwm/_duty_pct are captured at "sleep" time and
	 * consumed at "wake" time; valid whenever state == NBLC_STATE_ASLEEP
	 * (and briefly still valid immediately after waking, harmlessly
	 * stale). asleep_since is a simple monotonic counter, same style as
	 * committed_since, valid only while state == NBLC_STATE_ASLEEP. */
	bool			sleep_wake_target_is_pwm;
	unsigned int		sleep_wake_target_duty_pct;
	unsigned long		asleep_since;
	unsigned int		sleep_count;
	unsigned int		wake_count;
	unsigned int		wake_failure_count;

	bool			safe_on_verified;

	/* PC22 resources - only ever non-NULL while a pc22-test-* operation
	 * is actually in flight (released immediately on convergence). */
	struct gpio_desc	*pc22_gpio;
	bool			pc22_initial_captured;
	bool			pc22_initial_level_known;
	int			pc22_initial_level;	/* valid only if pc22_initial_level_known */
	bool			pc22_initial_direction_known;	/* always false on this platform - see file header */

	unsigned int		restore_count;			/* explicit "restore"/"disarm" */
	unsigned int		watchdog_restore_count;		/* timeout-triggered */
	unsigned int		restore_failure_count;		/* readback did not confirm the target */
	unsigned int		op_failure_count;		/* rejected/failed operation requests */
	char			last_restore_reason[40];

	struct dentry		*debugfs_dir;
};

static const char *nblc_state_name(enum nblc_state s)
{
	switch (s) {
	case NBLC_STATE_BOOT_PRESERVE: return "boot-preserve";
	case NBLC_STATE_SAFE_ON: return "safe-on";
	case NBLC_STATE_SAFE_OFF_TEST: return "safe-off-test";
	case NBLC_STATE_PWM_ACTIVE: return "pwm-active";
	case NBLC_STATE_PWM_COMMITTED: return "pwm-committed";
	case NBLC_STATE_ASLEEP: return "asleep";
	case NBLC_STATE_FAULT: return "fault-unclaimed-hardware";
	default: return "unknown";
	}
}

static const char *nblc_op_name(enum nblc_op op)
{
	switch (op) {
	case NBLC_OP_NONE: return "none";
	case NBLC_OP_SAFE_OFF_TEST: return "safe-off-test";
	case NBLC_OP_PC22_TEST_LOW: return "pc22-test-low";
	case NBLC_OP_PC22_TEST_HIGH: return "pc22-test-high";
	case NBLC_OP_PWM_ACTIVE: return "pwm-active";
	case NBLC_OP_SLEEP_TRANSITION: return "sleep-transition";
	case NBLC_OP_WAKE_TRANSITION: return "wake-transition";
	default: return "unknown";
	}
}

static unsigned int nblc_clamped_safe_off_ms(void)
{
	unsigned int ms = READ_ONCE(safe_off_test_ms);

	if (ms < NBLC_SAFE_OFF_TEST_MIN_MS)
		ms = NBLC_SAFE_OFF_TEST_MIN_MS;
	if (ms > NBLC_WATCHDOG_MAX_MS)
		ms = NBLC_WATCHDOG_MAX_MS;
	return ms;
}

/*
 * The single, shared GPC0 restoration routine - see the file header
 * item (g). Always call with n->lock held. Safe to call from ANY prior
 * state, including a partially-torn mid-transition state (pwm held but
 * pinctrl not yet released, or neither held at all) - it inspects what is
 * currently held and unwinds/acquires exactly what is needed, in the safe
 * order established in items (c)/(d): disable+release PWM first, release
 * the "pwm-active" pinctrl claim second (cleanly clearing GPC0's mux
 * ownership - see item (c)), THEN acquire/re-drive GPC0 as GPIO high last,
 * so GPC0 is never simultaneously claimed through two subsystems at once.
 */
static void nblc_converge_gpc0_safe_on_locked(struct nblc *n, const char *reason)
{
	int ret;

	if (n->pwm) {
		struct pwm_state st;

		pwm_get_state(n->pwm, &st);
		st.enabled = false;
		ret = pwm_apply_might_sleep(n->pwm, &st);
		if (ret)
			dev_err(n->dev, "converge: failed to disable PWM channel 0: %d\n", ret);
		pwm_put(n->pwm);
		n->pwm = NULL;
		n->pwm_duty_pct = 0;
	}

	if (n->pwm_pinctrl) {
		/* pinctrl_put() -> pinctrl_free() -> pinctrl_free_setting()
		 * disables the currently-active "pwm-active" mux setting and
		 * calls pin_free() for GPC0 - a clean, symmetric release of
		 * the mux claim, not merely "hardware ends up correct despite
		 * stale bookkeeping". See the file header item (c). */
		pinctrl_put(n->pwm_pinctrl);
		n->pwm_pinctrl = NULL;
	}

	if (!n->gpc0_gpio) {
		n->gpc0_gpio = gpiod_get(n->dev, "backlight", GPIOD_OUT_HIGH);
		if (IS_ERR(n->gpc0_gpio)) {
			ret = PTR_ERR(n->gpc0_gpio);
			n->gpc0_gpio = NULL;
			n->safe_on_verified = false;
			n->restore_failure_count++;
			strscpy(n->last_restore_reason, "gpiod_get(backlight) failed",
				sizeof(n->last_restore_reason));
			dev_err(n->dev, "converge (%s): failed to acquire GPC0 as GPIO: %d - "
				"entering fault state, hardware ownership indeterminate\n",
				reason, ret);
			/* Invariant guard: never leave state claiming safe-on
			 * while gpc0_gpio is actually NULL - see
			 * NBLC_STATE_FAULT's definition. */
			n->state = NBLC_STATE_FAULT;
			return;
		}
	} else {
		/* Already held (e.g. converging out of safe-off-test, which
		 * never released it) - (re)drive it high unconditionally.
		 * gpiod_direction_output() unconditionally reprograms GPC0's
		 * pinmux to GPIO_OUTPUT regardless of its current function -
		 * see the file header item (c). */
		ret = gpiod_direction_output(n->gpc0_gpio, 1);
		if (ret) {
			n->safe_on_verified = false;
			n->restore_failure_count++;
			strscpy(n->last_restore_reason, "gpiod_direction_output failed",
				sizeof(n->last_restore_reason));
			dev_err(n->dev, "converge (%s): failed to drive GPC0 high: %d - "
				"entering fault state, hardware ownership indeterminate\n",
				reason, ret);
			n->state = NBLC_STATE_FAULT;
			return;
		}
	}

	/* Hard-stop honesty check (mission requirement): re-verify the
	 * readback rather than assuming the write succeeded. */
	if (gpiod_get_value(n->gpc0_gpio) == 1) {
		n->safe_on_verified = true;
	} else {
		n->safe_on_verified = false;
		n->restore_failure_count++;
		strscpy(n->last_restore_reason, "readback did not confirm GPC0 high",
			sizeof(n->last_restore_reason));
		dev_err(n->dev, "converge (%s): GPC0 readback did NOT confirm high - "
			"restoration failure, entering fault state\n", reason);
		n->state = NBLC_STATE_FAULT;
		return;
	}

	strscpy(n->last_restore_reason, reason, sizeof(n->last_restore_reason));
	n->state = NBLC_STATE_SAFE_ON;
}

/* PC22 equivalent of the above - restores PC22 to its captured initial
 * level and releases the descriptor. Always call with n->lock held. Never
 * touches GPC0/PWM state. */
static void nblc_converge_pc22_locked(struct nblc *n, const char *reason)
{
	int ret;

	if (!n->pc22_gpio)
		return;

	if (n->pc22_initial_level_known) {
		ret = gpiod_direction_output(n->pc22_gpio, n->pc22_initial_level);
		if (ret) {
			n->restore_failure_count++;
			strscpy(n->last_restore_reason, "pc22 restore-drive failed",
				sizeof(n->last_restore_reason));
			dev_err(n->dev, "converge (%s): failed to restore PC22: %d\n",
				reason, ret);
		} else if (gpiod_get_value(n->pc22_gpio) != n->pc22_initial_level) {
			n->restore_failure_count++;
			strscpy(n->last_restore_reason, "pc22 readback mismatch after restore",
				sizeof(n->last_restore_reason));
			dev_err(n->dev, "converge (%s): PC22 readback did not confirm "
				"restored level\n", reason);
		} else {
			strscpy(n->last_restore_reason, reason, sizeof(n->last_restore_reason));
		}
	} else {
		/* Should be unreachable - the initial level is always
		 * captured synchronously before a test op is ever armed (see
		 * nblc_pc22_test_locked()). Documented defensively rather
		 * than silently leaving PC22 in the test level. */
		dev_err(n->dev, "converge (%s): PC22 has no known initial level to "
			"restore to - leaving as-is, this should never happen\n", reason);
		n->restore_failure_count++;
	}

	gpiod_put(n->pc22_gpio);
	n->pc22_gpio = NULL;
}

/* The kernel-owned watchdog. Runs on the system workqueue, independent of
 * any userspace process. Dispatches on active_op to decide whether GPC0 or
 * PC22 needs converging - see the file header "WATCHDOG (Phase 11)"
 * section. */
static void nblc_restore_work(struct work_struct *work)
{
	struct nblc *n = container_of(to_delayed_work(work), struct nblc, restore_work);

	mutex_lock(&n->lock);
	if (n->active_op == NBLC_OP_NONE) {
		mutex_unlock(&n->lock);
		return;
	}
	switch (n->active_op) {
	case NBLC_OP_SAFE_OFF_TEST:
	case NBLC_OP_PWM_ACTIVE:
	case NBLC_OP_SLEEP_TRANSITION:
	case NBLC_OP_WAKE_TRANSITION:
		nblc_converge_gpc0_safe_on_locked(n, "watchdog-timeout");
		break;
	case NBLC_OP_PC22_TEST_LOW:
	case NBLC_OP_PC22_TEST_HIGH:
		nblc_converge_pc22_locked(n, "watchdog-timeout");
		break;
	default:
		break;
	}
	n->active_op = NBLC_OP_NONE;
	n->watchdog_restore_count++;
	mutex_unlock(&n->lock);
}

/* "restore" / "disarm" 's shared synchronous-cancel-then-converge helper.
 * Always call WITHOUT n->lock held - cancel_delayed_work_sync() can block
 * waiting for a concurrently-running watchdog callback that itself takes
 * n->lock (same ordering rule nebulaos_backlight_probe_diag.c already
 * established). */
static void nblc_force_restore_now(struct nblc *n, const char *reason)
{
	cancel_delayed_work_sync(&n->restore_work);

	mutex_lock(&n->lock);
	if (n->active_op != NBLC_OP_NONE) {
		switch (n->active_op) {
		case NBLC_OP_SAFE_OFF_TEST:
		case NBLC_OP_PWM_ACTIVE:
		case NBLC_OP_SLEEP_TRANSITION:
		case NBLC_OP_WAKE_TRANSITION:
			nblc_converge_gpc0_safe_on_locked(n, reason);
			break;
		case NBLC_OP_PC22_TEST_LOW:
		case NBLC_OP_PC22_TEST_HIGH:
			nblc_converge_pc22_locked(n, reason);
			break;
		default:
			break;
		}
		n->active_op = NBLC_OP_NONE;
		n->restore_count++;
	} else if (n->state != NBLC_STATE_SAFE_ON && n->state != NBLC_STATE_BOOT_PRESERVE) {
		/* Defensive: state claims something active but active_op
		 * disagrees - treat as inconsistent and force convergence
		 * anyway rather than trusting a state that doesn't match. */
		nblc_converge_gpc0_safe_on_locked(n, reason);
		n->restore_count++;
	}
	mutex_unlock(&n->lock);
}

static int nblc_cmd_restore(struct nblc *n)
{
	nblc_force_restore_now(n, "explicit-restore");
	dev_info(n->dev, "restore complete: state=%s safe_on_verified=%d\n",
		 nblc_state_name(n->state), n->safe_on_verified);
	return 0;
}

/* enter-safe-on: the universal convergence command, valid from ANY state -
 * see the file header's runtime-interface list. */
static int nblc_cmd_enter_safe_on(struct nblc *n)
{
	bool verified;

	cancel_delayed_work_sync(&n->restore_work);

	mutex_lock(&n->lock);
	if (n->active_op != NBLC_OP_NONE) {
		switch (n->active_op) {
		case NBLC_OP_PC22_TEST_LOW:
		case NBLC_OP_PC22_TEST_HIGH:
			/* A PC22 test doesn't touch GPC0 - restore it too so
			 * "enter-safe-on" really does leave everything this
			 * driver owns in a clean, known state. */
			nblc_converge_pc22_locked(n, "enter-safe-on");
			break;
		default:
			break;
		}
		n->active_op = NBLC_OP_NONE;
	}
	nblc_converge_gpc0_safe_on_locked(n, "enter-safe-on");
	verified = n->safe_on_verified;
	mutex_unlock(&n->lock);

	return verified ? 0 : -EIO;
}

static int nblc_cmd_disarm(struct nblc *n)
{
	int ret;

	/* Fail-safe convergence first (same fail-safe-before-disable
	 * discipline as nebulaos_backlight_probe_diag.c's disarm). If this
	 * does not succeed, do NOT proceed to release the GPIO claim and
	 * silently drop into boot-preserve anyway - that would discard a
	 * genuine fault condition instead of surfacing it (mission
	 * requirement: a restore that itself fails is a hard-stop
	 * condition). */
	ret = nblc_cmd_enter_safe_on(n);
	if (ret) {
		dev_err(n->dev, "disarm: refusing to release GPC0 - safe-on convergence did "
			"not verify (state=%s) - see .../status for the failure reason\n",
			nblc_state_name(n->state));
		return ret;
	}

	mutex_lock(&n->lock);
	if (n->gpc0_gpio) {
		gpiod_put(n->gpc0_gpio);
		n->gpc0_gpio = NULL;
	}
	n->state = NBLC_STATE_BOOT_PRESERVE;
	n->safe_on_verified = false;
	strscpy(n->last_restore_reason, "disarmed", sizeof(n->last_restore_reason));
	mutex_unlock(&n->lock);

	dev_info(n->dev, "disarmed - returned to boot-preserve (GPC0 left at its last-driven "
		 "level, no longer claimed by this driver)\n");
	return 0;
}

/* The single "drive GPC0 low" mechanic, shared by safe-off-test and sleep
 * (entered from safe-on) - see file header "SLEEP (Phase 14)". Always call
 * with n->lock held and n->gpc0_gpio already valid (i.e. reached from a
 * genuinely safe-on GPC0 - both callers guarantee this before calling). */
static int nblc_drive_gpc0_low_locked(struct nblc *n)
{
	return gpiod_direction_output(n->gpc0_gpio, 0);
}

static int nblc_cmd_safe_off_test(struct nblc *n)
{
	unsigned int ms = nblc_clamped_safe_off_ms();
	int ret;

	mutex_lock(&n->lock);
	if (n->state != NBLC_STATE_SAFE_ON) {
		n->op_failure_count++;
		mutex_unlock(&n->lock);
		dev_warn(n->dev, "rejected safe-off-test: only valid from safe-on (current: %s)\n",
			 nblc_state_name(n->state));
		return -EPERM;
	}
	if (n->active_op != NBLC_OP_NONE) {
		n->op_failure_count++;
		mutex_unlock(&n->lock);
		dev_warn(n->dev, "rejected safe-off-test: another operation is already active\n");
		return -EBUSY;
	}

	/* Arm BEFORE mutating - see the file header WATCHDOG section. */
	n->active_op = NBLC_OP_SAFE_OFF_TEST;
	n->active_timeout_ms = ms;
	n->deadline = jiffies + msecs_to_jiffies(ms);
	schedule_delayed_work(&n->restore_work, msecs_to_jiffies(ms));

	ret = gpiod_direction_output(n->gpc0_gpio, 0);
	if (ret) {
		cancel_delayed_work(&n->restore_work);
		n->active_op = NBLC_OP_NONE;
		n->op_failure_count++;
		mutex_unlock(&n->lock);
		dev_err(n->dev, "safe-off-test: failed to drive GPC0 low: %d\n", ret);
		return ret;
	}
	n->state = NBLC_STATE_SAFE_OFF_TEST;
	mutex_unlock(&n->lock);

	dev_info(n->dev, "safe-off-test active: GPC0 low for %ums, auto-restoring\n", ms);
	return 0;
}

/* Only reachable from safe-on - see the file header item (d)/state
 * machine section. Every failure path converges back to safe-on before
 * returning the error. */
static int nblc_enter_pwm_active_locked(struct nblc *n, unsigned int duty_pct)
{
	struct pinctrl *pctl;
	struct pwm_device *pwm;
	struct pwm_state st;
	int ret;

	/* Step 1: release the GPIO claim BEFORE acquiring the PWM mux claim
	 * - GPC0 must never be simultaneously claimed through both
	 * subsystems at once (file header item (d)). */
	gpiod_put(n->gpc0_gpio);
	n->gpc0_gpio = NULL;

	/* Step 2: select this driver's OWN "pwm-active" pinctrl state (never
	 * named "default"/"init" - see file header item (b)). This is the
	 * ONLY place in this file that muxes GPC0 to the PWM peripheral
	 * function. */
	pctl = pinctrl_get_select(n->dev, "pwm-active");
	if (IS_ERR(pctl)) {
		ret = PTR_ERR(pctl);
		dev_err(n->dev, "enter-pwm-active: failed to select pwm-active pinctrl "
			"state: %d\n", ret);
		goto unwind;
	}
	n->pwm_pinctrl = pctl;

	/* Step 3: acquire PWM channel 0. */
	pwm = pwm_get(n->dev, NULL);
	if (IS_ERR(pwm)) {
		ret = PTR_ERR(pwm);
		dev_err(n->dev, "enter-pwm-active: failed to acquire PWM channel 0: %d\n", ret);
		goto unwind;
	}
	n->pwm = pwm;

	/* Step 4: apply the fixed candidate period at the requested duty -
	 * 25/50/75 only, enforced by the caller (nblc_cmd_pwm_active()). */
	st.period = NBLC_PWM_PERIOD_NS;
	st.duty_cycle = (u64)NBLC_PWM_PERIOD_NS * duty_pct / 100;
	st.polarity = PWM_POLARITY_NORMAL;
	st.enabled = true;
	st.usage_power = false;
	ret = pwm_apply_might_sleep(n->pwm, &st);
	if (ret) {
		dev_err(n->dev, "enter-pwm-active: pwm_apply_might_sleep(%u%%) failed: %d\n",
			duty_pct, ret);
		goto unwind;
	}
	n->pwm_duty_pct = duty_pct;

#ifdef CONFIG_PWM_INGENIC_V2_GET_STATE
	/* Optional, non-gating cross-check readback - see file header
	 * "PWM (Phase 12)" section. Never gates success/failure. */
	if (n->pwm->chip && n->pwm->chip->dev && n->pwm->chip->dev->driver &&
	    n->pwm->chip->dev->driver->name &&
	    !strcmp(n->pwm->chip->dev->driver->name, "ingenic-pwm")) {
		struct pwm_state readback;

		pwm_get_state(n->pwm, &readback);
		dev_info(n->dev, "enter-pwm-active: post-apply readback period=%lluns "
			 "duty=%lluns enabled=%d (exact=%d)\n",
			 readback.period, readback.duty_cycle, readback.enabled,
			 ingenic_pwm_channel_get_state_is_exact(n->pwm->chip, n->pwm->hwpwm));
	}
#endif

	return 0;

unwind:
	/* Never leave GPC0 muxed-to-PWM-but-not-actually-driven - the exact
	 * shape of tonight's incident. Always converge back to safe-on on
	 * any failure here. */
	nblc_converge_gpc0_safe_on_locked(n, "enter-pwm-active-failed");
	return ret;
}

static int nblc_cmd_pwm_active(struct nblc *n, unsigned int duty_pct)
{
	int ret;

	if (duty_pct != 25 && duty_pct != 50 && duty_pct != 75)
		return -EINVAL;	/* defensive - dispatcher only ever passes these three */

	mutex_lock(&n->lock);
	if (n->state != NBLC_STATE_SAFE_ON) {
		n->op_failure_count++;
		mutex_unlock(&n->lock);
		dev_warn(n->dev, "rejected pwm-active-%u: only valid from safe-on "
			 "(current: %s)\n", duty_pct, nblc_state_name(n->state));
		return -EPERM;
	}
	if (n->active_op != NBLC_OP_NONE) {
		n->op_failure_count++;
		mutex_unlock(&n->lock);
		dev_warn(n->dev, "rejected pwm-active-%u: another operation is already active\n",
			 duty_pct);
		return -EBUSY;
	}

	/* Arm BEFORE mutating - covers a crash anywhere inside
	 * nblc_enter_pwm_active_locked()'s multi-step sequence with the same
	 * idempotent convergence routine the watchdog itself uses. */
	n->active_op = NBLC_OP_PWM_ACTIVE;
	n->active_timeout_ms = NBLC_PWM_TEST_MS;
	n->deadline = jiffies + msecs_to_jiffies(NBLC_PWM_TEST_MS);
	schedule_delayed_work(&n->restore_work, msecs_to_jiffies(NBLC_PWM_TEST_MS));

	ret = nblc_enter_pwm_active_locked(n, duty_pct);
	if (ret) {
		cancel_delayed_work(&n->restore_work);
		n->active_op = NBLC_OP_NONE;
		n->op_failure_count++;
		mutex_unlock(&n->lock);
		return ret;
	}

	n->state = NBLC_STATE_PWM_ACTIVE;
	mutex_unlock(&n->lock);

	dev_info(n->dev, "pwm-active active: %u%% duty, auto-restoring to safe-on in %ums\n",
		 duty_pct, NBLC_PWM_TEST_MS);
	return 0;
}

/* commit-pwm: promote an in-flight, still-active pwm-active bounded test
 * into NBLC_STATE_PWM_COMMITTED - see the file header "PWM COMMITTED
 * (Phase 13)" section for the full design and reasoning. Reachable ONLY
 * while a pwm-active test is genuinely still in flight (state ==
 * NBLC_STATE_PWM_ACTIVE *and* active_op == NBLC_OP_PWM_ACTIVE) - this
 * forces every commit through the exact same acquire/apply/verify sequence
 * (nblc_enter_pwm_active_locked()) already exercised by the pwm-active
 * regression tests; there is no, and will never be, a direct safe-on ->
 * pwm-committed path. Also naturally rejects a second "commit-pwm" issued
 * while already committed, since state is then NBLC_STATE_PWM_COMMITTED,
 * not NBLC_STATE_PWM_ACTIVE - no separate "already committed" check
 * needed.
 *
 * Disarms the in-flight test's fixed ~2s auto-revert timer
 * (cancel_delayed_work(), same in-lock/non-sync pattern every other
 * command's own error/transition path in this file already uses - see the
 * n->lock comment on cancel_delayed_work_sync() for why the _sync variant
 * must never be called while holding the lock) and clears active_op to
 * NBLC_OP_NONE - from this point on, no watchdog callback will ever fire
 * for this operation again. No hardware is touched: n->pwm/n->pwm_pinctrl
 * are left exactly as they were, so committing causes no visible flicker.
 */
static int nblc_cmd_commit_pwm(struct nblc *n)
{
	unsigned int duty;

	mutex_lock(&n->lock);
	if (n->state != NBLC_STATE_PWM_ACTIVE || n->active_op != NBLC_OP_PWM_ACTIVE) {
		n->op_failure_count++;
		mutex_unlock(&n->lock);
		dev_warn(n->dev, "rejected commit-pwm: only valid from an in-flight "
			 "pwm-active test (current: state=%s active_op=%s)\n",
			 nblc_state_name(n->state), nblc_op_name(n->active_op));
		return -EPERM;
	}

	cancel_delayed_work(&n->restore_work);
	n->active_op = NBLC_OP_NONE;
	n->state = NBLC_STATE_PWM_COMMITTED;
	n->committed_since = jiffies;
	duty = n->pwm_duty_pct;
	mutex_unlock(&n->lock);

	dev_info(n->dev, "pwm committed: %u%% duty held indefinitely (no auto-revert) - "
		 "write \"enter-safe-on\" or \"restore\" to release\n", duty);
	return 0;
}

/* sleep: backlight-only sleep - see file header "SLEEP (Phase 14)". Only
 * reachable from safe-on or pwm-committed (both are a genuinely on, stable
 * backlight). Captures the wake target BEFORE any hardware mutation, then
 * (if leaving pwm-committed) releases the PWM claim and remuxes GPC0 back
 * to GPIO via the SAME nblc_converge_gpc0_safe_on_locked() routine every
 * other exit from an active PWM state already uses, then drives GPC0 low
 * via nblc_drive_gpc0_low_locked(). No auto-revert once asleep - the
 * transition-bounding watchdog work is cancelled on success, same pattern
 * commit-pwm already uses to disarm its own bounding timer on a successful
 * transition into an unbounded settled state.
 */
static int nblc_cmd_sleep(struct nblc *n)
{
	bool from_pwm;
	unsigned int duty = 0;
	int ret;

	mutex_lock(&n->lock);
	if (n->state != NBLC_STATE_SAFE_ON && n->state != NBLC_STATE_PWM_COMMITTED) {
		n->op_failure_count++;
		mutex_unlock(&n->lock);
		dev_warn(n->dev, "rejected sleep: only valid from safe-on or pwm-committed "
			 "(current: %s)\n", nblc_state_name(n->state));
		return -EPERM;
	}
	if (n->active_op != NBLC_OP_NONE) {
		n->op_failure_count++;
		mutex_unlock(&n->lock);
		dev_warn(n->dev, "rejected sleep: another operation is already active\n");
		return -EBUSY;
	}

	/* Capture the wake target BEFORE any mutation. */
	from_pwm = (n->state == NBLC_STATE_PWM_COMMITTED);
	if (from_pwm)
		duty = n->pwm_duty_pct;

	/* Arm BEFORE mutating - same ordering principle as every other
	 * operation in this file. This window bounds only the transition
	 * itself; on success the work is cancelled below rather than left to
	 * fire, since sleep has no auto-revert. */
	n->active_op = NBLC_OP_SLEEP_TRANSITION;
	n->active_timeout_ms = NBLC_SLEEP_TRANSITION_MS;
	n->deadline = jiffies + msecs_to_jiffies(NBLC_SLEEP_TRANSITION_MS);
	schedule_delayed_work(&n->restore_work, msecs_to_jiffies(NBLC_SLEEP_TRANSITION_MS));

	if (from_pwm) {
		/* Release the PWM claim and remux GPC0 back to GPIO, driven
		 * high and verified - the exact same convergence routine
		 * every other exit from an active PWM state already uses
		 * (file header item (c)); never a second, parallel "release
		 * PWM, remux to GPIO" implementation. */
		nblc_converge_gpc0_safe_on_locked(n, "sleep-from-pwm-committed");
		if (n->state != NBLC_STATE_SAFE_ON) {
			/* Convergence already recorded the failure reason/
			 * counters and moved to NBLC_STATE_FAULT - nothing
			 * more to do here but stop this operation cleanly. */
			cancel_delayed_work(&n->restore_work);
			n->active_op = NBLC_OP_NONE;
			n->op_failure_count++;
			mutex_unlock(&n->lock);
			dev_err(n->dev, "sleep: aborted - could not converge out of "
				"pwm-committed before sleeping\n");
			return -EIO;
		}
	}

	ret = nblc_drive_gpc0_low_locked(n);
	if (ret) {
		cancel_delayed_work(&n->restore_work);
		n->active_op = NBLC_OP_NONE;
		n->op_failure_count++;
		mutex_unlock(&n->lock);
		dev_err(n->dev, "sleep: failed to drive GPC0 low: %d\n", ret);
		return ret;
	}

	n->sleep_wake_target_is_pwm = from_pwm;
	n->sleep_wake_target_duty_pct = duty;
	n->asleep_since = jiffies;
	n->sleep_count++;
	n->state = NBLC_STATE_ASLEEP;

	/* No auto-revert for sleep - cancel the transition-bounding work now
	 * that the mutation itself completed successfully. */
	cancel_delayed_work(&n->restore_work);
	n->active_op = NBLC_OP_NONE;
	mutex_unlock(&n->lock);

	if (from_pwm)
		dev_info(n->dev, "sleep active: GPC0 low, wake target=pwm-committed-%u%% - "
			 "write \"wake\" to .../command to restore\n", duty);
	else
		dev_info(n->dev, "sleep active: GPC0 low, wake target=safe-on - write "
			 "\"wake\" to .../command to restore\n");
	return 0;
}

/* wake: reverse of sleep - see file header "SLEEP (Phase 14)". Only
 * reachable from asleep. Always drives GPC0 high and verifies first, via
 * the SAME universal convergence-to-safe-on routine every other transition
 * into safe-on already uses. If the saved wake target was plain safe-on,
 * that is the whole operation. If the saved target was pwm-committed at a
 * saved duty, re-enters pwm-active at that duty via
 * nblc_enter_pwm_active_locked() - the SAME acquire/apply/verify sequence
 * "pwm-active-<pct>" itself uses, never a shortcut around
 * pwm_apply_state()/readback - then immediately performs the identical
 * bookkeeping-only commit transition nblc_cmd_commit_pwm() itself performs
 * (see that function - no hardware is touched a second time here; n->pwm/
 * n->pwm_pinctrl are left exactly as nblc_enter_pwm_active_locked() left
 * them). The end result is indistinguishable from having issued
 * "pwm-active-<pct>" then "commit-pwm" fresh.
 */
static int nblc_cmd_wake(struct nblc *n)
{
	bool from_pwm;
	unsigned int duty = 0;
	int ret;

	mutex_lock(&n->lock);
	if (n->state != NBLC_STATE_ASLEEP) {
		n->op_failure_count++;
		mutex_unlock(&n->lock);
		dev_warn(n->dev, "rejected wake: only valid from asleep (current: %s)\n",
			 nblc_state_name(n->state));
		return -EPERM;
	}
	if (n->active_op != NBLC_OP_NONE) {
		n->op_failure_count++;
		mutex_unlock(&n->lock);
		dev_warn(n->dev, "rejected wake: another operation is already active\n");
		return -EBUSY;
	}

	from_pwm = n->sleep_wake_target_is_pwm;
	duty = n->sleep_wake_target_duty_pct;

	/* Arm BEFORE mutating - same ordering principle as every other
	 * operation in this file. */
	n->active_op = NBLC_OP_WAKE_TRANSITION;
	n->active_timeout_ms = NBLC_WAKE_TRANSITION_MS;
	n->deadline = jiffies + msecs_to_jiffies(NBLC_WAKE_TRANSITION_MS);
	schedule_delayed_work(&n->restore_work, msecs_to_jiffies(NBLC_WAKE_TRANSITION_MS));

	/* Step 1: drive GPC0 high and verify - identical to how every other
	 * transition into safe-on already does it. GPC0 is still held as a
	 * GPIO from sleep (only its value was driven low, the claim itself
	 * was never released), so this takes the "already held" branch. */
	nblc_converge_gpc0_safe_on_locked(n, "wake");
	if (n->state != NBLC_STATE_SAFE_ON) {
		cancel_delayed_work(&n->restore_work);
		n->active_op = NBLC_OP_NONE;
		n->wake_failure_count++;
		mutex_unlock(&n->lock);
		dev_err(n->dev, "wake: failed to converge GPC0 high\n");
		return -EIO;
	}

	if (!from_pwm) {
		n->wake_count++;
		cancel_delayed_work(&n->restore_work);
		n->active_op = NBLC_OP_NONE;
		mutex_unlock(&n->lock);
		dev_info(n->dev, "wake complete: restored safe-on\n");
		return 0;
	}

	/* Step 2: re-enter pwm-active at the saved duty, via the SAME
	 * acquire/apply/verify sequence "pwm-active-<pct>" itself uses (see
	 * nblc_enter_pwm_active_locked() and file header item (b)/(d)). Any
	 * failure here already converges back to safe-on internally (its
	 * own "unwind:" path) - never leaves GPC0 muxed-but-undriven. */
	ret = nblc_enter_pwm_active_locked(n, duty);
	if (ret) {
		cancel_delayed_work(&n->restore_work);
		n->active_op = NBLC_OP_NONE;
		n->wake_failure_count++;
		mutex_unlock(&n->lock);
		dev_err(n->dev, "wake: failed to re-enter pwm-active at %u%% duty: %d\n",
			duty, ret);
		return ret;
	}
	n->state = NBLC_STATE_PWM_ACTIVE;

	/* Step 3: immediately re-commit - the identical bookkeeping-only
	 * transition nblc_cmd_commit_pwm() itself performs (see that
	 * function). No hardware touched a second time: n->pwm/
	 * n->pwm_pinctrl are left exactly as step 2 left them. */
	cancel_delayed_work(&n->restore_work);
	n->state = NBLC_STATE_PWM_COMMITTED;
	n->committed_since = jiffies;
	n->active_op = NBLC_OP_NONE;
	n->wake_count++;
	mutex_unlock(&n->lock);

	dev_info(n->dev, "wake complete: restored pwm-committed at %u%% duty\n", duty);
	return 0;
}

static int nblc_pc22_test_locked(struct nblc *n, int level)
{
	struct gpio_desc *desc;
	int ret;

	desc = gpiod_get(n->dev, "enable", GPIOD_ASIS);
	if (IS_ERR(desc)) {
		ret = PTR_ERR(desc);
		if (ret == -EBUSY)
			dev_warn(n->dev, "pc22-test: PC22 is already owned by another "
				 "driver - refusing\n");
		else
			dev_warn(n->dev, "pc22-test: failed to acquire PC22: %d\n", ret);
		return ret;
	}
	n->pc22_gpio = desc;

	if (!n->pc22_initial_captured) {
		/* GPIOD_ASIS never changes direction/value, so this read is
		 * still a genuine live hardware readback of whatever level
		 * was already present - see file header "PC22 (Phase 10)". */
		n->pc22_initial_level = gpiod_get_raw_value_cansleep(n->pc22_gpio);
		n->pc22_initial_level_known = true;
		/* Direction genuinely cannot be reported on this platform -
		 * see file header. Not a claim-timing limitation. */
		n->pc22_initial_direction_known = false;
		n->pc22_initial_captured = true;
	}

	ret = gpiod_direction_output(n->pc22_gpio, level);
	if (ret) {
		dev_err(n->dev, "pc22-test: failed to drive PC22 %s: %d\n",
			level ? "high" : "low", ret);
		gpiod_put(n->pc22_gpio);
		n->pc22_gpio = NULL;
		return ret;
	}
	return 0;
}

static int nblc_cmd_pc22_test(struct nblc *n, int level)
{
	enum nblc_op op = level ? NBLC_OP_PC22_TEST_HIGH : NBLC_OP_PC22_TEST_LOW;
	int ret;

	mutex_lock(&n->lock);
	if (n->active_op != NBLC_OP_NONE) {
		n->op_failure_count++;
		mutex_unlock(&n->lock);
		dev_warn(n->dev, "rejected pc22-test-%s: another operation is already active\n",
			 level ? "high" : "low");
		return -EBUSY;
	}

	/* Arm BEFORE mutating. */
	n->active_op = op;
	n->active_timeout_ms = NBLC_PC22_TEST_MS;
	n->deadline = jiffies + msecs_to_jiffies(NBLC_PC22_TEST_MS);
	schedule_delayed_work(&n->restore_work, msecs_to_jiffies(NBLC_PC22_TEST_MS));

	ret = nblc_pc22_test_locked(n, level);
	if (ret) {
		cancel_delayed_work(&n->restore_work);
		n->active_op = NBLC_OP_NONE;
		n->op_failure_count++;
		mutex_unlock(&n->lock);
		return ret;
	}
	mutex_unlock(&n->lock);

	dev_info(n->dev, "pc22-test-%s active for %ums, auto-restoring\n",
		 level ? "high" : "low", NBLC_PC22_TEST_MS);
	return 0;
}

static void nblc_status_dump(struct nblc *n, struct seq_file *s)
{
	unsigned long remaining_ms = 0;
	const char *gpc0_mode;
	int gpc0_level = -1;
	bool pwm_committed = (n->state == NBLC_STATE_PWM_COMMITTED);
	unsigned long committed_ms = 0;
	bool asleep = (n->state == NBLC_STATE_ASLEEP);
	unsigned long asleep_ms = 0;
	char wake_target[32] = "n/a";

	if (n->active_op != NBLC_OP_NONE) {
		long delta = (long)(n->deadline - jiffies);

		remaining_ms = delta > 0 ? jiffies_to_msecs(delta) : 0;
	}

	if (pwm_committed)
		committed_ms = jiffies_to_msecs((unsigned long)(jiffies - n->committed_since));

	if (asleep) {
		asleep_ms = jiffies_to_msecs((unsigned long)(jiffies - n->asleep_since));
		if (n->sleep_wake_target_is_pwm)
			snprintf(wake_target, sizeof(wake_target), "pwm-committed-%u",
				 n->sleep_wake_target_duty_pct);
		else
			strscpy(wake_target, "safe-on", sizeof(wake_target));
	}

	if (n->gpc0_gpio) {
		gpc0_mode = "gpio";
		gpc0_level = gpiod_get_value(n->gpc0_gpio);
	} else if (n->pwm_pinctrl) {
		gpc0_mode = "pwm";
	} else {
		gpc0_mode = "unclaimed";
	}

	if (s) {
#define P(fmt, ...) seq_printf(s, fmt "\n", ##__VA_ARGS__)
		P("armed: %d", n->state != NBLC_STATE_BOOT_PRESERVE);
		P("state: %s", nblc_state_name(n->state));
		P("active_op: %s", nblc_op_name(n->active_op));
		P("timeout_remaining_ms: %lu", remaining_ms);
		P("gpc0_mode: %s", gpc0_mode);
		P("gpc0_level: %d", gpc0_level);
		P("pwm_owned: %d", n->pwm != NULL);
		P("pwm_enabled: %d", n->pwm != NULL);
		P("pwm_period_ns: %u", n->pwm ? NBLC_PWM_PERIOD_NS : 0);
		P("pwm_duty_pct: %u", n->pwm ? n->pwm_duty_pct : 0);
		P("pwm_committed: %d", pwm_committed);
		P("pwm_committed_duration_ms: %lu", committed_ms);
		P("asleep: %d", asleep);
		P("wake_target: %s", wake_target);
		P("asleep_duration_ms: %lu", asleep_ms);
		P("sleep_count: %u", n->sleep_count);
		P("wake_count: %u", n->wake_count);
		P("wake_failure_count: %u", n->wake_failure_count);
		P("pc22_active: %d", n->pc22_gpio != NULL);
		P("pc22_initial_level_known: %d", n->pc22_initial_level_known);
		P("pc22_initial_level: %d", n->pc22_initial_level_known ? n->pc22_initial_level : -1);
		P("pc22_initial_direction_known: %d", n->pc22_initial_direction_known);
		P("restore_count: %u", n->restore_count);
		P("watchdog_restore_count: %u", n->watchdog_restore_count);
		P("restore_failure_count: %u", n->restore_failure_count);
		P("op_failure_count: %u", n->op_failure_count);
		P("last_restore_reason: %s", n->last_restore_reason);
		P("safe_on_verified: %d", n->safe_on_verified);
#undef P
	} else {
		dev_info(n->dev,
			 "status: armed=%d state=%s active_op=%s timeout_remaining=%lums "
			 "gpc0_mode=%s gpc0_level=%d pwm_owned=%d pwm_duty=%u%% "
			 "pwm_committed=%d pwm_committed_duration=%lums asleep=%d "
			 "wake_target=%s asleep_duration=%lums sleep_count=%u wake_count=%u "
			 "wake_failure_count=%u pc22_active=%d "
			 "restore_count=%u watchdog_restore_count=%u restore_failure_count=%u "
			 "safe_on_verified=%d last_restore_reason=%s\n",
			 n->state != NBLC_STATE_BOOT_PRESERVE, nblc_state_name(n->state),
			 nblc_op_name(n->active_op), remaining_ms, gpc0_mode, gpc0_level,
			 n->pwm != NULL, n->pwm ? n->pwm_duty_pct : 0,
			 pwm_committed, committed_ms, asleep, wake_target, asleep_ms,
			 n->sleep_count, n->wake_count, n->wake_failure_count,
			 n->pc22_gpio != NULL,
			 n->restore_count, n->watchdog_restore_count, n->restore_failure_count,
			 n->safe_on_verified, n->last_restore_reason);
	}
}

static int nblc_cmd_status_log(struct nblc *n)
{
	mutex_lock(&n->lock);
	nblc_status_dump(n, NULL);
	mutex_unlock(&n->lock);
	return 0;
}

static ssize_t nblc_command_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct nblc *n = file->private_data;
	char buf[64];
	size_t len;
	char *p, *cmd, *rest;
	int ret;

	if (count == 0 || count >= sizeof(buf))
		return -EINVAL;
	len = count;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	p = strim(buf);
	cmd = strsep(&p, " ");
	if (!cmd || !*cmd)
		return -EINVAL;

	/* Whitelist enforcement - same discipline as
	 * nebulaos_backlight_probe_diag.c's command-interface hardening
	 * pass: every recognized command takes ZERO arguments, and any
	 * trailing token is rejected outright, not silently ignored. This
	 * is what makes it structurally impossible to smuggle an arbitrary
	 * GPIO/channel/duty/period/timeout through this interface. */
	rest = p ? strim(p) : NULL;
	if (rest && *rest)
		return -EINVAL;

	if (!strcmp(cmd, "status"))
		ret = nblc_cmd_status_log(n);
	else if (!strcmp(cmd, "enter-safe-on"))
		ret = nblc_cmd_enter_safe_on(n);
	else if (!strcmp(cmd, "safe-off-test"))
		ret = nblc_cmd_safe_off_test(n);
	else if (!strcmp(cmd, "pc22-test-low"))
		ret = nblc_cmd_pc22_test(n, 0);
	else if (!strcmp(cmd, "pc22-test-high"))
		ret = nblc_cmd_pc22_test(n, 1);
	else if (!strcmp(cmd, "pwm-active-25"))
		ret = nblc_cmd_pwm_active(n, 25);
	else if (!strcmp(cmd, "pwm-active-50"))
		ret = nblc_cmd_pwm_active(n, 50);
	else if (!strcmp(cmd, "pwm-active-75"))
		ret = nblc_cmd_pwm_active(n, 75);
	else if (!strcmp(cmd, "commit-pwm"))
		ret = nblc_cmd_commit_pwm(n);
	else if (!strcmp(cmd, "sleep"))
		ret = nblc_cmd_sleep(n);
	else if (!strcmp(cmd, "wake"))
		ret = nblc_cmd_wake(n);
	else if (!strcmp(cmd, "disarm"))
		ret = nblc_cmd_disarm(n);
	else if (!strcmp(cmd, "restore"))
		ret = nblc_cmd_restore(n);
	else
		ret = -EINVAL;

	return ret < 0 ? ret : count;
}

static const struct file_operations nblc_command_fops = {
	.open = simple_open,
	.write = nblc_command_write,
	.llseek = no_llseek,
};

static int nblc_status_show(struct seq_file *s, void *unused)
{
	struct nblc *n = s->private;

	mutex_lock(&n->lock);
	nblc_status_dump(n, s);
	mutex_unlock(&n->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(nblc_status);

/*
 * probe() - see file header item (a). Registers the platform driver, sets
 * up debugfs, initializes internal state. Deliberately, structurally NEVER
 * acquires GPC0, PC22, or PWM channel 0 - no devm_gpiod_get()/gpiod_get()/
 * devm_pwm_get()/pwm_get()/pinctrl_select_state() call of any kind appears
 * anywhere in this function. The bootloader's own GPC0 configuration is
 * left completely untouched by loading/binding this driver - this is what
 * makes this driver's presence in the kernel, before any explicit command
 * is ever issued, behaviorally identical to it not being present at all.
 */
static int nblc_probe(struct platform_device *pdev)
{
	struct nblc *n;
	struct device *dev = &pdev->dev;

	n = devm_kzalloc(dev, sizeof(*n), GFP_KERNEL);
	if (!n)
		return -ENOMEM;

	n->dev = dev;
	n->state = NBLC_STATE_BOOT_PRESERVE;
	n->active_op = NBLC_OP_NONE;
	n->pc22_initial_level = -1;
	strscpy(n->last_restore_reason, "boot", sizeof(n->last_restore_reason));
	mutex_init(&n->lock);
	INIT_DELAYED_WORK(&n->restore_work, nblc_restore_work);

	platform_set_drvdata(pdev, n);

	n->debugfs_dir = debugfs_create_dir(NBLC_NAME, NULL);
	if (IS_ERR_OR_NULL(n->debugfs_dir)) {
		dev_warn(dev, "debugfs unavailable - control interface not exposed\n");
		n->debugfs_dir = NULL;
	} else {
		debugfs_create_file("command", 0200, n->debugfs_dir, n, &nblc_command_fops);
		debugfs_create_file("status", 0444, n->debugfs_dir, n, &nblc_status_fops);
	}

	dev_info(dev, "backlight final controller ready - boot-preserve (zero hardware "
		 "claimed, bootloader's own GPC0 configuration untouched); write "
		 "\"enter-safe-on\" to .../command to begin\n");
	return 0;
}

static int nblc_remove(struct platform_device *pdev)
{
	struct nblc *n = platform_get_drvdata(pdev);

	/* Remove debugfs first so no new command can race with teardown. */
	debugfs_remove_recursive(n->debugfs_dir);
	n->debugfs_dir = NULL;

	cancel_delayed_work_sync(&n->restore_work);

	mutex_lock(&n->lock);
	if (n->active_op != NBLC_OP_NONE) {
		switch (n->active_op) {
		case NBLC_OP_SAFE_OFF_TEST:
		case NBLC_OP_PWM_ACTIVE:
		case NBLC_OP_SLEEP_TRANSITION:
		case NBLC_OP_WAKE_TRANSITION:
			nblc_converge_gpc0_safe_on_locked(n, "driver-remove");
			break;
		case NBLC_OP_PC22_TEST_LOW:
		case NBLC_OP_PC22_TEST_HIGH:
			nblc_converge_pc22_locked(n, "driver-remove");
			break;
		default:
			break;
		}
		n->active_op = NBLC_OP_NONE;
	}
	/* Converge to safe-on and release the GPIO claim, mirroring disarm -
	 * unbinding must never leave a dangling claim on GPC0. */
	if (n->state != NBLC_STATE_BOOT_PRESERVE)
		nblc_converge_gpc0_safe_on_locked(n, "driver-remove");
	if (n->gpc0_gpio) {
		gpiod_put(n->gpc0_gpio);
		n->gpc0_gpio = NULL;
	}
	mutex_unlock(&n->lock);

	return 0;
}

static const struct of_device_id nblc_of_match[] = {
	{ .compatible = "nebulaos,backlight-final-controller" },
	{ }
};
MODULE_DEVICE_TABLE(of, nblc_of_match);

static struct platform_driver nblc_driver = {
	.probe = nblc_probe,
	.remove = nblc_remove,
	.driver = {
		.name = NBLC_NAME,
		.of_match_table = nblc_of_match,
	},
};
module_platform_driver(nblc_driver);

MODULE_DESCRIPTION("NebulaOS backlight final controller (GPC0/PWM0/PC22 exclusive owner)");
MODULE_LICENSE("GPL");
