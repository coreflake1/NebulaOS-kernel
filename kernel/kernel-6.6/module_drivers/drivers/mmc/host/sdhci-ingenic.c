#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_address.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>

#include <linux/mmc/host.h>
#include <soc/cpm.h>
#include <linux/mmc/core.h>
#include <linux/mmc/slot-gpio.h>
#include "sdhci.h"
#include "sdhci-ingenic.h"

/* OpenKE (2026-07-22, FIRMWARE.md sec 46): mmc_power_up()/mmc_power_off() are real,
 * non-static, built-in (CONFIG_MMC=y) mmc-core functions - just declared in the
 * core subsystem's own *private* drivers/mmc/core/core.h, not the public
 * include/linux/mmc/core.h, so they need a manual extern here rather than an
 * include. No EXPORT_SYMBOL needed since this driver is built directly into
 * vmlinux (CONFIG_MMC_SDHCI_INGENIC=y), not a loadable module. */
extern void mmc_power_up(struct mmc_host *host, u32 ocr);
extern void mmc_power_off(struct mmc_host *host);

#define CLK_CTRL
/* Software redefinition caps */
#define CAPABILITIES1_SW    0x276dc898
#define CAPABILITIES2_SW    0

static LIST_HEAD(manual_list);

#ifdef CONFIG_SOC_X2500
	#define CPM_MSC0_CLK_R      (0xB0000068)
	#define CPM_MSC1_CLK_R      (0xB000006c)
#else
	#define CPM_MSC0_CLK_R      (0xB0000068)
	#define CPM_MSC1_CLK_R      (0xB00000a4)
	#define CPM_MSC2_CLK_R      (0xB00000a8)
#endif

#ifdef CONFIG_FPGA_TEST
#define MSC_CLK_H_FREQ      (0x1 << 20)

static void sdhci_ingenic_fpga_clk(unsigned int clock)
{
#define CPM_MSC_CLK_R CPM_MSC0_CLK_R
	//#define CPM_MSC_CLK_R CPM_MSC1_CLK_R
	unsigned int val;

	if (500000 <= clock) {
		val = readl((const volatile void *)CPM_MSC_CLK_R);
		val |= MSC_CLK_H_FREQ;
		writel(val, (void *)CPM_MSC_CLK_R);
	} else {
		val = readl((const volatile void *)CPM_MSC_CLK_R);
		val &= ~MSC_CLK_H_FREQ;
		writel(val, (void *)CPM_MSC_CLK_R);
	}
	printk("\tclk=%d, CPM_MSC0_CLK_R: %08x\n\n", clock, readl((const volatile void *)CPM_MSC0_CLK_R));
}
#endif

static unsigned int sdhci_ingenic_get_cpm_msc(struct sdhci_host *host)
{
	char msc_ioaddr[16];
	unsigned int cpm_msc;
	sprintf(msc_ioaddr, "0x%x", (unsigned int)host->ioaddr);
#ifdef CONFIG_SOC_X2500
	if (!strcmp(msc_ioaddr, "0xb3450000")) {
		cpm_msc = CPM_MSC0_CLK_R;
	}
	if (!strcmp(msc_ioaddr, "0xb3070000")) {
		cpm_msc = CPM_MSC1_CLK_R;
	}
#else
	if (!strcmp(msc_ioaddr, "0xb3450000")) {
		cpm_msc = CPM_MSC0_CLK_R;
	}
	if (!strcmp(msc_ioaddr, "0xb3460000")) {
		cpm_msc = CPM_MSC1_CLK_R;
	}
	if (!strcmp(msc_ioaddr, "0xb3490000")) {
		cpm_msc = CPM_MSC2_CLK_R;
	}
#endif
	return cpm_msc;

}

/**
 * sdhci_ingenic_msc_tuning  Enable msc controller tuning
 *
 * Tuning rx phase
 * */
static void sdhci_ingenic_en_msc_tuning(struct sdhci_host *host, unsigned int cpm_msc)
{
	if (host->mmc->ios.timing & MMC_TIMING_UHS_SDR50 ||
	    host->mmc->ios.timing & MMC_TIMING_UHS_SDR104 ||
	    host->mmc->ios.timing & MMC_TIMING_MMC_HS400) {
		*(volatile unsigned int *)cpm_msc &= ~(0x1 << 20);
	}
}

static void sdhci_ingenic_sel_rx_phase(unsigned int cpm_msc)
{
	*(volatile unsigned int *)cpm_msc |= (0x1 << 20); // default

	*(volatile unsigned int *)cpm_msc &= ~(0x7 << 17);
	*(volatile unsigned int *)cpm_msc |= (0x7 << 17);
}

static void sdhci_ingenic_sel_tx_phase(unsigned int cpm_msc)
{
	*(volatile unsigned int *)cpm_msc &= ~(0x3 << 15);
	/*  *(volatile unsigned int*)cpm_msc |= (0x2 << 15); // 180  100M OK*/
	*(volatile unsigned int *)cpm_msc |= (0x3 << 15);
}

