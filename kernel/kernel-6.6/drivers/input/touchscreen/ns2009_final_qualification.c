// SPDX-License-Identifier: GPL-2.0
/*
 * NS2009 touchscreen - CONFIG_TOUCHSCREEN_NS2009_FINAL_QUALIFICATION
 *
 * A NEW, separate, genuinely one-shot irq-assist qualification driver for
 * the same pendown-gpios NS2009 touch path ns2009.c already binds to -
 * ke-mainline-klipper display/touch investigation mission follow-on
 * (2026-08-02+).
 *
 * This is intentionally NOT the same feature as the existing
 * CONFIG_TOUCHSCREEN_NS2009_QUALIFICATION driver (see
 * scripts/build/patches/touch-qualification-unified.patch /
 * scripts/build/touch-qualification-variant.sh) - that driver, its patch,
 * its toggle script, and its tests are left completely untouched by this
 * work. This is a fresh implementation informed by a real, live finding
 * from that driver's own irq-observe mode: requesting the pendown GPIO IRQ
 * with a plain FALLING-edge trigger and only counting/handling events
 * (never explicitly masking the line for the full duration of a contact)
 * hit a real bounce storm on real hardware - 492 raw IRQ events across
 * only 56 touch/release cycles (~8.8 events per cycle), because GPIO79
 * bounces heavily under edge-triggered sampling during actual mechanical/
 * capacitive contact. The existing driver's storm-threshold fallback
 * caught this correctly (no missed touches, no stuck state) - but the
 * right fix is a driver that doesn't let the bounce reach the interrupt
 * controller as repeated interrupts in the first place.
 *
 * Design (see the Kconfig help text for the same summary):
 *
 *   - Boot default is poll-only, always. No GPIO IRQ is ever requested
 *     until a debugfs client explicitly writes "irq-assist" to
 *     ns2009_final_qualification/mode, so selecting this Kconfig option
 *     alone does not change boot-time behavior at all.
 *   - irq-assist is genuinely one-shot: a REAL hard-IRQ handler
 *     (ns2009_nfq_irq_handler(), NOT a NULL primary handler like the older
 *     driver's IRQF_ONESHOT/threaded-only design) masks the IRQ
 *     (disable_irq_nosync()) as the very first action, before anything
 *     else runs, in genuine hard-IRQ context. That mask stays in effect
 *     for the ENTIRE duration of the contact (not just until a threaded
 *     handler returns, which is all IRQF_ONESHOT actually guarantees -
 *     that narrower guarantee is exactly what let the older driver's
 *     bounce-storm reach it as real, separate interrupt deliveries). No
 *     further raw edge from GPIO79 can generate another interrupt at all
 *     until this driver's own release-confirmation logic explicitly
 *     re-arms it.
 *   - The threaded handler (process context, woken via IRQ_WAKE_THREAD)
 *     records exactly one logical touch-IRQ activation and schedules a
 *     work item that promotes the *existing*, unmodified
 *     ns2009_ts_poll()/ns2009_ts_report() path to its normal ~30ms
 *     cadence. No I2C transaction and no coordinate/input processing ever
 *     happens in ns2009_nfq_irq_handler() or ns2009_nfq_irq_thread() -
 *     verifiable directly by inspection (see
 *     tests/touch-final-qualification-variant-tests.sh).
 *   - Release requires GPIO79 to read HIGH (idle) for THREE consecutive
 *     poll ticks before the contact is considered over - debounces the
 *     release itself, so a bouncy release can't prematurely re-arm the
 *     IRQ. A generous bound (NS2009_NFQ_RELEASE_CONFIRM_TIMEOUT_MS, see
 *     its own comment below) guards against ever waiting indefinitely.
 *   - A slower ~250ms safety poll stays active while idle (IRQ armed, no
 *     contact) as a fallback net, same principle as the older driver.
 *
 * Linking model: this file is compiled into the SAME module/built-in
 * object as ns2009.c (see the composite ns2009-y / ns2009-$(CONFIG_...)
 * lines this feature adds to the touchscreen Makefile), not built as an
 * independent loadable module - so the two functions ns2009.c calls
 * directly here (ns2009_nfq_probe(), ns2009_nfq_on_poll()) are plain,
 * non-exported "extern" symbols, deliberately not EXPORT_SYMBOL_GPL()'d:
 * there is no cross-module boundary to cross, unlike (for comparison) the
 * genuinely separate pwm-ingenic-v2.c/nebulaos_backlight_probe_diag.c pair
 * from the display mission, which really do live in different modules.
 *
 * ns2009.c's own footprint for this feature is deliberately tiny and
 * placed at points in that file the existing
 * CONFIG_TOUCHSCREEN_NS2009_QUALIFICATION patch never touches, so the two
 * patches apply cleanly regardless of order - see
 * scripts/build/touch-final-qualification-variant.sh's header comment for
 * the full reasoning and the direct verification this project performed
 * for both apply orders.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/workqueue.h>

/* Prototypes for the two symbols ns2009.c calls directly (matching
 * declarations to the ones ns2009.c itself forward-declares at its own
 * top-of-file block) - present here purely so -Wmissing-prototypes (this
 * project's W=1 compile-test standard) has something to check these
 * definitions against; there is still no shared header and no
 * EXPORT_SYMBOL_GPL() (see the file header comment for why neither is
 * needed for this composite-object linking model). */
