/*
 * Author: Paul Reioux aka Faux123 <reioux@gmail.com>
 *
 * Copyright 2012~2014 Paul Reioux
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
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/rq_stats.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/cpufreq.h>

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

//#define DEBUG_INTELLI_PLUG
#undef DEBUG_INTELLI_PLUG

#define INTELLI_PLUG_MAJOR_VERSION	2
#define INTELLI_PLUG_MINOR_VERSION	6

#define DEF_SAMPLING_MS			(1000)
#define BUSY_SAMPLING_MS		(500)

#define DUAL_CORE_PERSISTENCE		7
#define TRI_CORE_PERSISTENCE		5
#define QUAD_CORE_PERSISTENCE		3

#define BUSY_PERSISTENCE		10

#define CPU_DOWN_FACTOR			2

static DEFINE_MUTEX(intelli_plug_mutex);

static struct delayed_work intelli_plug_work;
static struct delayed_work intelli_plug_boost;

static struct workqueue_struct *intelliplug_wq;
static struct workqueue_struct *intelliplug_boost_wq;

static unsigned int intelli_plug_active = 0;
module_param(intelli_plug_active, uint, 0644);

static unsigned int eco_mode_active = 0;
module_param(eco_mode_active, uint, 0644);

static unsigned int touch_boost_active = 1;
module_param(touch_boost_active, uint, 0644);

//default to something sane rather than zero
static unsigned int sampling_time = DEF_SAMPLING_MS;

static unsigned int persist_count = 0;
static unsigned int busy_persist_count = 0;

static bool suspended = false;

struct ip_cpu_info {
	int cpu;
	unsigned int curr_max;
};

static DEFINE_PER_CPU(struct ip_cpu_info, ip_info);

static unsigned int screen_off_max = UINT_MAX;
module_param(screen_off_max, uint, 0644);

static int sleep_active = 0;

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

static unsigned int NwNs_Threshold[] = { 19, 30,  19,  11,  19,  11, 0,  11};
static unsigned int TwTs_Threshold[] = {140,  0, 140, 190, 140, 190, 0, 190};

static int mp_decision(void)
{
	static bool first_call = true;
	int new_state = 0;
	int nr_cpu_online;
	int index;
	unsigned int rq_depth;
	static cputime64_t total_time = 0;
	static cputime64_t last_time;
	cputime64_t current_time;
	cputime64_t this_time = 0;

	current_time = ktime_to_ms(ktime_get());
	if (first_call) {
		first_call = false;
	} else {
		this_time = current_time - last_time;
	}
	total_time += this_time;

	rq_depth = rq_info.rq_avg;
	//pr_info(" rq_deptch = %u", rq_depth);
	nr_cpu_online = num_online_cpus();

	if (nr_cpu_online) {
		index = (nr_cpu_online - 1) * 2;
		if ((nr_cpu_online < 4) &&
			(rq_depth >= NwNs_Threshold[index])) {
			if (total_time >= TwTs_Threshold[index]) {
				new_state = 1;
			}
		} else if (rq_depth <= NwNs_Threshold[index+1]) {
			if (total_time >= TwTs_Threshold[index+1] ) {
				new_state = 0;
			}
		} else {
			total_time = 0;
		}
	} else {
		total_time = 0;
	}

	last_time = ktime_to_ms(ktime_get());

	return new_state;
}

static unsigned int calculate_thread_stats(void)
{
	unsigned int avg_nr_run = avg_nr_running();
	unsigned int nr_run;
	unsigned int threshold_size;

	if (!eco_mode_active) {
		threshold_size =  ARRAY_SIZE(nr_run_thresholds_full);
		nr_run_hysteresis = 8;
		nr_fshift = 3;
#ifdef DEBUG_INTELLI_PLUG
		pr_info("intelliplug: full mode active!");
#endif
	}
	else {
		threshold_size =  ARRAY_SIZE(nr_run_thresholds_eco);
		nr_run_hysteresis = 4;
		nr_fshift = 1;
#ifdef DEBUG_INTELLI_PLUG
		pr_info("intelliplug: eco mode active!");
#endif
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

static void __cpuinit intelli_plug_boost_fn(struct work_struct *work)
{

	int nr_cpus = num_online_cpus();

	if (touch_boost_active)
		if (nr_cpus < 2)
			cpu_up(1);
}

static void __cpuinit intelli_plug_work_fn(struct work_struct *work)
{
	unsigned int nr_run_stat;
	unsigned int cpu_count = 0;
	unsigned int nr_cpus = 0;

	int decision = 0;
	int i;

	if (intelli_plug_active == 1) {
		nr_run_stat = calculate_thread_stats();
#ifdef DEBUG_INTELLI_PLUG
		pr_info("nr_run_stat: %u\n", nr_run_stat);
#endif
		cpu_count = nr_run_stat;
		// detect artificial loads or constant loads
		// using msm rqstats
		nr_cpus = num_online_cpus();
		if (!eco_mode_active && (nr_cpus >= 1 && nr_cpus < 4)) {
			decision = mp_decision();
			if (decision) {
				switch (nr_cpus) {
				case 2:
					cpu_count = 3;
#ifdef DEBUG_INTELLI_PLUG
					pr_info("nr_run(2) => %u\n", nr_run_stat);
#endif
					break;
				case 3:
					cpu_count = 4;
#ifdef DEBUG_INTELLI_PLUG
					pr_info("nr_run(3) => %u\n", nr_run_stat);
#endif
					break;
				}
			}
		}
		/* it's busy.. lets help it a bit */
		if (cpu_count > 2) {
			if (busy_persist_count == 0) {
				sampling_time = BUSY_SAMPLING_MS;
				busy_persist_count = BUSY_PERSISTENCE;
			}
		} else {
			if (busy_persist_count > 0)
				busy_persist_count--;
			else
				sampling_time = DEF_SAMPLING_MS;
		}

		if (!suspended) {
			switch (cpu_count) {
			case 1:
				if (persist_count > 0)
					persist_count--;
				if (persist_count == 0) {
					//take down everyone
					for (i = 3; i > 0; i--)
						cpu_down(i);
				}
#ifdef DEBUG_INTELLI_PLUG
				pr_info("case 1: %u\n", persist_count);
#endif
				break;
			case 2:
				persist_count = DUAL_CORE_PERSISTENCE;
				if (!decision)
					persist_count = DUAL_CORE_PERSISTENCE / CPU_DOWN_FACTOR;
				if (nr_cpus < 2) {
					for (i = 1; i < cpu_count; i++)
						cpu_up(i);
				} else {
					for (i = 3; i >  1; i--)
						cpu_down(i);
				}
#ifdef DEBUG_INTELLI_PLUG
				pr_info("case 2: %u\n", persist_count);
#endif
				break;
			default:
				pr_err("Run Stat Error: Bad value %u\n", nr_run_stat);
				break;
			}
		}