/**
 * sdhci_ingenic_set_clock - callback on clock change
 * @host: The SDHCI host being changed
 * @clock: The clock rate being requested.
 *
 * When the card's clock is going to be changed, look at the new frequency
 * and find the best clock source to go with it.
*/
static void sdhci_ingenic_set_clock(struct sdhci_host *host, unsigned int clock)
{
#ifndef CONFIG_FPGA_TEST
	struct sdhci_ingenic *sdhci_ing = sdhci_priv(host);
	unsigned int cpm_msc = sdhci_ingenic_get_cpm_msc(host);
	char clkname[16];

	if (clock == 0) {
		return ;
	}

	sdhci_set_clock(host, clock);
	sprintf(clkname, "mux_msc%d", sdhci_ing->pdev->id);
	sdhci_ing->clk_mux = clk_get(NULL, clkname);

	if (clock > 400000) {
		clk_set_parent(sdhci_ing->clk_mux, sdhci_ing->clk_mpll);
	} else {
		clk_set_parent(sdhci_ing->clk_mux, sdhci_ing->clk_ext);
		/* OpenKE (2026-07-22, FIRMWARE.md sec 44): real vendor-source copy-paste bug,
		 * not a devicetree issue - this unconditionally hit MSC0's own clock control
		 * register (0xB0000068 == CPM_MSC0_CLK_R, #define'd above) regardless of which
		 * controller instance is actually being clocked, even though the correct,
		 * per-instance register address was already computed two lines above via
		 * sdhci_ingenic_get_cpm_msc(host) into `cpm_msc` and then never used here. This
		 * runs on every low-speed (<=400kHz) clock setup - i.e. every single card
		 * identification attempt, on every controller. MSC0 (eMMC) incidentally still
		 * worked since it's the one actually being hit; MSC1 (this board's WiFi SDIO)
		 * silently never got its own register write, on top of five other real,
		 * independently-verified fixes (gpio pin, polarity, uart4 pin-mux conflict,
		 * vmmc-supply power model, rtc32k clock enable) that all left "mmc1: Failed to
		 * initialize a non-removable card" completely unchanged - this is the first
		 * candidate that could explain the SDIO clock line itself never toggling
		 * correctly at the hardware level, independent of every gpio/power fix tried
		 * so far. */
		*(volatile unsigned int *)cpm_msc |= 1 << 21;
	}

	clk_set_rate(sdhci_ing->clk_cgu, clock);

	//  printk("%s, set clk: %d, get_clk_rate=%ld\n", __func__, clock, clk_get_rate(sdhci_ing->clk_cgu));

	if (host->mmc->ios.timing == MMC_TIMING_MMC_HS200 ||
	    host->mmc->ios.timing == MMC_TIMING_UHS_SDR104) {

		/* RX phase selecte */
		if (sdhci_ing->pdata->enable_cpm_rx_tuning == 1) {
			sdhci_ingenic_sel_rx_phase(cpm_msc);
		} else {
			sdhci_ingenic_en_msc_tuning(host, cpm_msc);
		}
		/* TX phase selecte */
		if (sdhci_ing->pdata->enable_cpm_tx_tuning == 1) {
			sdhci_ingenic_sel_tx_phase(cpm_msc);
		}
	}
#else //CONFIG_FPGA_TEST
	sdhci_ingenic_fpga_clk(clock);
#endif
}

/* I/O Driver Strength Types */
#define INGENIC_TYPE_0  0x0     //30
#define INGENIC_TYPE_1  0x1     //50
#define INGENIC_TYPE_2  0x2     //66
#define INGENIC_TYPE_3  0x3     //100

#if (!defined(CONFIG_SOC_X2500))
void sdhci_ingenic_voltage_switch(struct sdhci_host *host)
{
	struct sdhci_ingenic *sdhci_ing = sdhci_priv(host);
	struct sdhci_ingenic_pdata *pdata = sdhci_ing->pdata;
	unsigned int val;

	if (IS_ERR(pdata->sdr_v18)) {
		return;
	}

	if (host->flags & SDHCI_SIGNALING_180) {
		val = cpm_inl(CPM_EXCLK_DS) | (1 << 31);
		cpm_outl(val, CPM_EXCLK_DS);
		/*Set up hardware circuit 1.8V*/
		gpiod_set_value(pdata->sdr_v18, 1);
	} else {
		val = cpm_inl(CPM_EXCLK_DS) & ~(1 << 31);
		cpm_outl(val, CPM_EXCLK_DS);
		/*Set up hardware circuit 3V*/
		gpiod_set_value(pdata->sdr_v18, 0);
	}

}
#endif

void sdhci_ingenic_power_set(struct sdhci_host *host, int onoff)
{
	struct sdhci_ingenic *sdhci_ing = sdhci_priv(host);
	struct sdhci_ingenic_pdata *pdata = sdhci_ing->pdata;

	if (!IS_ERR(pdata->gpio->pwr)) {
		if (onoff) {
			gpiod_set_value(pdata->gpio->pwr, 1);
		} else {
			gpiod_set_value(pdata->gpio->pwr, 0);
		}
	} else {
		return ;
	}
}

void ingenic_sdhci_hwreset(struct sdhci_host *host)
{
	struct sdhci_ingenic *sdhci_ing = sdhci_priv(host);
	struct sdhci_ingenic_pdata *pdata = sdhci_ing->pdata;

	if (!IS_ERR(pdata->gpio->rst)) {
		gpiod_set_value(pdata->gpio->rst, 1);
		udelay(10);
		gpiod_set_value(pdata->gpio->rst, 0);
		usleep_range(300, 500);
	} else {
		return ;
	}

}