struct i2c_client;
struct input_dev;
struct gpio_desc;

void *ns2009_nfq_probe(struct i2c_client *client, struct input_dev *input,
			struct gpio_desc *pendown_gpio,
			unsigned int normal_poll_interval_ms);
void ns2009_nfq_on_poll(void *handle, bool pen_down);

/* Rolling-window storm threshold - same numeric threshold as the older
 * driver's own (>50 events / rolling 1s), but now counting LOGICAL
 * one-shot activations rather than raw bounce edges. Since this design
 * suppresses bounce-driven re-triggering entirely (the IRQ is physically
 * masked at the interrupt-controller level for the whole contact, so
 * bounce edges never reach the handler at all), this threshold is expected
 * to be far less likely to trip under any normal use than it was for the
 * older driver's irq-observe mode - 50 *genuine, separate contacts* inside
 * one second would require faster-than-humanly-possible repeated tapping.
 * Kept anyway as the same safety net, never removed, in case of a genuine
 * hardware fault (e.g. a stuck-oscillating pin that still manages to
 * toggle past the mask window on every single re-arm). */
#define NS2009_NFQ_STORM_WINDOW_MS		1000
#define NS2009_NFQ_STORM_THRESHOLD		50

#define NS2009_NFQ_SAFETY_POLL_INTERVAL_MS	250

/* Require the pin to read idle-HIGH on three separate, consecutive poll
 * ticks (not just one) before trusting that the contact is really over -
 * debounces the release edge itself, so a bouncy release can't prematurely
 * re-arm the IRQ mid-bounce. */
#define NS2009_NFQ_RELEASE_CONFIRM_POLLS	3

/* Bound on total time spent in "active contact, waiting for release to be
 * confirmed" before giving up and treating it as a fault (permanent
 * fallback to poll-only). Reasoning: at the ~30ms active-poll cadence,
 * three consecutive HIGH reads take a minimum of ~90ms once the pin
 * actually goes high and stays high - so this bound is not really about
 * bounding a normal release, it is about bounding a scenario where the pin
 * never settles (e.g. a stuck/damaged sensor, or a bug in this driver's
 * own bookkeeping) and the release-confirmation streak keeps getting reset
 * to zero forever. Real UI interactions on this product's touchscreen
 * (tap, drag a mesh point, press a jog button) are expected to be
 * sub-second to low-single-digit-second events; 5000ms is generous enough
 * to comfortably cover a slow drag or a deliberately long press-and-hold,
 * while still guaranteeing this driver can never hang indefinitely in the
 * active-contact state. A legitimately-held touch longer than this bound
 * will trip a conservative, one-way fallback to poll-only - safe and
 * non-fatal (poll-only continues reporting every coordinate correctly for
 * the remainder of that boot; only the IRQ-assist power/latency benefit is
 * lost), and the exact reason is recorded in fallback_reason for anyone
 * investigating a real-world trip of this bound. This is a deliberate,
 * documented conservative tradeoff, not an oversight. */
#define NS2009_NFQ_RELEASE_CONFIRM_TIMEOUT_MS	5000

/* "Repeated" immediate re-arm failure: this many consecutive safety-poll
 * ticks (each ~250ms apart) finding the pin still LOW right when this
 * driver is trying to re-arm the IRQ after a confirmed release. 5 attempts
 * (~1.25s of continuously-low readings immediately after a release was
 * just confirmed HIGH three times in a row) is far more than any
 * legitimate fast-repeat-tap scenario would produce, and points instead at
 * a genuine inconsistency between this driver's own state and the real pin
 * state. */
#define NS2009_NFQ_REARM_FAILURE_LIMIT		5

enum ns2009_nfq_mode {
	NS2009_NFQ_MODE_POLL_ONLY	= 0,
	NS2009_NFQ_MODE_IRQ_ASSIST	= 1,
};

