/*
 * Copyright (C) ST-Ericsson SA 2011
 *
 * License Terms: GNU General Public License v2
 * Author: Mattias Wallin <mattias.wallin@stericsson.com> for ST-Ericsson
 */
#include <linux/io.h>
#include <linux/errno.h>
#include <linux/clksrc-dbx500-prcmu.h>
#include <linux/clksrc-db5500-mtimer.h>
#include <linux/bitops.h>

#include <asm/localtimer.h>

#include <plat/mtu.h>

#include <mach/setup.h>
#include <mach/hardware.h>
#include <mach/context.h>
#include <mach/db8500-regs.h>

/* PRCMU watchdog driver during boot */
static __iomem void *tcdm_base;
static __iomem void *prcmu_base;

#define PRCM_MBOX_CPU_VAL	0x0fc
#define PRCM_MBOX_CPU_SET	0x100

#define PRCM_MBOX_HEADER_REQ_MB4 (0xFE8 + 0x4)
#define PRCM_REQ_MB4 0xE48
#define PRCM_REQ_MB4_A9WDOG_0 (PRCM_REQ_MB4 + 0x0)
#define PRCM_REQ_MB4_A9WDOG_1 (PRCM_REQ_MB4 + 0x1)
#define PRCM_REQ_MB4_A9WDOG_2 (PRCM_REQ_MB4 + 0x2)
#define PRCM_REQ_MB4_A9WDOG_3 (PRCM_REQ_MB4 + 0x3)

#define MB4H_A9WDOG_CONF 0x16
#define MB4H_A9WDOG_EN   0x17
#define MB4H_A9WDOG_DIS  0x18
#define MB4H_A9WDOG_LOAD 0x19
#define MB4H_A9WDOG_KICK 0x20

#define A9WDOG_ID_MASK 0xf

#define BOOT_PRCMU_WD_TIMEOUT (30 * 1000) /* 30 sec */

void noinline mydelay_loop(unsigned long loops)
{
	asm volatile(
	"1:	subs %0, %0, #1 \n"
	"	bhi 1b		\n"
	: /* No output */
	: "r" (loops)
	);
}

static int wait_for_mb4(void)
{
	int i;

	for (i = 0; i < 20000; i++) {
		if (!(readl(prcmu_base + PRCM_MBOX_CPU_VAL) & BIT(4)))
			break;
		mydelay_loop(20000);
	}

	if (readl(prcmu_base + PRCM_MBOX_CPU_VAL) & BIT(4)) {
		return false;
	}

	return true;
}

static int __prcmu_a9wdog(u8 cmd, u8 d0, u8 d1, u8 d2, u8 d3)
{
	writeb(d0, (tcdm_base + PRCM_REQ_MB4_A9WDOG_0));
	writeb(d1, (tcdm_base + PRCM_REQ_MB4_A9WDOG_1));
	writeb(d2, (tcdm_base + PRCM_REQ_MB4_A9WDOG_2));
	writeb(d3, (tcdm_base + PRCM_REQ_MB4_A9WDOG_3));

	writeb(cmd, (tcdm_base + PRCM_MBOX_HEADER_REQ_MB4));

	writel(BIT(4), prcmu_base + PRCM_MBOX_CPU_SET);
	return wait_for_mb4();
}

static int boot_enable_a9wdog(u8 id)
{
	return __prcmu_a9wdog(MB4H_A9WDOG_EN, id, 0, 0, 0);
}

int boot_disable_a9wdog(u8 id)
{
	return __prcmu_a9wdog(MB4H_A9WDOG_DIS, id, 0, 0, 0);
}

/*
 * timeout is 28 bit, in ms.
 */
static int boot_load_a9wdog(u8 id, u32 timeout)
{
	return __prcmu_a9wdog(MB4H_A9WDOG_LOAD,
			    (id & A9WDOG_ID_MASK) |
			    /*
			     * Put the lowest 28 bits of timeout at
			     * offset 4. Four first bits are used for id.
			     */
			    (u8)((timeout << 4) & 0xf0),
			    (u8)((timeout >> 4) & 0xff),
			    (u8)((timeout >> 12) & 0xff),
			    (u8)((timeout >> 20) & 0xff));
}