#define BOUNDARY_OK(addr, len) \
	((addr | (SZ_128M - 1)) == ((addr + len - 1) | (SZ_128M - 1)))

/*
 * If DMA addr spans 128MB boundary, we split the DMA transfer into two
 * so that each DMA transfer doesn't exceed the boundary.
 */
static void ingenic_sdhci_adma_write_desc(struct sdhci_host *host, void **desc,
        dma_addr_t addr, int len, unsigned int cmd)
{
	int tmplen, offset;

	if (likely(!len || BOUNDARY_OK(addr, len))) {
		sdhci_adma_write_desc(host, desc, addr, len, cmd);
		return;
	}

	offset = addr & (SZ_128M - 1);
	tmplen = SZ_128M - offset;
	sdhci_adma_write_desc(host, desc, addr, tmplen, cmd);

	addr += tmplen;
	len -= tmplen;
	sdhci_adma_write_desc(host, desc, addr, len, cmd);
}

static struct sdhci_ops sdhci_ingenic_ops = {
	.set_clock          = sdhci_ingenic_set_clock,
	.set_bus_width          = sdhci_set_bus_width,
	.reset              = sdhci_reset,
	.set_uhs_signaling      = sdhci_set_uhs_signaling,
	//  .power_set          = sdhci_ingenic_power_set,
	.hw_reset           = ingenic_sdhci_hwreset,
	.adma_write_desc        = ingenic_sdhci_adma_write_desc,
#if (!defined(CONFIG_SOC_X2500) && !defined(CONFIG_SOC_X2600) && !defined(CONFIG_SOC_AD100))
	.voltage_switch         = sdhci_ingenic_voltage_switch,
#endif
};

#ifdef CONFIG_OF
static inline void ingenic_mmc_clk_onoff(struct sdhci_ingenic *ingenic_ing, unsigned int on)
{
	if (on) {
		clk_prepare_enable(ingenic_ing->clk_cgu);
		clk_prepare_enable(ingenic_ing->clk_gate);
	} else {
		clk_disable_unprepare(ingenic_ing->clk_cgu);
		clk_disable_unprepare(ingenic_ing->clk_gate);
	}
}

/* OpenKE (2026-07-22, FIRMWARE.md sec 49): temporary paired-trace
 * instrumentation for the stock-vs-custom CMD5 comparison. Polls the same
 * raw MSC1 SDHCI register block (via the host's own host->ioaddr, already
 * mapped by probe) and the CPM MSC1 clock-divider register (same physical
 * address 0x100000a4 a stock-side /dev/mem sampler already captured) for a
 * bounded 2s window right after mmc_detect_change() is kicked off, logging
 * only changed words with a millisecond timestamp - same format/offsets as
 * the stock capture, for a direct diff.
 *
 * Gated behind msc1_trace (default off). NebulaOS BOOT-06: the whole 2 s
 * window now only runs with msc1_trace=1. It ran synchronously from
 * openke_wifi_manual_insert()'s late_initcall on every boot, so the kernel
 * waited 2 s before mounting root while the card was already detected
 * (~0.45 s after the trigger) and brcmfmac (built-in firmware) was up - the
 * detection itself is mmc_detect_change()'s asynchronous rescan and is not
 * affected. msc1_trace=1 still traces exactly as before, including the
 * first SDHCI_RESPONSE_0 change. */
static bool msc1_trace;
module_param(msc1_trace, bool, 0644);
MODULE_PARM_DESC(msc1_trace, "log every MSC1 SDHCI register change during manual insert (default: off, only report the first SDHCI_RESPONSE_0 change)");

/* OpenKE (2026-07-22, FIRMWARE.md sec 50): WL_REG_ON (gpd 4 / PD04) is
 * requested exclusively by the mmc-pwrseq-simple platform driver via
 * reset-gpios, so sdhci-ingenic.c can't devm_gpiod_get() it itself without
 * an -EBUSY conflict. gpio_get_value() is the legacy global-number gpiolib
 * accessor (the same one /sys/kernel/debug/gpio itself uses) - it only
 * reads, doesn't need ownership, so it can observe the real live pin state
 * without touching how pwrseq_simple owns/drives it. 100 is this pin's real
 * global gpio number, confirmed live via "gpio-100 (|reset ) out lo ACTIVE
 * LOW" in /sys/kernel/debug/gpio on this exact board (gpiochip3 GPIOs
 * 96-127 = GPD, +4 = PD04). */
#define OPENKE_WL_REG_ON_GPIO 100