struct ns2009_nfq_state {
	/* Set true only if the initial allocation itself failed - every
	 * other field is meaningless/unused in that case. Never re-attempted
	 * (see ns2009_nfq_probe()'s caller in ns2009.c), so this is a
	 * permanent, boot-lifetime condition, always leaving the existing,
	 * unmodified poll-only path fully unaffected. */
	bool				inert;

	struct i2c_client		*client;
	struct input_dev		*input;
	struct gpio_desc		*pendown_gpio;
	unsigned int			normal_poll_interval_ms;

	struct mutex			lock;

	enum ns2009_nfq_mode		mode;
	/* Persistent-REQUESTED mode: an in-kernel-memory field for this
	 * boot only, set/read independently of the live "mode" above. NOT
	 * itself written to disk - a separate, later piece of work (not
	 * part of this driver) is responsible for the actual on-disk
	 * config file, and is expected to read this field (or write it,
	 * to record a pending request) and then explicitly write "mode"
	 * itself once its own boot-health gates pass. This driver never
	 * reads this field to auto-apply anything - purely a handoff slot. */
	enum ns2009_nfq_mode		persistent_mode;

	bool				permanent_fallback;
	u64				fallback_count;
	const char			*fallback_reason;

	int				irq;
	bool				irq_requested;
	bool				irq_masked;
	bool				unexpected_irq_while_masked;
	int				irq_request_failure;

	u64				irq_activation_count;
	u64				touch_down_count;
	u64				release_count;
	u64				false_idle_count;
	u64				rearm_count;
	u64				rearm_failure_count;
	int				rearm_consecutive_failures;
	u64				idle_safety_poll_count;
	u64				poll_count;

	unsigned long			storm_window_start;
	unsigned int			storm_window_count;

	bool				active_contact;
	bool				active_contact_saw_touch_down;
	int				release_confirm_streak;
	unsigned long			active_contact_start_jiffies;
	unsigned long			active_contact_total_jiffies;
	u64				active_contact_spell_count;

	bool				prev_pen_down;
	bool				first_sample_pending;
	unsigned long			touch_down_to_first_sample_jiffies;

	ktime_t				irq_fire_ktime;
	s64				irq_to_worker_latency_us;

	struct work_struct		kick_work;

	struct dentry			*debugfs_dir;
};

/* Returned by ns2009_nfq_probe() if the initial allocation fails - a
 * single shared, statically-inert placeholder so every caller-facing
 * function can rely on the handle never being NULL after the first probe
 * attempt (no per-tick retry, no extra "did we already try" field needed),
 * while still guaranteeing zero behavior change if this ever happens. */
static struct ns2009_nfq_state ns2009_nfq_inert_singleton = {
	.inert = true,
};

static const char *ns2009_nfq_mode_name(enum ns2009_nfq_mode mode)
{
	switch (mode) {
	case NS2009_NFQ_MODE_POLL_ONLY:
		return "poll-only";
	case NS2009_NFQ_MODE_IRQ_ASSIST:
		return "irq-assist";
	}
	return "unknown";
}

/* Central, one-way fallback trigger - the only place permanent_fallback is
 * ever set, and the only dev_warn_ratelimited() call site for the fallback
 * path specifically (the other call site, in ns2009_nfq_probe(), covers a
 * completely separate, one-time, non-fallback condition: the initial
 * allocation itself failing). Mirrors the existing
 * CONFIG_TOUCHSCREEN_NS2009_QUALIFICATION driver's own "exactly one
 * ratelimited warning for the fallback path, never per-event log spam"
 * contract. Idempotent: a later call is a silent no-op. Always leaves the
 * existing, unmodified 30ms poll path as the sole active touch-reporting
 * mechanism. Takes the mutex itself - callers must never hold nfq->lock
 * when calling this. */
static void ns2009_nfq_trigger_fallback(struct ns2009_nfq_state *nfq, const char *reason)
{
	mutex_lock(&nfq->lock);

	if (nfq->permanent_fallback) {
		mutex_unlock(&nfq->lock);
		return;
	}

	nfq->permanent_fallback = true;
	nfq->fallback_count++;
	nfq->fallback_reason = reason;

	if (nfq->irq_requested && !nfq->irq_masked) {
		disable_irq_nosync(nfq->irq);
		nfq->irq_masked = true;
	}
	nfq->active_contact = false;
	nfq->first_sample_pending = false;
	nfq->mode = NS2009_NFQ_MODE_POLL_ONLY;

	input_set_poll_interval(nfq->input, nfq->normal_poll_interval_ms);

	mutex_unlock(&nfq->lock);

	dev_warn_ratelimited(&nfq->client->dev,
			     "final-qualification: permanent fallback to poll-only (%s) - "
			     "polling is unaffected\n", reason);
}

