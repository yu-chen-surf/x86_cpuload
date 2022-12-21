/*
 * x86_cpuload.c
 *
 * Copyright (C) 2015 Intel Corp.
 *
 * gcc x86_cpuload.c -o x86_cpuload -lm -lpthread
 *  ./x86_cpuload -s 2 -c 1 -b 40 -t 100 or
 *  ./x86_cpuload --start 2 --count 1 --busy 40 --timeout 100
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <getopt.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <errno.h>

/* Percpu info structure */
struct cpu_info {
	unsigned long long sample_tsc;
	unsigned long long timeout_tsc;
	unsigned long long period_tsc;
	pthread_t thread;
};
struct cpu_info *cpu_info;

/* To simulate the load, sleep for (100-load)msecs during each sample. */
#define DEFAULT_SAMPLE_MS 10
#define DEFAULT_TIMEOUT_SEC 10
#define DEFAULT_CPU_BUSY 100
#define DEFAULT_PERIOD_SEC 5
#define CPU_LOAD_CONST 1
#define CPU_LOAD_SIN 2
#define DEFAULT_LOAD_MODE CPU_LOAD_CONST

int sample_ms = DEFAULT_SAMPLE_MS;	/* sample time in msec */
int time_out = DEFAULT_TIMEOUT_SEC;	/* total time for testing */
int busy_pct = DEFAULT_CPU_BUSY;	/* busy percentage, const mode*/
int load_mode = DEFAULT_LOAD_MODE;	/* y=ax y=sin(x)...*/
int period_sec = DEFAULT_PERIOD_SEC;	/* period time for sin(x) */
unsigned long thread_count = 1;		/* worker thread */
unsigned long start_cpu;		/* cpu idto start wuth */
unsigned long online_cpus;		/* total online cpus*/
char *progname;

#undef UNUSED
#define UNUSED(x) ((void)x)

void help(void);
unsigned long get_online_cpus(void);
unsigned long long (*get_idle_time)(unsigned long long elapse,
				    unsigned long cpu);

static void err_exit(char *msg, int reason)
{
	if (reason)
		perror(msg);
	else
		printf("%s\n", msg);

	exit(1);
}

static unsigned long long rdtsc(void)
{
	unsigned int low, high;

	asm volatile("rdtsc" : "=a" (low), "=d" (high));

	return low | ((unsigned long long)high) << 32;
}

static unsigned long long us_to_tsc(unsigned long long usec,
				    unsigned long cpu)
{
	return usec * cpu_info[cpu].sample_tsc / sample_ms / 1000;
}

/*
 * Functions to get the idle time in usec
 * to sleep during one sample period.
 */
#define PI		3.14

unsigned long long get_idle_time_const(unsigned long long elapse,
				       unsigned long cpu)
{
	UNUSED(elapse);
	UNUSED(cpu);
	/* y = ax */
	return (sample_ms * 1000 - (sample_ms * 10 * busy_pct));
}

unsigned long long get_idle_time_sin(unsigned long long elapse,
				     unsigned long cpu)
{
	/* y = 0.5sin(ax) + 0.5 */
	unsigned long long period_tsc;
	float pct;

	period_tsc = cpu_info[cpu].period_tsc;
	pct = .5 * sinf((2.0 * PI * elapse / period_tsc)) + .5;

	return (sample_ms * 1000 - (sample_ms * 1000 * pct));
}

/* To estimate the delta tsc for sample_ms and time_out */
static int cpu_info_init(unsigned long cpu)
{
	unsigned long long sample_tsc;

	sample_tsc = rdtsc();
	usleep(sample_ms * 1000);
	cpu_info[cpu].sample_tsc = rdtsc() - sample_tsc;
	cpu_info[cpu].timeout_tsc = us_to_tsc(time_out * 1000000, cpu);
	cpu_info[cpu].period_tsc = us_to_tsc(period_sec * 1000000, cpu);

	return 0;
}

static void cpus_init(void)
{
	online_cpus = get_online_cpus();
	if (!online_cpus)
		err_exit("Get cpu online number failed.", 0);

	cpu_info = malloc(sizeof(struct cpu_info) * online_cpus);
	if (!cpu_info)
		err_exit("Allocate cpu_info array failed.", 1);
	switch (load_mode) {
	case CPU_LOAD_CONST:
		get_idle_time = get_idle_time_const;
		break;
	case CPU_LOAD_SIN:
		get_idle_time = get_idle_time_sin;
		break;
	default:
		help();
		err_exit("Work load mode not supported.", 0);
	}
}

/**
 * sample_loop: - core function to run specific time during sample period
 * simulate the load by usleep idle_tsc/sample_tsc, lasts for 'timeout_tsc'
 */
static void sample_loop(unsigned long long cpu)
{
	unsigned long long begin, now, end, tmp;
	unsigned long long sleep_us;
	unsigned long long sample_tsc, timeout_tsc;

	sample_tsc = cpu_info[cpu].sample_tsc;
	timeout_tsc = cpu_info[cpu].timeout_tsc;

	begin = now = rdtsc();
	end = now + timeout_tsc;
	tmp = now + sample_tsc;
	while (now < end) {
		now = rdtsc();
		if (now > tmp) {
			/* sleep and loop in each sample period */
			sleep_us = get_idle_time(now - begin, cpu);
			if (sleep_us)
				usleep(sleep_us);

			tmp += sample_tsc;
		}
	}
}