#ifdef DEBUG_INTELLI_PLUG
		else
			pr_info("intelli_plug is suspened!\n");
#endif
	}
	queue_delayed_work_on(0, intelliplug_wq, &intelli_plug_work,
		msecs_to_jiffies(sampling_time));
}

static void screen_off_limit(bool on)
{
	unsigned int i, ret;
	struct cpufreq_policy policy;
	struct ip_cpu_info *l_ip_info;

	/* not active, so exit */
	if (screen_off_max == UINT_MAX)
		return;

	for_each_online_cpu(i) {

		l_ip_info = &per_cpu(ip_info, i);
		ret = cpufreq_get_policy(&policy, i);
		if (ret)
			continue;

		if (on) {
			/* save current instance */
			l_ip_info->curr_max = policy.max;
			policy.max = screen_off_max;
		} else {
			/* restore */
			policy.max = l_ip_info->curr_max;
		}
		cpufreq_update_policy(i);
	}
}

static void intelli_plug_suspend(void)
{
	int i;
	int num_of_active_cores = num_possible_cpus();
	
	flush_workqueue(intelliplug_wq);

	mutex_lock(&intelli_plug_mutex);
	suspended = true;
	screen_off_limit(true);
	mutex_unlock(&intelli_plug_mutex);

	// put rest of the cores to sleep!
	for (i = num_of_active_cores - 1; i > 0; i--) {
		cpu_down(i);
	}
}

static void wakeup_boost(void)
{
	unsigned int i, ret;
	struct cpufreq_policy policy;

	for_each_online_cpu(i) {

		ret = cpufreq_get_policy(&policy, i);
		if (ret)
			continue;

		policy.cur = policy.max;
		cpufreq_update_policy(i);
	}
}