static void openke_msc1_trace(struct sdhci_host *host)
{
	void __iomem *cpm_msc1cdr;
	u32 prev[25];
	u32 prev_cdr = 0;
	int prev_gpio = -1;
	ktime_t t0;
	bool first = true;
	bool response_seen = false;
	int i;
	s64 ms;

	cpm_msc1cdr = ioremap(0x100000a4, 4);
	if (!cpm_msc1cdr) {
		pr_err("openke_msc1_trace: ioremap CPM_MSC1CDR failed\n");
		return;
	}

	memset(prev, 0, sizeof(prev));
	t0 = ktime_get();
	if (msc1_trace) {
		pr_info("openke_msc1_trace: start\n");
	}

	while (ktime_ms_delta(ktime_get(), t0) < 2000) {
		ms = ktime_ms_delta(ktime_get(), t0);
		{
			int gpio_val = gpio_get_value(OPENKE_WL_REG_ON_GPIO);

			if (first || gpio_val != prev_gpio) {
				pr_info("openke_msc1_trace: %6lld ms WL_REG_ON (gpio %d) = %d\n",
					ms, OPENKE_WL_REG_ON_GPIO, gpio_val);
			}
			prev_gpio = gpio_val;
		}
		for (i = 0; i < 25; i++) {
			u32 v = readl(host->ioaddr + i * 4);

			if (i == 4 && !first && v != prev[i] && !response_seen) {
				/* offset 0x10 = SDHCI_RESPONSE_0 - always report the
				 * first real response, trace or not. */
				pr_info("openke_msc1_trace: %6lld ms first SDHCI_RESPONSE_0 change: 0x%08x -> 0x%08x\n",
					ms, prev[i], v);
				response_seen = true;
			}
			if (msc1_trace && (first || v != prev[i])) {
				pr_info("openke_msc1_trace: %6lld ms MSC1+0x%02x = 0x%08x\n",
					ms, i * 4, v);
			}
			prev[i] = v;
		}
		if (msc1_trace) {
			u32 cdr = readl(cpm_msc1cdr);

			if (first || cdr != prev_cdr) {
				pr_info("openke_msc1_trace: %6lld ms CPM_MSC1CDR = 0x%08x\n",
					ms, cdr);
			}
			prev_cdr = cdr;
		}
		first = false;
		usleep_range(200, 300);
	}
	if (msc1_trace) {
		pr_info("openke_msc1_trace: done\n");
	}
	if (!response_seen) {
		pr_info("openke_msc1_trace: no SDHCI_RESPONSE_0 change across the whole window - chip never answered\n");
	}
	iounmap(cpm_msc1cdr);
}

/**
 *  ingenic_mmc_manual_detect - insert or remove card manually
 *  @index: host->index, namely the index of the controller.
 *  @on: 1 means insert card, 0 means remove card.
 *
 *  This functions will be called by manually card-detect driver such as
 *  wifi. To enable this mode you can set value pdata.removal = MANUAL.
 *
 *  OpenKE (2026-07-22, FIRMWARE.md sec 52): sec 47 briefly renamed this to
 *  sdhci_ingenic_mmc_manual_detect to dodge a link collision with
 *  ingenic_mmc.c's own function of this name, when msc1 was (wrongly)
 *  switched to that driver. Reverted - disassembling stock's real, live
 *  soc_msc.ko proved stock's actual msc1 driver is this file (sdhci-ingenic.c),
 *  with its own genuine jzmmc_clk_ctrl/jzmmc_manual_detect exports; ingenic_mmc.c
 *  was never stock's msc1 driver, just a separate, unrelated driver that
 *  happens to define functions with the same names. CONFIG_INGENIC_MMC is
 *  disabled again, so this is the only definition in the tree once more, and
 *  ingenic_sdio.c's manual-insert glue calls it by this exact literal name. */
