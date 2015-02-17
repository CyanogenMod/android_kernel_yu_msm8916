/*
 * Author: Paul Reioux aka Faux123 <reioux@gmail.com>
 *
 * Copyright 2012 Paul Reioux
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/earlysuspend.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/module.h>

#define INTELLI_PLUG_MAJOR_VERSION	1
#define INTELLI_PLUG_MINOR_VERSION	1

#define DEF_SAMPLING_RATE		(50000)
#define DEF_SAMPLING_MS			(50)

#define DUAL_CORE_PERSISTENCE		50
#define TRI_CORE_PERSISTENCE		40
#define QUAD_CORE_PERSISTENCE		30

static DEFINE_MUTEX(intelli_plug_mutex);

struct delayed_work intelli_plug_work;

static unsigned int intelli_plug_active = 1;
module_param(intelli_plug_active, uint, 0644);

static unsigned int eco_mode_active = 0;
module_param(eco_mode_active, uint, 0644);

static unsigned int persist_count = 0;
static bool suspended = false;

#define NR_FSHIFT	3
static unsigned int nr_fshift = NR_FSHIFT;
module_param(nr_fshift, uint, 0644);

static unsigned int nr_run_thresholds_full[] = {
/* 	1,  2,  3,  4 - on-line cpus target */
	5,  7,  9,  UINT_MAX /* avg run threads * 2 (e.g., 9 = 2.25 threads) */
	};

static unsigned int nr_run_thresholds_eco[] = {
/*      1,  2, - on-line cpus target */
        3,  UINT_MAX /* avg run threads * 2 (e.g., 9 = 2.25 threads) */
        };

static unsigned int nr_run_hysteresis = 4;  /* 0.5 thread */
module_param(nr_run_hysteresis, uint, 0644);

static unsigned int nr_run_last;

static unsigned int calculate_thread_stats(void)
{
	unsigned int avg_nr_run = avg_nr_running();
	unsigned int nr_run;
	unsigned int threshold_size;

	if (!eco_mode_active) {
		threshold_size =  ARRAY_SIZE(nr_run_thresholds_full);
		nr_run_hysteresis = 8;
		nr_fshift = 3;
		//pr_info("intelliplug: full mode active!");
	}
	else {
		threshold_size =  ARRAY_SIZE(nr_run_thresholds_eco);
		nr_run_hysteresis = 4;
		nr_fshift = 1;
		//pr_info("intelliplug: eco mode active!");
	}

	for (nr_run = 1; nr_run < threshold_size; nr_run++) {
		unsigned int nr_threshold;
		if (!eco_mode_active)
			nr_threshold = nr_run_thresholds_full[nr_run - 1];
		else
			nr_threshold = nr_run_thresholds_eco[nr_run - 1];

		if (nr_run_last <= nr_run)
			nr_threshold += nr_run_hysteresis;
		if (avg_nr_run <= (nr_threshold << (FSHIFT - nr_fshift)))
			break;
	}
	nr_run_last = nr_run;

	return nr_run;
}

static void __cpuinit intelli_plug_work_fn(struct work_struct *work)
{
	unsigned int nr_run_stat;

	if (intelli_plug_active == 1) {
		nr_run_stat = calculate_thread_stats();
		//pr_info("nr_run_stat: %u\n", nr_run_stat);

		if (!suspended) {
			switch (nr_run_stat) {
				case 1:
					if (persist_count > 0)
						persist_count--;
					if (num_online_cpus() == 2 && persist_count == 0)
						cpu_down(1);
					if (eco_mode_active) {
						cpu_down(3);
						cpu_down(2);
					}
					//pr_info("case 1: %u\n", persist_count);
					break;
				case 2:
					persist_count = DUAL_CORE_PERSISTENCE;
					if (num_online_cpus() == 1)
						cpu_up(1);
					else {
						cpu_down(3);
						cpu_down(2);
					}
					//pr_info("case 2: %u\n", persist_count);
					break;
				case 3:
					persist_count = TRI_CORE_PERSISTENCE;
					if (num_online_cpus() == 2)
						cpu_up(2);
					else
						cpu_down(3);
					//pr_info("case 3: %u\n", persist_count);
					break;
				case 4:
					persist_count = QUAD_CORE_PERSISTENCE;
					if (num_online_cpus() == 3)
						cpu_up(3);
					//pr_info("case 4: %u\n", persist_count);
					break;
				default:
					pr_err("Run Stat Error: Bad value %u\n", nr_run_stat);
					break;
			}
		} //else
			//pr_info("intelli_plug is suspened!\n");
	}
	schedule_delayed_work_on(0, &intelli_plug_work,
		msecs_to_jiffies(DEF_SAMPLING_MS));
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void intelli_plug_early_suspend(struct early_suspend *handler)
{
	int i;
	int num_of_active_cores = 4;
	
	cancel_delayed_work_sync(&intelli_plug_work);

	mutex_lock(&intelli_plug_mutex);
	suspended = true;
	mutex_unlock(&intelli_plug_mutex);

	// put rest of the cores to sleep!
	for (i=1; i<num_of_active_cores; i++) {
		if (cpu_online(i))
			cpu_down(i);
	}
}

static void __cpuinit intelli_plug_late_resume(struct early_suspend *handler)
{
	int num_of_active_cores;
	int i;

	mutex_lock(&intelli_plug_mutex);
	/* keep cores awake long enough for faster wake up */
	persist_count = DUAL_CORE_PERSISTENCE;
	suspended = false;
	mutex_unlock(&intelli_plug_mutex);

	/* wake up everyone */
	if (eco_mode_active)
		num_of_active_cores = 2;
	else
		num_of_active_cores = 4;

	for (i=1; i<num_of_active_cores; i++) {
		if (!cpu_online(i))
			cpu_up(i);
	}

	schedule_delayed_work_on(0, &intelli_plug_work,
		msecs_to_jiffies(10));
}

static struct early_suspend intelli_plug_early_suspend_struct_driver = {
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 10,
	.suspend = intelli_plug_early_suspend,
	.resume = intelli_plug_late_resume,
};
#endif	/* CONFIG_HAS_EARLYSUSPEND */

int __init intelli_plug_init(void)
{
	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(DEF_SAMPLING_RATE);

	if (num_online_cpus() > 1)
		delay -= jiffies % delay;

	//pr_info("intelli_plug: scheduler delay is: %d\n", delay);
	pr_info("intelli_plug: version %d.%d by faux123\n",
		 INTELLI_PLUG_MAJOR_VERSION,
		 INTELLI_PLUG_MINOR_VERSION);

	INIT_DELAYED_WORK(&intelli_plug_work, intelli_plug_work_fn);
	schedule_delayed_work_on(0, &intelli_plug_work, delay);

#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&intelli_plug_early_suspend_struct_driver);
#endif
	return 0;
}

MODULE_AUTHOR("Paul Reioux <reioux@gmail.com>");
MODULE_DESCRIPTION("'intell_plug' - An intelligent cpu hotplug driver for "
	"Low Latency Frequency Transition capable processors");
MODULE_LICENSE("GPL");

late_initcall(intelli_plug_init);