/* Work item scheduled from the threaded IRQ handler (process context) -
 * the ONLY place any coordinate/I2C-adjacent consequence of a touch-IRQ
 * activation happens, and even this only touches the poll *interval*.
 * ns2009_ts_report()'s own I2C reads happen entirely inside the input
 * core's own separate poll workqueue via the existing, unmodified
 * ns2009_ts_poll() - never here. */
static void ns2009_nfq_kick_work_fn(struct work_struct *work)
{
	struct ns2009_nfq_state *nfq = container_of(work, struct ns2009_nfq_state, kick_work);

	input_set_poll_interval(nfq->input, nfq->normal_poll_interval_ms);
}

/* Real hard-IRQ handler - genuinely one-shot masking, the core fix this
 * whole feature exists for. See the file header comment for the full
 * rationale versus the older driver's IRQF_ONESHOT/NULL-primary-handler
 * design. Absolutely nothing beyond disable_irq_nosync()/flag/timestamp
 * writes happens here - no mutex (hard-IRQ context can't sleep), no I2C,
 * no coordinate/input processing. */
static irqreturn_t ns2009_nfq_irq_handler(int irq, void *dev_id)
{
	struct ns2009_nfq_state *nfq = dev_id;

	if (nfq->irq_masked) {
		/* Should be impossible if masking below ever worked correctly
		 * - the whole point of masking is that the interrupt
		 * controller itself will not deliver another interrupt for
		 * this line until re-armed. Flag it for the threaded handler
		 * (process context - safe to take the mutex and drive a
		 * permanent fallback) to escalate; still mask again here
		 * defensively even though this state should be unreachable. */
		nfq->unexpected_irq_while_masked = true;
		disable_irq_nosync(irq);
		return IRQ_WAKE_THREAD;
	}

	/* THE fix: mask before literally anything else, in genuine hard-IRQ
	 * context, so no further raw bounce edge on GPIO79 can generate
	 * another interrupt at all for the rest of this contact - unlike
	 * the older driver's threaded-only/IRQF_ONESHOT design, this mask
	 * stays in effect until this driver's own release-confirmation
	 * logic (ns2009_nfq_on_poll()) explicitly re-arms it, not just
	 * until this handler's thread finishes. */
	disable_irq_nosync(irq);
	nfq->irq_masked = true;
	nfq->irq_fire_ktime = ktime_get();

	return IRQ_WAKE_THREAD;
}

/* Threaded IRQ handler - process context, safe to take the mutex. Still
 * performs ZERO I2C transfers and ZERO coordinate/input processing -
 * verifiable directly by inspection (see
 * tests/touch-final-qualification-variant-tests.sh, same awk-extract-then-
 * grep method the existing driver's own tests use). Its only jobs: record
 * exactly one logical touch-IRQ activation, run the storm check, and
 * schedule the work item that promotes the existing poll cadence. */
static irqreturn_t ns2009_nfq_irq_thread(int irq, void *dev_id)
{
	struct ns2009_nfq_state *nfq = dev_id;
	unsigned long now = jiffies;

	if (nfq->permanent_fallback)
		return IRQ_HANDLED;

	if (nfq->unexpected_irq_while_masked) {
		ns2009_nfq_trigger_fallback(nfq, "unexpected_irq_while_masked");
		return IRQ_HANDLED;
	}

	nfq->irq_to_worker_latency_us = ktime_us_delta(ktime_get(), nfq->irq_fire_ktime);
	nfq->irq_activation_count++;

	/* Storm protection - rolling window over LOGICAL one-shot
	 * activations now, not raw bounce edges (see the
	 * NS2009_NFQ_STORM_THRESHOLD comment above for why this is expected
	 * to be far less likely to trip than the older driver's own
	 * irq-observe-mode threshold of the same numeric value). */
	if (time_before(now, nfq->storm_window_start +
			msecs_to_jiffies(NS2009_NFQ_STORM_WINDOW_MS))) {
		nfq->storm_window_count++;
		if (nfq->storm_window_count > NS2009_NFQ_STORM_THRESHOLD) {
			ns2009_nfq_trigger_fallback(nfq, "irq_storm");
			return IRQ_HANDLED;
		}
	} else {
		nfq->storm_window_start = now;
		nfq->storm_window_count = 1;
	}

	mutex_lock(&nfq->lock);
	nfq->active_contact = true;
	nfq->active_contact_start_jiffies = now;
	nfq->active_contact_saw_touch_down = false;
	nfq->release_confirm_streak = 0;
	nfq->first_sample_pending = true;
	nfq->active_contact_spell_count++;
	mutex_unlock(&nfq->lock);

	if (!schedule_work(&nfq->kick_work)) {
		/* The work item was already pending - should be impossible,
		 * since the IRQ stays masked for the full duration of one
		 * contact, so a second activation can never logically overlap
		 * with an unconsumed kick from a previous one. Real fault. */
		ns2009_nfq_trigger_fallback(nfq, "touch_worker_schedule_failed");
		return IRQ_HANDLED;
	}

	return IRQ_HANDLED;
}