int ingenic_mmc_manual_detect(int index, int on)
{
	struct sdhci_ingenic *sdhci_ing;
	struct sdhci_host *host;
	struct list_head *pos;

	list_for_each(pos, &manual_list) {
		sdhci_ing = list_entry(pos, struct sdhci_ingenic, list);
		if (sdhci_ing->pdev->id == index) {
			break;
		} else {
			sdhci_ing = NULL;
		}
	}

	if (!sdhci_ing) {
		printk("no manual card detect\n");
		return -1;
	}

	host = sdhci_ing->host;

	if (on) {
		dev_err(&sdhci_ing->pdev->dev, "card insert manually\n");
		set_bit(INGENIC_MMC_CARD_PRESENT, &sdhci_ing->flags);
		/* OpenKE (2026-07-22, FIRMWARE.md sec 46): mmc_add_host()'s own
		 * mmc_start_host() ALREADY did one automatic mmc_power_up()+rescan for
		 * every mmc host unconditionally (real, mainline drivers/mmc/core/
		 * core.c behavior, independent of non-removable/cd_type) - that's the
		 * "Failed to initialize" seen at probe time, before this function ever
		 * runs. Calling mmc_detect_change() alone below re-triggers a rescan
		 * against gpios that are already sitting at whatever level that first,
		 * automatic cycle left them at - no new edge happens. If the real chip
		 * needs an actual transition (not just a static level) at the moment
		 * of the real power-on, a bare re-rescan can never produce one. Force
		 * a real power-cycle - off, real settle time, on again via our own
		 * already-hardware-verified-correct pwrseq (wlan_pwrseq: gpd4 reset
		 * line + the wifi_bt_power regulator via vmmc-supply) - immediately
		 * before detection, so the actual reset/enable line gets a genuine
		 * fresh transition right here, not stale state from minutes/seconds
		 * earlier at a completely different point in boot.
		 *
		 * OpenKE (2026-07-22, FIRMWARE.md sec 49): a paired stock-vs-custom
		 * register trace proved this power-cycle races the automatic rescan
		 * the comment above already knows about. On custom, mmc1 registers
		 * at ~1.33s and mmc_start_host()'s own automatic rescan doesn't
		 * report "Failed to initialize a non-removable card" until ~2.26s -
		 * nearly a full second later. This function's own power-off/on
		 * cycle runs at ~1.77s, squarely *inside* that window, while the
		 * automatic rescan may still be issuing commands against the chip.
		 * On stock's real, working boot the equivalent manual insert is the
		 * *only* rescan ever run against mmc1 - there is no second,
		 * concurrent attempt to race against. cancel_delayed_work_sync()
		 * on the host's own detect work guarantees any in-flight automatic
		 * rescan has fully finished - not just had its next run cancelled -
		 * before we cut power, so our power-cycle can never land mid-command
		 * against a rescan that's still using the chip. */
		cancel_delayed_work_sync(&host->mmc->detect);
		/* OpenKE (2026-07-22, FIRMWARE.md sec 51): WL_REG_ON now driven
		 * directly, bypassing mmc_pwrseq_simple entirely - sec 50 proved
		 * mmc_power_up()'s pwrseq sequence completes (power_mode really
		 * transitions 0->2) without ever moving the physical pin, and
		 * PD04 is no longer in wlan_pwrseq's reset-gpios at all (see the
		 * devicetree), so mmc_power_off()/mmc_power_up() below can no
		 * longer touch this pin either way - they're kept only for their
		 * other real effects (voltage/clock setup via mmc_set_ios()).
		 * Sequence matches stock's own disassembled bcm_wlan_power_on()
		 * exactly: raw LOW, real 100ms hold, raw HIGH, detect immediately
		 * - no extra delay invented after the HIGH transition. */
		mmc_power_off(host->mmc);
		if (sdhci_ing->wlan_reg_on) {
			gpiod_set_raw_value_cansleep(sdhci_ing->wlan_reg_on, 0);
			pr_info("openke: WIFI_SEQ: WL_REG_ON requested_raw=0 descriptor_raw=%d\n",
				gpiod_get_raw_value_cansleep(sdhci_ing->wlan_reg_on));
		}
		pr_info("openke: WIFI_SEQ: sleeping 100 ms\n");
		msleep(100);
		mmc_power_up(host->mmc, host->mmc->ocr_avail);
		if (sdhci_ing->wlan_reg_on) {
			gpiod_set_raw_value_cansleep(sdhci_ing->wlan_reg_on, 1);
			pr_info("openke: WIFI_SEQ: WL_REG_ON requested_raw=1 descriptor_raw=%d\n",
				gpiod_get_raw_value_cansleep(sdhci_ing->wlan_reg_on));
		}
#ifdef CLK_CTRL
		ingenic_mmc_clk_onoff(sdhci_ing, 1);
#endif
		host->flags &= ~SDHCI_DEVICE_DEAD;
		host->quirks |= SDHCI_QUIRK_BROKEN_CARD_DETECTION;
		pr_info("openke: WIFI_SEQ: triggering manual detection\n");
		mmc_detect_change(sdhci_ing->host->mmc, 0);
		if (msc1_trace)
			openke_msc1_trace(host);
	} else {
		dev_err(&sdhci_ing->pdev->dev, "card remove manually\n");
		clear_bit(INGENIC_MMC_CARD_PRESENT, &sdhci_ing->flags);

		host->flags |= SDHCI_DEVICE_DEAD;
		host->quirks &= ~SDHCI_QUIRK_BROKEN_CARD_DETECTION;
		mmc_detect_change(sdhci_ing->host->mmc, 0);
#ifdef CLK_CTRL
		ingenic_mmc_clk_onoff(sdhci_ing, 0);
#endif
	}

	return 0;
}
EXPORT_SYMBOL(ingenic_mmc_manual_detect);

/**
 *  ingenic_mmc_clk_ctrl - enable or disable msc clock gate
 *  @index: host->index, namely the index of the controller.
 *  @on: 1-enable msc clock gate, 0-disable msc clock gate.
 *
 *  OpenKE (2026-07-22, FIRMWARE.md sec 52): reverted for the same reason as
 *  ingenic_mmc_manual_detect above - msc1 is back on this driver, and
 *  CONFIG_INGENIC_MMC is disabled again, so the sec 47 rename that dodged a
 *  collision with ingenic_mmc.c's own ingenic_mmc_clk_ctrl is no longer
 *  needed. */
int ingenic_mmc_clk_ctrl(int index, int on)
{
	struct sdhci_ingenic *sdhci_ing;
	struct list_head *pos;

#ifdef CLK_CTRL
	list_for_each(pos, &manual_list) {
		sdhci_ing = list_entry(pos, struct sdhci_ingenic, list);
		if (sdhci_ing->pdev->id == index) {
			break;
		} else {
			sdhci_ing = NULL;
		}
	}

	if (!sdhci_ing) {
		printk("no manual card detect\n");
		return -1;
	}
	ingenic_mmc_clk_onoff(sdhci_ing, on);
#endif
	return 0;
}
EXPORT_SYMBOL(ingenic_mmc_clk_ctrl);

static int sdhci_ingenic_parse_dt(struct device *dev,
                                  struct sdhci_host *host,
                                  struct sdhci_ingenic_pdata *pdata)
{
	struct device_node *np = dev->of_node;
	struct card_gpio *card_gpio;
	unsigned int val;