void *cpu_workload(void *arg)
{
	unsigned long cpu = (unsigned long)arg;

	if (cpu_info_init(cpu))
		return 0;

	printf("Starting workload on cpu %ld, lasts for %d seconds...\n",
		cpu, time_out);
	sample_loop(cpu);

	return 0;
}

static void start_worker_threads(void)
{
	unsigned long i, end;
	int ret;
	cpu_set_t cpus;
	pthread_attr_t thread_attr;

	pthread_attr_init(&thread_attr);
	i = start_cpu;
	end = i + thread_count;
	for (; i < end; ++i) {
		CPU_ZERO(&cpus);
		CPU_SET(i, &cpus);
		ret = pthread_attr_setaffinity_np(&thread_attr,
						  sizeof(cpu_set_t), &cpus);
		if (ret)
			err_exit("pthread_attr_setaffinity_np failed.", 1);

		pthread_create(&cpu_info[i].thread, &thread_attr, cpu_workload,
			       (void *)(unsigned long)i);
	}
	pthread_attr_destroy(&thread_attr);
	/* Wait for the threads to be scheduled. */
	sleep(1);
	for (i = start_cpu; i < end; i++) {
		ret = pthread_join(cpu_info[i].thread, NULL);
		if (ret && (ret != ESRCH))
			err_exit("pthread_join failed.", 1);
	}
	printf("Done.\n");
}

unsigned long get_online_cpus(void)
{
	FILE *fp;
	unsigned long cpu_num = 0;
	int cpu_id, ret;
	char *proc_stat = "/proc/stat";

	fp = fopen(proc_stat, "r");
	if (!fp)
		err_exit("Open failed /proc/stat.", 1);

	ret = fscanf(fp, "cpu %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d\n");
	if (ret)
		err_exit("Failed to parse /proc/stat format.", 0);

	while (1) {
		ret = fscanf(fp, "cpu%u %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d\n",
			     &cpu_id);
		if (ret != 1)
			break;
		cpu_num++;
	}
	fclose(fp);

	return cpu_num;
}

void help(void)
{
	char *msg =
	"Usage: %s [OPTION [ARG]] ...\n"
	" -h, --help         show this help statement\n"
	" -t, --timeout N    timeout after N seconds\n"
	" -s, --start cpu    start from CPU of this id\n"
	" -c, --count N      spawn N worker threads on following N cpus\n"
	" -b, --busy N       N%% busy when mode is set to constant load\n"
	" -p, --sample N     N msec of one sample period\n"
	" -d, --period N     N sec for one trigonometric period\n"
	" -m, --mode N       workload type, default to constant load\n"
	"                    m=1: constant load, y=ax (default)\n"
	"                    m=2: wave load, y=0.5sin(ax)+0.5\n"
	"Example: %s --start 3 --count 4 --timeout 100 --busy 60\n";

	printf(msg, progname, progname, progname);
}

static const struct option long_options[] = {
	/* These options set a flag. */
	{"start", required_argument, NULL, 's'},
	{"timeout", required_argument, NULL, 't'},
	{"count", required_argument, NULL, 'c'},
	{"busy", required_argument, NULL, 'b'},
	{"mode", required_argument, NULL, 'm'},
	{"sample", required_argument, NULL, 'p'},
	{"period", required_argument, NULL, 'd'},
	{"help", no_argument, NULL, 'h'},
	{0, 0, 0, 0}
};

int cmdline(int argc, char **argv)
{
	int opt, option_index = 0;
	int match = 0;

	progname = argv[0];

	while ((opt = getopt_long(argc, argv, "c:b:t:s:p:d:m:h",
		long_options, &option_index)) != -1) {
		switch (opt) {
		case 's':
			start_cpu = atol(optarg);
			match++;
			break;
		case 'c':
			thread_count = atol(optarg);
			break;
		case 't':
			time_out = atoi(optarg);
			break;
		case 'b':
			busy_pct = atoi(optarg);
			break;
		case 'p':
			sample_ms = atoi(optarg);
			break;
		case 'd':
			period_sec = atoi(optarg);
			break;
		case 'm':
			load_mode = atoi(optarg);
			break;
		case 'h':
		default:
			goto err_out;
		}
	}

	if (match != 1) {
		printf("Invalid input: -s is a must.\n");
		goto err_out;
	}
	return 0;
 err_out:
	help();
	exit(1);
}

static int verify_input(void)
{
	return (start_cpu < online_cpus) &&
		(thread_count > 0 && thread_count <= online_cpus) &&
		(start_cpu + thread_count <= online_cpus);
}

int main(int argc, char *argv[])
{
	cmdline(argc, argv);
	cpus_init();
	if (!verify_input())
		err_exit("Cpu range invalid", 0);

	start_worker_threads();

	return 0;
}