/* Lazily requests the pendown GPIO IRQ - only ever called from
 * ns2009_nfq_set_mode()'s IRQ_ASSIST case, never at probe/first-poll time,
 * so selecting this Kconfig option alone (without ever writing
 * "irq-assist" to debugfs) never requests any IRQ at all. Best-effort:
 * any failure here is recorded and left non-fatal to the caller. */
static int ns2009_nfq_request_irq(struct ns2009_nfq_state *nfq)
{
	int irq;
	int error;

	if (!nfq->pendown_gpio) {
		nfq->irq_request_failure = -ENODEV;
		return -ENODEV;
	}

	irq = gpiod_to_irq(nfq->pendown_gpio);
	if (irq < 0) {
		nfq->irq_request_failure = irq;
		return irq;
	}

	nfq->irq = irq;
	nfq->storm_window_start = jiffies;
	nfq->storm_window_count = 0;

	/* FALLING edge only - PROVEN_FROM_LIVE_TEST GPIO79 is
	 * IDLE_HIGH_ACTIVE_LOW (see
	 * docs/NEBULAOS_TOUCH_IRQ_TRIGGER_FINDINGS.md). No IRQF_ONESHOT here
	 * - this driver's own explicit disable_irq_nosync()/enable_irq()
	 * pair in the hard handler and the release-confirmation path already
	 * provide a strictly stronger guarantee for the whole contact, so
	 * relying on IRQF_ONESHOT's own narrower auto-remask-until-thread-
	 * completes behavior would be redundant at best. */
	error = devm_request_threaded_irq(&nfq->client->dev, irq,
					   ns2009_nfq_irq_handler,
					   ns2009_nfq_irq_thread,
					   IRQF_TRIGGER_FALLING,
					   "ns2009-final-qualification", nfq);
	if (error) {
		nfq->irq_request_failure = error;
		return error;
	}

	nfq->irq_requested = true;
	nfq->irq_masked = false;
	dev_info(&nfq->client->dev, "final-qualification: IRQ %d active for irq-assist\n", irq);
	return 0;
}

/* The only place nfq->mode actually changes. Returns 0 on success; a
 * negative errno on rejection (never applied) or on a genuine internal
 * failure while carrying out a legitimate request (in which case a
 * permanent fallback has also been triggered). */
static int ns2009_nfq_set_mode(struct ns2009_nfq_state *nfq, enum ns2009_nfq_mode requested)
{
	int error;

	mutex_lock(&nfq->lock);

	if (nfq->permanent_fallback) {
		mutex_unlock(&nfq->lock);
		return requested == NS2009_NFQ_MODE_POLL_ONLY ? 0 : -EPERM;
	}

	if (requested == nfq->mode) {
		mutex_unlock(&nfq->lock);
		return 0;
	}

	switch (requested) {
	case NS2009_NFQ_MODE_POLL_ONLY:
		if (nfq->irq_requested && !nfq->irq_masked) {
			disable_irq_nosync(nfq->irq);
			nfq->irq_masked = true;
		}
		nfq->active_contact = false;
		nfq->first_sample_pending = false;
		input_set_poll_interval(nfq->input, nfq->normal_poll_interval_ms);
		break;

	case NS2009_NFQ_MODE_IRQ_ASSIST:
		if (!nfq->irq_requested) {
			error = ns2009_nfq_request_irq(nfq);
			if (error) {
				mutex_unlock(&nfq->lock);
				ns2009_nfq_trigger_fallback(nfq, "irq_request_failed");
				return -EIO;
			}
		} else if (nfq->irq_masked) {
			enable_irq(nfq->irq);
			nfq->irq_masked = false;
		}
		nfq->active_contact = false;
		nfq->first_sample_pending = false;
		nfq->release_confirm_streak = 0;
		nfq->rearm_consecutive_failures = 0;
		input_set_poll_interval(nfq->input, NS2009_NFQ_SAFETY_POLL_INTERVAL_MS);
		break;
	}

	nfq->mode = requested;
	mutex_unlock(&nfq->lock);
	return 0;
}