	card_gpio = devm_kzalloc(dev, sizeof(struct card_gpio), GFP_KERNEL);
	if (!card_gpio) {
		return 0;
	}
	pdata->gpio = card_gpio;

	pdata->sdr_v18 = devm_gpiod_get_optional(dev, "ingenic,sdr", GPIOD_OUT_HIGH);
	if (IS_ERR(pdata->sdr_v18)) {
		dev_err(dev, "get ingenic,sdr failed with error %ld\n",
		        PTR_ERR(pdata->sdr_v18));
	}

	pdata->gpio->pwr = devm_gpiod_get_optional(dev, "ingenic,pwr", GPIOD_OUT_LOW);
	if (IS_ERR(pdata->gpio->pwr)) {
		dev_err(dev, "get ingenic,pwr failed with error %ld\n",
		        PTR_ERR(pdata->gpio->pwr));
	}

	pdata->gpio->rst = devm_gpiod_get_optional(dev, "ingenic,rst", GPIOD_OUT_LOW);
	if (IS_ERR(pdata->gpio->rst)) {
		dev_err(dev, "get ingenic,rst failed with error %ld\n",
		        PTR_ERR(pdata->gpio->rst));
	}

	/* assuming internal card detect that will be configured by pinctrl */
	pdata->cd_type = SDHCI_INGENIC_CD_INTERNAL;

	if (of_property_read_bool(np, "pio-mode")) {
		pdata->pio_mode = 1;
	}
	if (of_property_read_bool(np, "enable_autocmd12")) {
		pdata->enable_autocmd12 = 1;
	}
	if (of_property_read_bool(np, "enable_cpm_rx_tuning")) {
		pdata->enable_cpm_rx_tuning = 1;
	}
	if (of_property_read_bool(np, "enable_cpm_tx_tuning")) {
		pdata->enable_cpm_tx_tuning = 1;
	}

	/* get the card detection method */
	if (of_get_property(np, "broken-cd", NULL)) {
		pdata->cd_type = SDHCI_INGENIC_CD_NONE;
	}

	if (of_get_property(np, "non-removable", NULL)) {
		pdata->cd_type = SDHCI_INGENIC_CD_PERMANENT;
	}

	if (of_get_property(np, "cd-inverted", NULL)) {
		pdata->cd_type = SDHCI_INGENIC_CD_GPIO;
	}

	if (!(of_property_read_u32(np, "ingenic,sdio_clk", &val))) {
		pdata->sdio_clk = val;
	}

	/* if(of_property_read_bool(np, "ingenic,removal-dontcare")) { */
	/*  pdata->removal = DONTCARE; */
	/* } else if(of_property_read_bool(np, "ingenic,removal-nonremovable")) { */
	/*  pdata->removal = NONREMOVABLE; */
	/* } else if(of_property_read_bool(np, "ingenic,removal-removable")) { */
	/*  pdata->removal = REMOVABLE; */
	/* } else if(of_property_read_bool(np, "ingenic,removal-manual")) { */
	/*  pdata->removal = MANUAL; */
	/* }; */

	/* mmc_of_parse_voltage(np, &pdata->ocr_avail); */

	return 0;
}
#else
static int sdhci_ingenic_parse_dt(struct device *dev,
                                  struct sdhci_host *host,
                                  struct sdhci_ingenic_pdata *pdata)
{
	return -EINVAL;
}
#endif