static void __cpuinit intelli_plug_resume(void)
{
	int num_of_active_cores;
	int i;

	mutex_lock(&intelli_plug_mutex);
	/* keep cores awake long enough for faster wake up */
	persist_count = BUSY_PERSISTENCE;
	suspended = false;
	mutex_unlock(&intelli_plug_mutex);

	/* wake up everyone */
	if (eco_mode_active)
		num_of_active_cores = 1;
	else
		num_of_active_cores = num_possible_cpus();

	for (i = 1; i < num_of_active_cores; i++) {
		cpu_up(i);
	}

	screen_off_limit(false);
	wakeup_boost();

	queue_delayed_work_on(0, intelliplug_wq, &intelli_plug_work,
		msecs_to_jiffies(10));
}

static ssize_t __cpuinit sleep_active_store(struct kobject *kobj,
                struct kobj_attribute *attr, const char *buf, size_t count)
{
	int input = 0;

	sscanf(buf, "%d", &input);

	if (input == 0) {
		if (sleep_active == 1) {
			intelli_plug_resume();
			sleep_active = 0;
		}
	} else {
		if (sleep_active == 0) {
			intelli_plug_suspend();
			sleep_active = 1;
		}
	}
	return 0;
}

static ssize_t sleep_active_show(struct kobject *kobj,
                                struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d", sleep_active);
}

static struct kobj_attribute sleep_active_attribute_driver = 
	__ATTR(sleep_active_status, 0666,
		sleep_active_show, sleep_active_store);

static struct attribute *sleep_active_attrs[] =
        {
                &sleep_active_attribute_driver.attr,
                NULL,
        };

static struct attribute_group sleep_active_attr_group =
        {
                .attrs = sleep_active_attrs,
        };

static struct kobject *sleep_active_kobj;

static void intelli_plug_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
#ifdef DEBUG_INTELLI_PLUG
	pr_info("intelli_plug touched!\n");
#endif
	queue_delayed_work_on(0, intelliplug_wq, &intelli_plug_boost,
		msecs_to_jiffies(10));
}

static int intelli_plug_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "intelliplug";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;
	pr_info("%s found and connected!\n", dev->name);
	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void intelli_plug_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id intelli_plug_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			 INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			    BIT_MASK(ABS_MT_POSITION_X) |
			    BIT_MASK(ABS_MT_POSITION_Y) },
	}, /* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			 INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			    BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	}, /* touchpad */
	{ },
};

static struct input_handler intelli_plug_input_handler = {
	.event          = intelli_plug_input_event,
	.connect        = intelli_plug_input_connect,
	.disconnect     = intelli_plug_input_disconnect,
	.name           = "intelliplug_handler",
	.id_table       = intelli_plug_ids,
};

int __init intelli_plug_init(void)
{
	int rc;

	/* We want all CPUs to do sampling nearly on same jiffy */
	int sysfs_result;

	sleep_active_kobj =
		kobject_create_and_add("intelliplug", kernel_kobj);

        if (!sleep_active_kobj) {
                pr_err("%s intelliplug create failed!\n",
                        __FUNCTION__);
                return -ENOMEM;
        }

        sysfs_result = sysfs_create_group(sleep_active_kobj,
                        &sleep_active_attr_group);

        if (sysfs_result) {
                pr_info("%s sysfs create failed!\n", __FUNCTION__);
                kobject_put(sleep_active_kobj);
        }

	//pr_info("intelli_plug: scheduler delay is: %d\n", delay);
	pr_info("intelli_plug: version %d.%d by faux123\n",
		 INTELLI_PLUG_MAJOR_VERSION,
		 INTELLI_PLUG_MINOR_VERSION);

	rc = input_register_handler(&intelli_plug_input_handler);

	intelliplug_wq = alloc_workqueue("intelliplug",
				WQ_HIGHPRI | WQ_UNBOUND, 1);
	intelliplug_boost_wq = alloc_workqueue("iplug_boost",
				WQ_HIGHPRI | WQ_UNBOUND, 1);
	INIT_DELAYED_WORK(&intelli_plug_work, intelli_plug_work_fn);
	INIT_DELAYED_WORK(&intelli_plug_boost, intelli_plug_boost_fn);
	queue_delayed_work_on(0, intelliplug_wq, &intelli_plug_work,
		msecs_to_jiffies(10));

	return 0;
}

MODULE_AUTHOR("Paul Reioux <reioux@gmail.com>");
MODULE_DESCRIPTION("'intell_plug' - An intelligent cpu hotplug driver for "
	"Low Latency Frequency Transition capable processors");
MODULE_LICENSE("GPL");

late_initcall(intelli_plug_init);