/* Zeroes cumulative counters only - deliberately leaves mode,
 * permanent-fallback state/reason, persistent_mode, and any live
 * transition-tracking fields (active_contact, irq_requested/masked,
 * rearm_consecutive_failures) untouched. Same non-fabrication rationale as
 * the existing CONFIG_TOUCHSCREEN_NS2009_QUALIFICATION driver's own reset:
 * resetting those could fabricate a false transition/duration measurement,
 * or silently hide that a fallback already happened. */
static void ns2009_nfq_reset_counters(struct ns2009_nfq_state *nfq)
{
	mutex_lock(&nfq->lock);
	nfq->irq_activation_count = 0;
	nfq->touch_down_count = 0;
	nfq->release_count = 0;
	nfq->false_idle_count = 0;
	nfq->rearm_count = 0;
	nfq->rearm_failure_count = 0;
	nfq->idle_safety_poll_count = 0;
	nfq->poll_count = 0;
	nfq->storm_window_count = 0;
	nfq->active_contact_spell_count = 0;
	nfq->active_contact_total_jiffies = 0;
	nfq->touch_down_to_first_sample_jiffies = 0;
	nfq->irq_to_worker_latency_us = 0;
	mutex_unlock(&nfq->lock);
}

/* Called once per poll tick (from ns2009_ts_poll(), after the existing,
 * unmodified ns2009_ts_report() has already run and updated data->pen_down
 * - passed in as pen_down here rather than read back from struct
 * ns2009_data directly, so this file never needs that struct's layout at
 * all). Detects touch-down/release transitions itself by comparing against
 * its own previously-observed state, and drives the release-confirmation /
 * re-arm / idle-safety-poll state machine while in irq-assist mode. */
void ns2009_nfq_on_poll(void *handle, bool pen_down)
{
	struct ns2009_nfq_state *nfq = handle;
	unsigned long now;
	int level;

	if (!nfq || nfq->inert)
		return;

	mutex_lock(&nfq->lock);

	nfq->poll_count++;

	if (nfq->permanent_fallback) {
		mutex_unlock(&nfq->lock);
		return;
	}

	now = jiffies;

	if (nfq->first_sample_pending) {
		nfq->touch_down_to_first_sample_jiffies = now - nfq->active_contact_start_jiffies;
		nfq->first_sample_pending = false;
	}

	if (pen_down && !nfq->prev_pen_down) {
		nfq->touch_down_count++;
		if (nfq->active_contact)
			nfq->active_contact_saw_touch_down = true;
	} else if (!pen_down && nfq->prev_pen_down) {
		nfq->release_count++;
	}
	nfq->prev_pen_down = pen_down;

	if (nfq->mode != NS2009_NFQ_MODE_IRQ_ASSIST) {
		mutex_unlock(&nfq->lock);
		return;
	}

	if (nfq->active_contact) {
		if (time_after(now, nfq->active_contact_start_jiffies +
				msecs_to_jiffies(NS2009_NFQ_RELEASE_CONFIRM_TIMEOUT_MS))) {
			mutex_unlock(&nfq->lock);
			ns2009_nfq_trigger_fallback(nfq, "release_confirmation_timeout");
			return;
		}

		level = gpiod_get_raw_value_cansleep(nfq->pendown_gpio);
		if (level == 1)
			nfq->release_confirm_streak++;
		else
			nfq->release_confirm_streak = 0;

		if (nfq->release_confirm_streak >= NS2009_NFQ_RELEASE_CONFIRM_POLLS) {
			nfq->active_contact_total_jiffies += now - nfq->active_contact_start_jiffies;
			if (!nfq->active_contact_saw_touch_down)
				nfq->false_idle_count++;
			nfq->active_contact = false;
			/* "Clear any pending/stale interrupt state on the
			 * line" - this driver's honest implementation of that
			 * requirement: there is no generic, portable API from
			 * a GPIO-IRQ consumer to poke the interrupt
			 * controller's own pending/latched status directly, so
			 * instead of fabricating such a call, re-arming
			 * (below, on the next idle-safety-poll tick) always
			 * re-checks the raw level first and refuses to enable
			 * the IRQ while it disagrees - functionally
			 * equivalent to a real "clear stale state" step
			 * without pretending to a register poke this driver
			 * cannot honestly perform. */
			input_set_poll_interval(nfq->input, NS2009_NFQ_SAFETY_POLL_INTERVAL_MS);
		}
	} else {
		nfq->idle_safety_poll_count++;

		if (nfq->irq_masked) {
			level = gpiod_get_raw_value_cansleep(nfq->pendown_gpio);
			if (level == 1) {
				enable_irq(nfq->irq);
				nfq->irq_masked = false;
				nfq->rearm_count++;
				nfq->rearm_consecutive_failures = 0;
			} else {
				nfq->rearm_failure_count++;
				nfq->rearm_consecutive_failures++;
				if (nfq->rearm_consecutive_failures >= NS2009_NFQ_REARM_FAILURE_LIMIT) {
					mutex_unlock(&nfq->lock);
					ns2009_nfq_trigger_fallback(nfq, "repeated_rearm_failure");
					return;
				}
			}
		}
	}

	mutex_unlock(&nfq->lock);
}