static int sdhci_ingenic_probe(struct platform_device *pdev)
{
	struct sdhci_ingenic_pdata *pdata;
	struct device *dev = &pdev->dev;
	struct sdhci_host *host;
	struct sdhci_ingenic *sdhci_ing;
	char clkname[16];
	int ret, irq;

	if (!pdev->dev.platform_data && !pdev->dev.of_node) {
		dev_err(dev, "no device data specified\n");
		return -ENOENT;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(dev, "no irq specified\n");
		return irq;
	}

	host = sdhci_alloc_host(dev, sizeof(struct sdhci_ingenic));
	if (IS_ERR(host)) {
		dev_err(dev, "sdhci_alloc_host() failed\n");
		return PTR_ERR(host);
	}
	sdhci_ing = sdhci_priv(host);

	pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		ret = -ENOMEM;
		return ret;
	}

	if (pdev->dev.of_node) {
		ret = sdhci_ingenic_parse_dt(dev, host, pdata);
		if (ret) {
			return ret;
		}
	} else {
		memcpy(pdata, pdev->dev.platform_data, sizeof(*pdata));
	}

	pdev->id = of_alias_get_id(pdev->dev.of_node, "mmc");

	sprintf(clkname, "div_msc%d", pdev->id);
	sdhci_ing->clk_cgu = devm_clk_get(&pdev->dev, clkname);
	if (!sdhci_ing->clk_cgu) {
		dev_err(&pdev->dev, "Failed to Get MSC clk!\n");
		return PTR_ERR(sdhci_ing->clk_cgu);
	}
	sprintf(clkname, "gate_msc%d", pdev->id);
	sdhci_ing->clk_gate = devm_clk_get(&pdev->dev, clkname);
	if (!sdhci_ing->clk_gate) {
		dev_err(&pdev->dev, "Failed to Get PWC MSC clk!\n");
		return PTR_ERR(sdhci_ing->clk_gate);
	}
	sdhci_ing->clk_ext = clk_get(NULL, "ext");
	sdhci_ing->clk_mpll = clk_get(NULL, "mpll");

	ingenic_mmc_clk_onoff(sdhci_ing, 1);

	sdhci_ing->host = host;
	sdhci_ing->dev  = &pdev->dev;
	sdhci_ing->pdev = pdev;
	sdhci_ing->pdata = pdata;

	/* OpenKE (2026-07-22, FIRMWARE.md sec 51): WL_REG_ON driven directly -
	 * see ingenic_mmc_manual_detect(). GPIOD_ASIS: this driver only ever
	 * uses gpiod_set_raw_value_cansleep() on this descriptor, never the
	 * active-flag-translating gpiod_set_value_cansleep(), so the initial
	 * direction/level here doesn't matter - the first real write in
	 * ingenic_mmc_manual_detect() sets it explicitly. Optional: absent on
	 * msc0/msc2, which don't have "wlan-reg-on-gpios". */
	sdhci_ing->wlan_reg_on = devm_gpiod_get_optional(dev, "wlan-reg-on", GPIOD_ASIS);
	if (IS_ERR(sdhci_ing->wlan_reg_on)) {
		return dev_err_probe(dev, PTR_ERR(sdhci_ing->wlan_reg_on),
			"failed to acquire WLAN_REG_ON GPIO\n");
	}
	if (sdhci_ing->wlan_reg_on) {
		ret = gpiod_direction_output_raw(sdhci_ing->wlan_reg_on, 0);
		if (ret) {
			return dev_err_probe(dev, ret,
				"failed to configure WLAN_REG_ON output\n");
		}
		pr_info("openke: WLAN_REG_ON GPIO acquired, direction_output_raw(0) ret=%d\n", ret);
	}

	host->ioaddr = of_iomap(pdev->dev.of_node, 0);
	if (IS_ERR(host->ioaddr)) {
		return PTR_ERR(host->ioaddr);
	}

	platform_set_drvdata(pdev, host);

	/* sdio for WIFI init*/
	if (pdata->sdio_clk) {
		ingenic_sdio_wlan_init(&pdev->dev, pdev->id);
		list_add(&(sdhci_ing->list), &manual_list);
	}

	host->hw_name = "ingenic-sdhci";
	host->ops = &sdhci_ingenic_ops;
	host->quirks = 0;
	host->irq = irq;

	/* Software redefinition caps */
	host->quirks |= SDHCI_QUIRK_MISSING_CAPS;
	host->caps  = CAPABILITIES1_SW;
	host->caps1 = CAPABILITIES2_SW;

	/* not check wp */
	host->quirks |= SDHCI_QUIRK_INVERTED_WRITE_PROTECT;

	/* Setup quirks for the controller */
	host->quirks |= SDHCI_QUIRK_NO_ENDATTR_IN_NOPDESC;
	host->quirks |= SDHCI_QUIRK_NO_HISPD_BIT;

	/* Data Timeout Counter Value */
	//host->quirks |= SDHCI_QUIRK_BROKEN_TIMEOUT_VAL;
	host->timeout_clk = 24000; //TMCLK = 24MHz

	/* This host supports the Auto CMD12 */
	if (pdata->enable_autocmd12) {
		host->quirks |= SDHCI_QUIRK_MULTIBLOCK_READ_ACMD12;
	}

	/* PIO transfer mode */
	if (pdata->pio_mode) {
		host->quirks |= SDHCI_QUIRK_BROKEN_DMA;
		host->quirks |= SDHCI_QUIRK_BROKEN_ADMA;
	}
	/* TODO:SoCs need BROKEN_ADMA_ZEROLEN_DESC */
	/*  host->quirks |= SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC;*/

	if (pdata->cd_type == SDHCI_INGENIC_CD_NONE ||
	    pdata->cd_type == SDHCI_INGENIC_CD_PERMANENT) {
		host->quirks |= SDHCI_QUIRK_BROKEN_CARD_DETECTION;
	}

	if (pdata->cd_type == SDHCI_INGENIC_CD_PERMANENT) {
		host->mmc->caps = MMC_CAP_NONREMOVABLE;
	}

	if (pdata->pm_caps) {
		host->mmc->pm_caps |= pdata->pm_caps;
	}

	host->quirks |= (SDHCI_QUIRK_32BIT_DMA_ADDR |
	                 SDHCI_QUIRK_32BIT_DMA_SIZE);

	/* It supports additional host capabilities if needed */
	if (pdata->host_caps) {
		host->mmc->caps |= pdata->host_caps;
	}

	if (pdata->host_caps2) {
		host->mmc->caps2 |= pdata->host_caps2;
	}

#ifdef CONFIG_PM_RUNTIME
	pm_runtime_enable(&pdev->dev);
	pm_runtime_set_autosuspend_delay(&pdev->dev, 50);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_suspend_ignore_children(&pdev->dev, 1);