#ifdef CONFIG_DBX500_CONTEXT
static int mtu_context_notifier_call(struct notifier_block *this,
				     unsigned long event, void *data)
{
	if (event == CONTEXT_APE_RESTORE)
		nmdk_clksrc_reset();
	return NOTIFY_OK;
}

static struct notifier_block mtu_context_notifier = {
	.notifier_call = mtu_context_notifier_call,
};
#endif

static void ux500_timer_reset(void)
{
	nmdk_clkevt_reset();
}

#include <asm/cacheflush.h>
void ux500_clean_l2_cache_all(void);

static void __init ux500_timer_init(void)
{
	void __iomem *prcmu_timer_base;
	int myret;

	if (cpu_is_u5500()) {
#ifdef CONFIG_LOCAL_TIMERS
		twd_base = __io_address(U5500_TWD_BASE);
#endif
		mtu_base = __io_address(U5500_MTU0_BASE);
		prcmu_timer_base = __io_address(U5500_PRCMU_TIMER_3_BASE);
	} else if (cpu_is_u8500() || cpu_is_u9540()) {
#ifdef CONFIG_LOCAL_TIMERS
		twd_base = __io_address(U8500_TWD_BASE);
#endif
		mtu_base = __io_address(U8500_MTU0_BASE);
		prcmu_timer_base = __io_address(U8500_PRCMU_TIMER_4_BASE);
	} else {
		ux500_unknown_soc();
	}

	/*
	 * Here we register the timerblocks active in the system.
	 * Localtimers (twd) is started when both cpu is up and running.
	 * MTU register a clocksource, clockevent and sched_clock.
	 * Since the MTU is located in the VAPE power domain
	 * it will be cleared in sleep which makes it unsuitable.
	 * We however need it as a timer tick (clockevent)
	 * during boot to calibrate delay until twd is started.
	 * RTC-RTT have problems as timer tick during boot since it is
	 * depending on delay which is not yet calibrated. RTC-RTT is in the
	 * always-on powerdomain and is used as clockevent instead of twd when
	 * sleeping.
	 *
	 * The PRCMU timer 4 (3 for DB5500) registers a clocksource and
	 * sched_clock with higher rating than the MTU since it is
	 * always-on.
	 *
	 * On DB5500, the MTIMER is the best clocksource since, unlike the
	 * PRCMU timer, it doesn't occasionally go backwards.
	 */
	tcdm_base = __io_address(U8500_PRCMU_TCDM_BASE);
	prcmu_base = __io_address(U8500_PRCMU_BASE);

	if (!boot_disable_a9wdog(1))
		goto wd_bypass;

	if (!boot_load_a9wdog(1, BOOT_PRCMU_WD_TIMEOUT))
		goto wd_bypass;

	myret = boot_enable_a9wdog(1);
	printk("%s: boot watchdog enabled %d\n", __func__, myret);
 
wd_bypass:
	pr_crit("before nmdk_timer_init\n");
	flush_cache_all();
	ux500_clean_l2_cache_all();
	nmdk_timer_init();
	pr_crit("after nmdk_timer_init\n");
	flush_cache_all();
	ux500_clean_l2_cache_all();
	if (cpu_is_u5500())
		db5500_mtimer_init(__io_address(U5500_MTIMER_BASE));
	pr_crit("before clksrc_dbx500_prcmu_init\n");
	flush_cache_all();
	ux500_clean_l2_cache_all();
	clksrc_dbx500_prcmu_init(prcmu_timer_base);
	pr_crit("after clksrc_dbx500_prcmu_init\n");
	flush_cache_all();
	ux500_clean_l2_cache_all();

#ifdef CONFIG_DBX500_CONTEXT
	WARN_ON(context_ape_notifier_register(&mtu_context_notifier));
#endif

}

struct sys_timer ux500_timer = {
	.init		= ux500_timer_init,
	.resume		= ux500_timer_reset,
};