static int ns2009_nfq_status_show(struct seq_file *s, void *unused)
{
	struct ns2009_nfq_state *nfq = s->private;

	if (nfq->inert) {
		seq_puts(s, "inert: 1 (initial allocation failed - feature permanently "
			    "disabled, unmodified poll-only reporting is unaffected)\n");
		return 0;
	}

	mutex_lock(&nfq->lock);
	seq_printf(s, "mode: %s\n", ns2009_nfq_mode_name(nfq->mode));
	seq_printf(s, "persistent_mode: %s\n", ns2009_nfq_mode_name(nfq->persistent_mode));
	seq_puts(s, "persistent_mode_note: in-kernel-memory only for this boot, NOT itself "
		    "written to disk - a later, separate userspace mechanism reads/writes "
		    "this field after its own boot-health gates pass, then explicitly "
		    "writes 'mode' itself\n");
	seq_printf(s, "permanent_fallback: %d\n", nfq->permanent_fallback);
	seq_printf(s, "fallback_count: %llu\n", nfq->fallback_count);
	seq_printf(s, "fallback_reason: %s\n", nfq->fallback_reason ?: "none");
	seq_printf(s, "irq_requested: %d\n", nfq->irq_requested);
	seq_printf(s, "irq: %d\n", nfq->irq_requested ? nfq->irq : -1);
	seq_printf(s, "irq_masked: %d\n", nfq->irq_masked);
	seq_printf(s, "irq_request_failure: %d\n", nfq->irq_request_failure);
	seq_printf(s, "irq_activation_count: %llu\n", nfq->irq_activation_count);
	seq_puts(s, "suppressed_bounce_count: 0 (not fabricated - not measurable by this "
		    "driver: the IRQ is masked at the interrupt-controller level, in "
		    "hard-IRQ context, immediately after the first edge, before any bounce "
		    "could occur, so further physical bounce edges during a held contact "
		    "never generate an interrupt at all; there is no generic, portable API "
		    "from a GPIO-IRQ consumer driver to query how many edges the "
		    "controller saw while masked, so this always reads 0)\n");
	seq_printf(s, "touch_down_count: %llu\n", nfq->touch_down_count);
	seq_printf(s, "release_count: %llu\n", nfq->release_count);
	seq_printf(s, "false_idle_count: %llu\n", nfq->false_idle_count);
	seq_printf(s, "rearm_count: %llu\n", nfq->rearm_count);
	seq_printf(s, "rearm_failure_count: %llu\n", nfq->rearm_failure_count);
	seq_printf(s, "irq_to_worker_latency_us: %lld\n", nfq->irq_to_worker_latency_us);
	seq_printf(s, "touch_down_to_first_sample_latency_ms: %u\n",
		   jiffies_to_msecs(nfq->touch_down_to_first_sample_jiffies));
	seq_printf(s, "active_contact: %d\n", nfq->active_contact);
	seq_printf(s, "active_contact_spell_count: %llu\n", nfq->active_contact_spell_count);
	seq_printf(s, "active_contact_total_duration_ms: %u\n",
		   jiffies_to_msecs(nfq->active_contact_total_jiffies));
	seq_printf(s, "idle_safety_poll_count: %llu\n", nfq->idle_safety_poll_count);
	seq_printf(s, "poll_count: %llu\n", nfq->poll_count);
	seq_printf(s, "storm_window_count: %u\n", nfq->storm_window_count);
	mutex_unlock(&nfq->lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ns2009_nfq_status);

static ssize_t ns2009_nfq_mode_read(struct file *file, char __user *ubuf,
				     size_t count, loff_t *ppos)
{
	struct ns2009_nfq_state *nfq = file->private_data;
	char buf[16];
	int len;

	len = scnprintf(buf, sizeof(buf), "%s\n", ns2009_nfq_mode_name(nfq->mode));
	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

/* Constrained command interface: accepts EXACTLY "poll-only", "irq-assist",
 * or "reset-counters" - anything else is rejected with -EINVAL. No raw or
 * arbitrary IRQ control of any kind is exposed anywhere in this driver. */
static ssize_t ns2009_nfq_mode_write(struct file *file, const char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	struct ns2009_nfq_state *nfq = file->private_data;
	char buf[24];
	size_t len = min(count, sizeof(buf) - 1);
	int error;

	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	strim(buf);

	if (!strcmp(buf, "poll-only")) {
		error = ns2009_nfq_set_mode(nfq, NS2009_NFQ_MODE_POLL_ONLY);
	} else if (!strcmp(buf, "irq-assist")) {
		error = ns2009_nfq_set_mode(nfq, NS2009_NFQ_MODE_IRQ_ASSIST);
	} else if (!strcmp(buf, "reset-counters")) {
		ns2009_nfq_reset_counters(nfq);
		error = 0;
	} else {
		return -EINVAL;
	}

	if (error)
		return error;
	return count;
}

static const struct file_operations ns2009_nfq_mode_fops = {
	.open = simple_open,
	.read = ns2009_nfq_mode_read,
	.write = ns2009_nfq_mode_write,
	.llseek = default_llseek,
};

static ssize_t ns2009_nfq_persistent_mode_read(struct file *file, char __user *ubuf,
						size_t count, loff_t *ppos)
{
	struct ns2009_nfq_state *nfq = file->private_data;
	char buf[16];
	int len;

	len = scnprintf(buf, sizeof(buf), "%s\n", ns2009_nfq_mode_name(nfq->persistent_mode));
	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

/* "persistent_mode" is deliberately a SEPARATE file from "mode": writing it
 * only ever records a requested mode for a later, separate userspace
 * mechanism to read - it never itself changes live behavior. Accepts
 * exactly "poll-only" or "irq-assist" (no "reset-counters" here - nothing
 * to reset); anything else is rejected with -EINVAL. */
static ssize_t ns2009_nfq_persistent_mode_write(struct file *file, const char __user *ubuf,
						 size_t count, loff_t *ppos)
{
	struct ns2009_nfq_state *nfq = file->private_data;
	char buf[16];
	size_t len = min(count, sizeof(buf) - 1);

	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	strim(buf);

	mutex_lock(&nfq->lock);
	if (!strcmp(buf, "poll-only")) {
		nfq->persistent_mode = NS2009_NFQ_MODE_POLL_ONLY;
	} else if (!strcmp(buf, "irq-assist")) {
		nfq->persistent_mode = NS2009_NFQ_MODE_IRQ_ASSIST;
	} else {
		mutex_unlock(&nfq->lock);
		return -EINVAL;
	}
	mutex_unlock(&nfq->lock);

	return count;
}

static const struct file_operations ns2009_nfq_persistent_mode_fops = {
	.open = simple_open,
	.read = ns2009_nfq_persistent_mode_read,
	.write = ns2009_nfq_persistent_mode_write,
	.llseek = default_llseek,
};

/* Lazily called from ns2009_ts_poll() on its first-ever invocation (input
 * registration has already succeeded by the time any poll tick can run, so
 * `input` is always valid here). Defaults to poll-only, requests no IRQ,
 * and can never affect ns2009_ts_probe()'s own success (it isn't called
 * from probe() at all). */
void *ns2009_nfq_probe(struct i2c_client *client, struct input_dev *input,
			struct gpio_desc *pendown_gpio,
			unsigned int normal_poll_interval_ms)
{
	struct ns2009_nfq_state *nfq;

	nfq = devm_kzalloc(&client->dev, sizeof(*nfq), GFP_KERNEL);
	if (!nfq) {
		dev_warn_ratelimited(&client->dev,
				     "final-qualification: allocation failed, "
				     "feature permanently inert - polling is unaffected\n");
		return &ns2009_nfq_inert_singleton;
	}

	nfq->client = client;
	nfq->input = input;
	nfq->pendown_gpio = pendown_gpio;
	nfq->normal_poll_interval_ms = normal_poll_interval_ms;
	mutex_init(&nfq->lock);
	nfq->mode = NS2009_NFQ_MODE_POLL_ONLY;
	nfq->persistent_mode = NS2009_NFQ_MODE_POLL_ONLY;
	nfq->fallback_reason = "none";
	INIT_WORK(&nfq->kick_work, ns2009_nfq_kick_work_fn);

	nfq->debugfs_dir = debugfs_create_dir("ns2009_final_qualification", NULL);
	if (IS_ERR_OR_NULL(nfq->debugfs_dir)) {
		nfq->debugfs_dir = NULL;
		return nfq;
	}
	debugfs_create_file("status", 0444, nfq->debugfs_dir, nfq, &ns2009_nfq_status_fops);
	debugfs_create_file("mode", 0644, nfq->debugfs_dir, nfq, &ns2009_nfq_mode_fops);
	debugfs_create_file("persistent_mode", 0644, nfq->debugfs_dir, nfq,
			     &ns2009_nfq_persistent_mode_fops);

	return nfq;
}