#endif

	ret = mmc_of_parse(host->mmc);
	if (ret) {
		dev_err(dev, "mmc_of_parse() failed\n");
		pm_runtime_forbid(&pdev->dev);
		pm_runtime_get_noresume(&pdev->dev);
		return ret;
	}
	/* OpenKE (2026-07-22, FIRMWARE.md sec 50): temporary diagnostic - the
	 * live GPIO trace shows WL_REG_ON never leaves raw LOW no matter what
	 * reset-gpios says, which is exactly what happens if host->pwrseq
	 * never actually got attached here (mmc_pwrseq_alloc() silently
	 * returns 0 with pwrseq left NULL if of_parse_phandle() doesn't find
	 * "mmc-pwrseq" on this exact node - no error, no log line either way).
	 * Settle it directly instead of inferring from what's missing. */
	pr_info("openke: mmc_of_parse ok, host->mmc->parent=%s of_node=%pOF pwrseq=%p\n",
		dev_name(host->mmc->parent), host->mmc->parent->of_node, host->mmc->pwrseq);

	sdhci_enable_v4_mode(host);
	ret = sdhci_add_host(host);
	if (ret) {
		dev_err(dev, "sdhci_add_host() failed\n");
		pm_runtime_forbid(&pdev->dev);
		pm_runtime_get_noresume(&pdev->dev);
		return ret;
	}

	/*   enable card  inserted and unplug wake-up system */
	if (host->mmc->slot.cd_irq > 0) {
		enable_irq_wake(host->mmc->slot.cd_irq);
	}
#ifdef CONFIG_PM_RUNTIME
	if (pdata->cd_type != SDHCI_INGENIC_CD_INTERNAL) {
		clk_disable_unprepare(sdhci_ing->clk_cgu);
		/* clk_disable_unprepare(sdhci_ing->clk_gate); */
	}
#endif

	return 0;
}

static int sdhci_ingenic_remove(struct platform_device *pdev)
{
	struct sdhci_host *host =  platform_get_drvdata(pdev);
	struct sdhci_ingenic *sdhci_ing = sdhci_priv(host);

	if (sdhci_ing->ext_cd_irq) {
		free_irq(sdhci_ing->ext_cd_irq, sdhci_ing);
	}

#ifdef CONFIG_PM_RUNTIME
	if (pdata->cd_type != SDHCI_INGENIC_CD_INTERNAL) {
		/* clk_prepare_enable(sdhci_ing->clk_gate); */
		clk_prepare_enable(sdhci_ing->clk_cgu);
	}
#endif
	sdhci_remove_host(host, 1);

	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	clk_disable_unprepare(sdhci_ing->clk_cgu);
	/* clk_disable_unprepare(sdhci_ing->clk_gate); */

	sdhci_free_host(host);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

#ifdef CONFIG_PM
static int sdhci_ingenic_suspend(struct device *dev)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	struct sdhci_ingenic *sdhci_ing = sdhci_priv(host);
	int ret = 0;

	ret = sdhci_suspend_host(host);

	clk_disable_unprepare(sdhci_ing->clk_gate);
	return ret;
}

static int sdhci_ingenic_resume(struct device *dev)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	struct sdhci_ingenic *sdhci_ing = sdhci_priv(host) ;
	clk_prepare_enable(sdhci_ing->clk_gate);
	return sdhci_resume_host(host);
}
#endif

#ifdef CONFIG_PM
static int sdhci_ingenic_runtime_suspend(struct device *dev)
{
	return 0;
}

static int sdhci_ingenic_runtime_resume(struct device *dev)
{
	return 0;
}
#endif

#ifdef CONFIG_PM
static const struct dev_pm_ops sdhci_ingenic_pmops = {
	SET_SYSTEM_SLEEP_PM_OPS(sdhci_ingenic_suspend, sdhci_ingenic_resume)
	SET_RUNTIME_PM_OPS(sdhci_ingenic_runtime_suspend, sdhci_ingenic_runtime_resume,
	                   NULL)
};

#define SDHCI_INGENIC_PMOPS (&sdhci_ingenic_pmops)

#else
#define SDHCI_INGENIC_PMOPS NULL
#endif

static struct platform_device_id sdhci_ingenic_driver_ids[] = {
	{
		.name       = "ingenic,sdhci",
		.driver_data    = (kernel_ulong_t)NULL,
	},
	{ }
};
MODULE_DEVICE_TABLE(platform, sdhci_ingenic_driver_ids);

#ifdef CONFIG_OF
static const struct of_device_id sdhci_ingenic_dt_match[] = {
	{.compatible = "ingenic,sdhci",},
	{},
};
MODULE_DEVICE_TABLE(of, sdhci_ingenic_dt_match);
#endif

static struct platform_driver sdhci_ingenic_driver = {
	.probe      = sdhci_ingenic_probe,
	.remove     = sdhci_ingenic_remove,
	.id_table   = sdhci_ingenic_driver_ids,
	.driver     = {
		.owner  = THIS_MODULE,
		.name   = "ingenic,sdhci",
		.pm = SDHCI_INGENIC_PMOPS,
		.of_match_table = of_match_ptr(sdhci_ingenic_dt_match),
	},
};

module_platform_driver(sdhci_ingenic_driver);

MODULE_DESCRIPTION("Ingenic SDHCI (MSC) driver");
MODULE_AUTHOR("Large Dipper <wangquan.shao@ingenic.cn>");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("20160808");
