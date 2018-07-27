/*****************************************************************************\
 *  acct_gather_energy_xcc.c - functions for reading acct_gather.conf
 *****************************************************************************
 *  Copyright (C) 2018
 *  Written by SchedMD - Felip Moll
 *  Based on IPMI plugin by Thomas Cadeau/Yoann Blein @ Bull
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
 *
 *  This file is patterned after jobcomp_linux.c, written by Morris Jette and
 *  Copyright (C) 2002 The Regents of the University of California.
\*****************************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include "src/common/uid.h"
#include "src/common/slurm_xlator.h"
#include "src/common/slurm_acct_gather_energy.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/fd.h"
#include "src/slurmd/common/proctrack.h"

#include "src/slurmd/slurmd/slurmd.h"
#include "acct_gather_energy_xcc_config.h"

#include <freeipmi/freeipmi.h>

/* These are defined here so when we link with something other than
 * the slurmctld we will have these symbols defined.  They will get
 * overwritten when linking with the slurmctld.
 */
#if defined (__APPLE__)
slurmd_conf_t *conf __attribute__((weak_import)) = NULL;
#else
slurmd_conf_t *conf = NULL;
#endif

#define _DEBUG 1
#define _DEBUG_ENERGY 1
#define IPMI_VERSION 2		/* Data structure version number */
#define MAX_LOG_ERRORS 5	/* Max sensor reading errors log messages */
#define XCC_MIN_RES 50         /* Minimum resolution for XCC readings, in ms */

const char plugin_name[] = "AcctGatherEnergy XCC plugin";
const char plugin_type[] = "acct_gather_energy/xcc";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;

/* Global vars */
ipmi_ctx_t ipmi_ctx = NULL;

/* LUN, NetFN, CMD, Data[n]*/
uint8_t cmd_rq[8] = { 0x00, 0x3A, 0x32, 4, 2, 0, 0, 0 };
unsigned int cmd_rq_len = 8;

/* Struct to store the raw single data command reading */
typedef struct xcc_raw_single_data {
	uint16_t fifo_inx;
	uint32_t j;
	uint16_t mj;
	uint32_t s;
	uint16_t ms;
} xcc_raw_single_data_t;

/* Status of the xcc sensor in this thread */
typedef struct sensor_status {
	struct timeval first_read_time; /* First read time in this thread */
	struct timeval prev_read_time;  /* Previous read time */
	struct timeval curr_read_time;  /* Current read time */
	uint32_t base_mj; /* Initial energy sensor value (in milijoules) */ 
	uint64_t curr_mj; /* Consumed milijoules in the last reading */
	uint64_t prev_mj; /* Consumed milijoules in the previous reading */
	uint32_t low_mj; /* The lowest watermark seen for consumed energy */
	uint32_t high_mj; /* The highest watermark seen for consumed energy */
	uint32_t low_elapsed_ms; /* Time elapsed on the lowest watermark */
	uint32_t high_elapsed_ms; /* Time elapsed on the highest watermark */
} sensor_status_t;

static sensor_status_t * xcc_sensor = NULL;

// FIXME: TO CLEAN ////////////////////////////////////////////////////////////
static int dataset_id = -1; /* id of the dataset for profile data */

static slurm_ipmi_conf_t slurm_ipmi_conf;
static uint64_t debug_flags = 0;

static bool flag_energy_accounting_shutdown = false;
static bool flag_thread_started = false;
static bool flag_init = false;

static pthread_mutex_t ipmi_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ipmi_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t launch_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t launch_cond = PTHREAD_COND_INITIALIZER;
pthread_t thread_ipmi_id_launcher = 0;
pthread_t thread_ipmi_id_run = 0;

static bool _is_thread_launcher(void)
{
	static bool set = false;
	static bool run = false;

	if (!set) {
		set = 1;
		run = run_in_daemon("slurmd");
	}

	return run;
}

static bool _run_in_daemon(void)
{
	static bool set = false;
	static bool run = false;

	if (!set) {
		set = 1;
		run = run_in_daemon("slurmd,slurmstepd");
	}

	return run;
}

static int _running_profile(void)
{
	static bool run = false;
	static uint32_t profile_opt = ACCT_GATHER_PROFILE_NOT_SET;

	if (profile_opt == ACCT_GATHER_PROFILE_NOT_SET) {
		acct_gather_profile_g_get(ACCT_GATHER_PROFILE_RUNNING,
					  &profile_opt);
		if (profile_opt & ACCT_GATHER_PROFILE_ENERGY)
			run = true;
	}

	return run;
}

/* FIXME: TO CHECK, TEST, FORMAT
 * _init_ipmi_config() initializes parameters for freeipmi library and then
 * opens a connection to the in-band device, thus setting up the ipmi_ctx
 * object. Slurm IPMI XCC plugin only supports in-band communications because
 * otherwise the network overhead associated by out-band communications does
 * not permit to mantain a 10mS ample rate, so losing accuracy.
 */
static int _init_ipmi_config (void)
{
	//FIXME: SEE _inband_init in ipmi_monitoring_ipmi_communication.c !!!
	int errnum;
	
	/* Initialization flags */
	unsigned int ipmi_flags = 0; //IPMI_FLAGS_DEBUG_DUMP
	
	if (!(ipmi_ctx = ipmi_ctx_create())) {
		error("ipmi_ctx_create: %s\n", strerror(errno));
		goto cleanup;
	}
	
	/* XCC OEM commands always require to use in-band communication */
	if (slurm_ipmi_conf.driver_type == IPMI_DEVICE_LAN_2_0 ||
	    slurm_ipmi_conf.driver_type == IPMI_DEVICE_LAN) {
		error ("%s: error: XCC Lenovo plugin only supports in-band"
		       "communication");
		goto cleanup;
	}
	
	if (getuid() != 0) {
		error ("%s: error : must be root to open ipmi devices\n", __FUNC__);
		goto cleanup;
	}
	
        //FIXME: parse_get_freeipmi_inband_flags (slurm_ipmi_conf.workaround_flags_inband, &workaround_flags);
	
	if (slurm_ipmi_conf.driver_type == IPMI_DEVICE_UNKNOWN)
	{
		if ((ret = ipmi_ctx_find_inband (ipmi_ctx,
						 NULL,
						 slurm_ipmi_conf.disable_auto_probe,
						 slurm_ipmi_conf.driver_address,
						 slurm_ipmi_conf.register_spacing,
						 slurm_ipmi_conf.driver_device,
						 slurm_ipmi_conf.workaround_flags,
						 slurm_ipmi_conf.ipmi_flags)) < 0)
		{
			error ("%s: error on ipmi_ctx_find_inband: "
			       "%s\n",
			       __FUNC__, ipmi_ctx_errormsg (ipmi_ctx));
			goto cleanup;
		}
		
		if (ret == 0)
		{
			error ("%s: error on ipmi_ctx_find_inband, "
			       "ipmi device not found.\n",
			       __FUNC__);
			goto cleanup;
		}
	}
	else
	{
		if ((ipmi_ctx_open_inband(ipmi_ctx,
					  slurm_ipmi_conf.driver_type,
					  slurm_ipmi_conf.disable_auto_probe,
					  slurm_ipmi_conf.driver_address,
					  slurm_ipmi_conf.register_spacing,
					  slurm_ipmi_conf.driver_device,
					  slurm_ipmi_conf.workaround_flags,
					  slurm_ipmi_conf.ipmi_flags) < 0))
		{
			error ("%s: error on ipmi_ctx_open_inband:"
			       "%s\n",
			       __FUNC__, ipmi_ctx_errormsg (ipmi_ctx));
			goto cleanup;
		}
	}
	
	
	
	if (slurm_ipmi_conf.target_channel_number_is_set
	    || slurm_ipmi_conf.target_slave_address_is_set)
	{
		if (ipmi_ctx_set_target (ipmi_ctx,
					 slurm_ipmi_conf.target_channel_number_is_set ? &slurm_ipmi_conf.target_channel_number : NULL,
					 slurm_ipmi_conf.target_slave_address_is_set ? &slurm_ipmi_conf.target_slave_address : NULL) < 0)
		{
			error ("%s: error on ipmi_ctx_set_target:"
			       "%s\n",
			       __FUNC__, ipmi_ctx_errormsg (ipmi_ctx));
			goto cleanup;
		}
	}
	return SLURM_SUCCESS;
cleanup:
	ipmi_ctx_close(ipmi_ctx);
	ipmi_ctx_destroy(ipmi_ctx);
	return SLURM_FAILURE;
}

/* FIXME: TO IMPLEMENT WITH THE CORRECT CALL TO IPMI
 * _read_ipmi_values() reads the XCC sensor doing an OEM call to ipmi.
 * It returns NULL if the reading was unable to complete.
 */
static struct xcc_raw_single_data_t * _read_ipmi_values(void)
{
	struct xcc_raw_single_data_t * xcc_reading =
		xmalloc(sizeof(xcc_reading_data_t));
	
	/* Here we issue the XCC raw command */
	xcc_reading->fifo_inx = 10;
	xcc_reading->j = 20;
	xcc_reading->mj = 30;
	xcc_reading->s = 123;
	xcc_reading->ms = 123;
       
	return xcc_reading;
}

/* FIXME: Convert this function to MACRO*/
static uint32_t _elapsed_last_interval_ms()
{
	return  (xcc_sensor->curr_read_time.tv_sec / 1000
		 + xcc_sensor->curr_read_time.tv_usec * 1000)
		-
		(xcc_sensor->prev_read_time.tv_sec / 1000
		 + xcc_sensor->prev_read_time.tv_usec * 1000);
}

/* FIXME: Convert this function to MACRO*/
static uint32_t _consumed_last_interval_mj()
{
	xcc_sensor->curr_mj - xcc_sensor->prev_mj;
}

/* 
 *_curr_watts() reads the xcc_sensor data and return the consumed watts since
 * the last reading.
 */
static uint32_t _curr_watts()
{
	return _consumed_last_interval_mj()/_elapsed_last_interval_ms() * 1000;
}

/*
 * _thread_update_node_energy() calls to _read_ipmi_values() to get the XCC
 * sensor values, and updates the sensor status struct about node consumption.
 */
static int _thread_update_node_energy(void)
{
	struct xcc_raw_single_data_t * xcc_raw;

	xcc_raw = _read_ipmi_values();
	
	if (!xcc_raw) {
		error("%s could not read XCC ipmi values");
		return SLURM_FAILURE;
	}

	xcc_sensor->prev_read_time.tv_sec = xcc_sensor->curr_read_time.tv_sec;
	xcc_sensor->prev_read_time.tv_usec = xcc_sensor->curr_read_time.tv_usec;
	xcc_sensor->curr_read_time.tv_sec = xcc_raw->s;
	xcc_sensor->curr_read_time.tv_usec = xcc_raw->ms * 1000;
	xcc_sensor->prev_mj = xcc_sensor->curr_mj;
	xcc_sensor->curr_mj = xcc_raw->j*1000 + xcc_raw->mj;

	/**** FIXME: Do we really need this here? ****/
	//Here we record the interval with highest/lowest consumption
	uint32_t c_mj = _consumed_last_interval_mj();
	uint32_t e_ms = _elapsed_lastinterval_ms();

	if (xcc_sensor->low_mj == 0 || xcc_sensor->low_mj > c_mj) {
		xcc_sensor->low_mj = c_mj;
		xcc_sensor->low_elapsed_ms = e_ms;
	}
	if (xcc_sensor->high_mj == 0 || xcc_sensor->high_mj < c_mj) {
		xcc_sensor->high_mj = c_mj;
		xcc_sensor->high_elapsed_ms = e_ms;
	}
	/***********************************/
	if (debug_flags & DEBUG_FLAG_ENERGY) {
		info("ipmi-thread: XCC current_watts: %u, "
		     "consumed energy last interval: %"PRIu64" miliJoules"
		     "elapsed time last interval: %"PRIu64" miliSeconds"
		     "first read time unix timestamp: %u.%u"
		     "first read energy counter val: %u"
		     _curr_watts(),
		     _consumed_last_interval_mj(),
		     _elapsed_last_interval_ms(),
		     xcc_sensor->first_read_time.tv_sec,
		     xcc_sensor->first_read_time.tv_usec*1000,
		     xcc_sensor->base_mj);
	}

	return SLURM_SUCCESS;
}

/*
 * _thread_init() initializes the ipmi interface depending on the conf params.
 * and opens a connection to the in-band device (call to _init_ipmi_config()).
 * Then it performs the first read of the IPMI XCC sensor.
 */
static int _thread_init(void)
{
	static bool first = true;
	static bool first_init = SLURM_FAILURE;
	struct xcc_raw_single_data_t * xcc_raw;
	uint16_t i;

	if (!first)
		return first_init;
	first = false;


	if (_init_ipmi_config() != SLURM_SUCCESS) {
		if (debug_flags & DEBUG_FLAG_ENERGY)
			info("%s thread init error on _init_ipmi_config()",
			     plugin_name);
		goto cleanup;
	}

	xcc_raw = _read_ipmi_values()
	if (!xcc_raw) {
		error("%s could not read XCC ipmi values");
		goto cleanup;
	}

#if _DEBUG
	if (xcc_sensor)
		fatal("There is already a xcc_sensor initialized, this"
		      "should never happen!");
#endif
	xcc_sensor = xmalloc(sizeof(sensor_status_t));
	memset(xcc_sensor, 0, sizeof(sensor_status_t));
	
	/* Let's fill the xcc_sensor with the first reading */
	xcc_sensor->first_read_time.tv_sec = xcc_raw->s;
	xcc_sensor->first_read_time.tv_usec = xcc_raw->ms * 1000;	
	xcc_sensor->prev_read_time.tv_sec = xcc_raw->s;
	xcc_sensor->prev_read_time.tv_usec = xcc_raw->ms * 1000;
	xcc_sensor->curr_read_time.tv_sec = xcc_raw->s;
	xcc_sensor->curr_read_time.tv_usec = xcc_raw->ms * 1000;
	xcc_sensor->base_mj = xcc_raw->j*1000 + xcc_raw->mj;
	xcc_sensor->curr_mj = xcc_sensor->base_mj;
	xcc_sensor->prev_mj = xcc_sensor->base_mj;
	
	if (debug_flags & DEBUG_FLAG_ENERGY)
		info("%s thread init success", plugin_name);
	
	first_init = SLURM_SUCCESS;
	xfree(xcc_raw);	

	return SLURM_SUCCESS;
cleanup:
	info("%s thread init error", plugin_name);
	xfree(xcc_raw);
	first_init = SLURM_FAILURE;
	ipmi_ctx_close(ipmi_ctx);
	ipmi_ctx_destroy(ipmi_ctx);
	return SLURM_FAILURE;
}

/*FIXME: To check everything*/
static int _ipmi_send_profile(void)
{
	int i;
	uint64_t data[4]; //Energy, [Max|Min|Avg]Power
	time_t last_time = last_update_time;

	if (!_running_profile())
		return SLURM_SUCCESS;

	if (dataset_id < 0) {
		acct_gather_profile_dataset_t dataset[5];
		dataset[0].name = xstrdup("Energy");
		dataset[1].name = xstrdup("MaxPower");
		dataset[2].name = xstrdup("MinPower");
		dataset[3].name = xstrdup("AvgPower");

		dataset[0].type = PROFILE_FIELD_UINT64;
		dataset[1].type = PROFILE_FIELD_UINT64;
		dataset[2].type = PROFILE_FIELD_UINT64;
		dataset[3].type = PROFILE_FIELD_UINT64;

		dataset[4].name = NULL;
		dataset[4].type = PROFILE_FIELD_NOT_SET;

		dataset_id = acct_gather_profile_g_create_dataset(
			"Energy", NO_PARENT, dataset);

		/* Once the dataset is created, free do cleanup*/
		for (i = 0; i < 5; ++i)
			xfree(dataset[i].name);
		
		if (debug_flags & DEBUG_FLAG_ENERGY)
			debug("Energy: dataset created (id = %d)", dataset_id);
		
		if (dataset_id == SLURM_ERROR) {
			error("Energy: Failed to create the dataset for IPMI");
			return SLURM_ERROR;
		}
	}

	/* pack an array of uint64_t with current sensors */
	memset(data, 0, sizeof(data));
	data[0] += xcc_sensor.energy.current_watts;
	data[1] += xcc_sensor.energy.current_watts;//FIXME
	data[2] += xcc_sensor.energy.current_watts;//FIXME
	data[3] += xcc_sensor.energy.current_watts;//FIXME
	last_time = xcc_sensor.energy.poll_time;
	
	if (debug_flags & DEBUG_FLAG_PROFILE) {
		info("PROFILE-Energy: ConsumedEnergy=%"PRIu64"", data[0]);
		info("PROFILE-Energy: MaxPower=%"PRIu64"", data[0]);
		info("PROFILE-Energy: MinPower=%"PRIu64"", data[0]);
		info("PROFILE-Energy: AvgPower=%"PRIu64"", data[0]);
	}
	
	return acct_gather_profile_g_add_sample_data(dataset_id, (void *)data,
						     last_time);
}


/*
 * _thread_ipmi_run() stays in a loop until shutdown, just updating the node
 * energy reading with a call to _thread_update_node_energy() and then waiting
 * for EnergyIPMIFrequency seconds.
 */
static void *_thread_ipmi_run(void *no_data)
{
	struct timeval now;
	struct timespec later;

	flag_energy_accounting_shutdown = false;
	
	if (debug_flags & DEBUG_FLAG_ENERGY)
		info("ipmi-thread: launched");

	(void) pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	(void) pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	slurm_mutex_lock(&ipmi_mutex);
	if (_thread_init() != SLURM_SUCCESS) {
		if (debug_flags & DEBUG_FLAG_ENERGY)
			info("ipmi-thread: aborted");
		slurm_mutex_unlock(&ipmi_mutex);
		slurm_cond_signal(&launch_cond);
		return NULL;
	}

	(void) pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
	slurm_mutex_unlock(&ipmi_mutex);
	
	flag_thread_started = true;

	/* Notify the parent that we are all set */
	slurm_cond_signal(&launch_cond);

	gettimeofday(&now, NULL);
	later.tv_sec = now.tv_sec;
	later.tv_nsec = now.tv_usec * 1000;

	/* This is the loop that gathers the ipmi data frequently */
	while (!flag_energy_accounting_shutdown) {		
		slurm_mutex_lock(&ipmi_mutex);
		_thread_update_node_energy();
		later.tv_sec += slurm_ipmi_conf.freq;		
		slurm_cond_timedwait(&ipmi_cond, &ipmi_mutex, &later);		
		slurm_mutex_unlock(&ipmi_mutex);
	}

	if (debug_flags & DEBUG_FLAG_ENERGY)
		info("ipmi-thread: ended");

	return NULL;
}

static void *_thread_launcher(void *no_data)
{
	struct timeval now;
	struct timespec timeout;

	slurm_thread_create(&thread_ipmi_id_run, _thread_ipmi_run, NULL);

	/* Wait for thread launch success for a max. of EnergyIPMITimeout */
	gettimeofday(&now, NULL);
	timeout.tv_sec = now.tv_sec + slurm_ipmi_conf.timeout;
        timeout.tv_nsec = now.tv_usec * 1000;

	slurm_mutex_lock(&launch_mutex);
	slurm_cond_timedwait(&launch_cond, &launch_mutex, &timeout);
	slurm_mutex_unlock(&launch_mutex);

	if (!flag_thread_started) {
		error("%s thread start timed out", plugin_name);

		flag_energy_accounting_shutdown = true;

		/* Just in case IPMI call is hang up. */
		pthread_cancel(thread_ipmi_id_run);

		/*
		 * Unlock just to make sure since we could have canceled the
		 * thread while in the lock.
		 */
		slurm_mutex_unlock(&ipmi_mutex);
	}

	return NULL;
}

/*
 *  _get_joules_task() issues an RPC call to slurmd in order to get the
 * node consumption. It will store this consumption in this task, thus the
 * plugin is not calling IPMI for each task. The difference from task to node
 * is that the base joules are taken from the start of the task instead of
 * from the slurmd register.
 */
static int _get_joules_task(uint16_t delta)
{
	time_t now = time(NULL);
	static bool first = true;
	acct_gather_energy_t *energy = NULL;
	uint16_t sensor_cnt = 0;       
	
	if (slurm_get_node_energy(NULL, delta, &sensor_cnt, &energy)) {
		error("_get_joules_task: can't get info from slurmd");
		return SLURM_ERROR;
	}

	if (sensor_cnt != 1) {
		error("_get_joules_task: received %u xcc sensors,"
		      "%u expected 1", sensor_cnt);
		acct_gather_energy_destroy(energy);
		return SLURM_ERROR;
	}

	if (first) {
		xcc_sensor = xmalloc(sizeof(sensor_status_t));
		memset(xcc_sensor, 0, sizeof(sensor_status_t));
	}

	xcc_sensor->prev_mj = xcc_sensor->curr_mj;


	if (!first) {
		xcc_sensor->prev_mj = curr_mj;
		xcc_sensor->prev_read_time = xcc_sensor->curr_read_time;
 		xcc_sensor->curr_read_time = energy->poll_time;		
		xcc_sensor->curr_mj = energy->consumed_energy;
		/**** FIXME: Do we really need this here? ****/
		//Here we record the interval with highest/lowest consumption
		uint32_t c_mj = _consumed_last_interval_mj();
		uint32_t e_ms = _elapsed_lastinterval_ms();
		
		if (xcc_sensor->low_mj == 0 || xcc_sensor->low_mj > c_mj) {
			xcc_sensor->low_mj = c_mj;
			xcc_sensor->low_elapsed_ms = e_ms;
		}
		if (xcc_sensor->high_mj == 0 || xcc_sensor->high_mj < c_mj) {
			xcc_sensor->high_mj = c_mj;
			xcc_sensor->high_elapsed_ms = e_ms;
		}
		/***********************************/
	       
	} else {
		/* This is just for the step, so take all the pervious
		   consumption out of the mix. */
		xcc_sensor->first_read_time.tv_sec = energy->poll_time;
		xcc_sensor->prev_read_time.tv_sec = energy->poll_time;
		xcc_sensor->curr_time.tv_sec = energy->poll_time;
		xcc_sensor->base_mj = energy->consumed_energy;
		xcc_sensor->curr_mj = xcc_sensor->base_mj;
		xcc_sensor->prev_mj = xcc_sensor->base_mj;
		xcc_sensor->low_mj = xcc_sensor->base_mj;
		xcc_sensor->high_mj = xcc_sensor->base_mj;
		xcc_sensor->low_elapsed_ms = 0;
		xcc_sensor->high_elapsed_ms = 0;
	}
	
	if (debug_flags & DEBUG_FLAG_ENERGY) {
		info("%s: XCC current_watts: %u, "
		     "consumed energy last interval: %"PRIu64" miliJoules"
		     "elapsed time last interval: %"PRIu64" miliSeconds"
		     "first read time unix timestamp: %u.%u"
		     "first read energy counter val: %u"
		     __FUNC__,
		     _curr_watts(),
		     _consumed_last_interval_mj(),
		     _elapsed_last_interval_ms(),
		     xcc_sensor->first_read_time.tv_sec,
		     xcc_sensor->first_read_time.tv_usec*1000,
		     xcc_sensor->base_mj);
	}

	acct_gather_energy_destroy(energy);
	first = false;
	return SLURM_SUCCESS;
}

/* _xcc_to_energy() translates the xcc_sensor data to an energy struct. */
static void _xcc_to_energy(acct_gather_energy_t *energy)
{
	if (!xcc_sensor || !energy)
		return;

	memset(energy, 0, sizeof(acct_gather_energy_t));

	energy->base_watts = (xcc_sensor->low_mj /
			      xcc_sensor->low_elapsed_ms) * 1000;
	energy->consumed_energy = (xcc_sensor->curr_mj -
				   xcc_sensor->base_mj) * 1000;
	energy->base_consumed_energy = xcc_sensor->low_mj * 1000;
	energy->poll_time = xcc_sensor->curr_read_time.sec;
	energy->current_watts = _curr_watts();
	energy->previous_consumed_energy = xcc_sensor->prev_mj * 1000;       
}

/*
 * FIXME: need to understand what field really means.
 */
static void _get_node_energy(acct_gather_energy_t *energy)
{
	_xcc_to_energy(energy);
}

/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
extern int init(void)
{
	debug_flags = slurm_get_debug_flags();
	/* put anything that requires the .conf being read in
	   acct_gather_energy_p_conf_set
	*/

	return SLURM_SUCCESS;
}

extern int fini(void)
{
	uint16_t i;

	if (!_run_in_daemon())
		return SLURM_SUCCESS;

	flag_energy_accounting_shutdown = true;

	/* clean up the launch thread */
	slurm_cond_signal(&launch_cond);

	if (thread_ipmi_id_launcher)
		pthread_join(thread_ipmi_id_launcher, NULL);

	/* clean up the run thread */
	slurm_cond_signal(&ipmi_cond);

	slurm_mutex_lock(&ipmi_mutex);

	if (ipmi_ctx)
		ipmi_monitoring_ctx_destroy(ipmi_ctx);
	reset_slurm_ipmi_conf(&slurm_ipmi_conf);

	slurm_mutex_unlock(&ipmi_mutex);

	if (thread_ipmi_id_run)
		pthread_join(thread_ipmi_id_run, NULL);

	xfree(xcc_sensor);

	return SLURM_SUCCESS;
}

extern int acct_gather_energy_p_update_node_energy(void)
{
	int rc = SLURM_SUCCESS;
	xassert(_run_in_daemon());

	return rc;
}

/*FIXME: Adapt to the new energy sensor!!*/
extern int acct_gather_energy_p_get_data(enum acct_energy_type data_type,
					 void *data)
{
	uint16_t i;
	int rc = SLURM_SUCCESS;
	acct_gather_energy_t *energy = (acct_gather_energy_t *)data;
	time_t *last_poll = (time_t *)data;
	uint16_t *sensor_cnt = (uint16_t *)data;

	xassert(_run_in_daemon());

	switch (data_type) {
	case ENERGY_DATA_NODE_ENERGY_UP:
		slurm_mutex_lock(&ipmi_mutex);
		if (_is_thread_launcher()) {
			if (_thread_init() == SLURM_SUCCESS)
				_thread_update_node_energy();
		} else {
			_get_joules_task(10);
		}
		_get_node_energy(energy);
		slurm_mutex_unlock(&ipmi_mutex);
		break;
	case ENERGY_DATA_NODE_ENERGY:
		slurm_mutex_lock(&ipmi_mutex);
		_get_node_energy(energy);
		slurm_mutex_unlock(&ipmi_mutex);
		break;
	case ENERGY_DATA_LAST_POLL:
		slurm_mutex_lock(&ipmi_mutex);
		if (xcc_sensor)
			*last_poll = xcc_sensor->curr_read_time;
		else
			*last_poll = 0;
		slurm_mutex_unlock(&ipmi_mutex);
		break;
	case ENERGY_DATA_SENSOR_CNT:
		*sensor_cnt = 1;
		break;
	case ENERGY_DATA_STRUCT:
		slurm_mutex_lock(&ipmi_mutex);
		if (!energy)
			energy = xmalloc(sizeof(acct_gather_energy_t));
		_xcc_to_energy(energy)
		slurm_mutex_unlock(&ipmi_mutex);
		break;
	case ENERGY_DATA_JOULES_TASK:
		slurm_mutex_lock(&ipmi_mutex);
		if (_is_thread_launcher()) {
			if (_thread_init() == SLURM_SUCCESS)
				_thread_update_node_energy();
		} else {
			_get_joules_task(10);
		}
		if (!energy)
			energy = xmalloc(sizeof(acct_gather_energy_t));
		_xcc_to_energy(energy);
		slurm_mutex_unlock(&ipmi_mutex);
		break;
	default:
		error("acct_gather_energy_p_get_data: unknown enum %d",
		      data_type);
		rc = SLURM_ERROR;
		break;
	}
	return rc;
}

extern int acct_gather_energy_p_set_data(enum acct_energy_type data_type,
					 void *data)
{
	int rc = SLURM_SUCCESS;
	int *delta = (int *)data;

	xassert(_run_in_daemon());

	switch (data_type) {
	case ENERGY_DATA_RECONFIG:
		debug_flags = slurm_get_debug_flags();
		break;
	case ENERGY_DATA_PROFILE:
		slurm_mutex_lock(&ipmi_mutex);
		_get_joules_task(*delta);
		_ipmi_send_profile();
		slurm_mutex_unlock(&ipmi_mutex);
		break;
	default:
		error("acct_gather_energy_p_set_data: unknown enum %d",
		      data_type);
		rc = SLURM_ERROR;
		break;
	}
	return rc;
}

extern void acct_gather_energy_p_conf_options(s_p_options_t **full_options,
					      int *full_options_cnt)
{
	s_p_options_t options[] = {
		{"EnergyIPMICalcAdjustment", S_P_BOOLEAN},
		{"EnergyIPMIAuthenticationType", S_P_UINT32},
		{"EnergyIPMICipherSuiteId", S_P_UINT32},
		{"EnergyIPMIDriverDevice", S_P_STRING},
		{"EnergyIPMIDriverType", S_P_UINT32},
		{"EnergyIPMIDisableAutoProbe", S_P_UINT32},
		{"EnergyIPMIDriverAddress", S_P_UINT32},
		{"EnergyIPMIFrequency", S_P_UINT32},
		{"EnergyIPMIk_g", S_P_STRING},
		{"EnergyIPMIk_g_len", S_P_UINT32},
		{"EnergyIPMIPassword", S_P_STRING},
		{"EnergyIPMIPrivilegeLevel", S_P_UINT32},		
		{"EnergyIPMIProtocolVersion", S_P_UINT32},
		{"EnergyIPMIRegisterSpacing", S_P_UINT32},
		{"EnergyIPMIRetransmissionTimeout", S_P_UINT32},
		{"EnergyIPMISessionTimeout", S_P_UINT32},
		{"EnergyIPMITimeout", S_P_UINT32},
		{"EnergyIPMIUsername", S_P_STRING},
		{"EnergyIPMIWorkaroundFlags", S_P_UINT32},
		{"EnergyIPMIFlags", S_P_UINT32},
		{NULL} };

	transfer_s_p_options(full_options, options, full_options_cnt);
}

extern void acct_gather_energy_p_conf_set(s_p_hashtbl_t *tbl)
{
	char *tmp_char;

	/* Set initial values */
	reset_slurm_ipmi_conf(&slurm_ipmi_conf);

	if (tbl) {
		/* ipmi initialization parameters */
		s_p_get_boolean(&(slurm_ipmi_conf.adjustment),
				"EnergyIPMICalcAdjustment", tbl));
		s_p_get_uint32(&slurm_ipmi_conf.authentication_type,
			       "EnergyIPMIAuthenticationType", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.cipher_suite_id,
			       "EnergyIPMICipherSuiteId", tbl);
		s_p_get_string(&slurm_ipmi_conf.driver_device,
			       "EnergyIPMIDriverDevice", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.driver_type,
			       "EnergyIPMIDriverType", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.disable_auto_probe,
			       "EnergyIPMIDisableAutoProbe", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.driver_address,
			       "EnergyIPMIDriverAddress", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.freq,
			       "EnergyIPMIFrequency", tbl);
		if ((int)slurm_ipmi_conf.freq <= 0)
			fatal("EnergyIPMIFrequency must be a positive integer "
			      "in acct_gather.conf.");
		s_p_get_string(&slurm_ipmi_conf.k_g, "EnergyIPMIk_g", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.freq,
			       "EnergyIPMIk_g_len", tbl);
		s_p_get_string(&slurm_ipmi_conf.password,
			       "EnergyIPMIPassword", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.privilege_level,
			       "EnergyIPMIPrivilegeLevel", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.protocol_version,
			       "EnergyIPMIProtocolVersion", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.register_spacing,
			       "EnergyIPMIRegisterSpacing", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.retransmission_timeout,
			       "EnergyIPMIRetransmissionTimeout", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.session_timeout,
			       "EnergyIPMISessionTimeout", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.timeout,
			       "EnergyIPMITimeout", tbl);
		s_p_get_string(&slurm_ipmi_conf.username,
			       "EnergyIPMIUsername", tbl));
		s_p_get_uint32(&slurm_ipmi_conf.workaround_flags,
			       "EnergyIPMIWorkaroundFlags", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.flags,
			       "EnergyIPMIFlags", tbl);
}

	if (!_run_in_daemon())
		return;

	if (!flag_init) {
		flag_init = true;
		if (_is_thread_launcher()) {
			slurm_thread_create(&thread_ipmi_id_launcher,
					    _thread_launcher, NULL);
			if (debug_flags & DEBUG_FLAG_ENERGY)
				info("%s thread launched", plugin_name);
		} else
			_get_joules_task(0);
	}

	verbose("%s loaded", plugin_name);
}

extern void acct_gather_energy_p_conf_values(List *data)
{
	config_key_pair_t *key_pair;

	xassert(*data);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMICalcAdjustment");
	key_pair->value = xstrdup(slurm_ipmi_conf.adjustment
				  ? "Yes" : "No");
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMIAuthenticationType");
	key_pair->value = xstrdup_printf("%u",
					 slurm_ipmi_conf.authentication_type);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMICipherSuiteId");
	key_pair->value = xstrdup_printf("%u", slurm_ipmi_conf.cipher_suite_id);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMIDriverDevice");
	key_pair->value = xstrdup(slurm_ipmi_conf.driver_device);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMIDriverType");
	key_pair->value = xstrdup_printf("%u", slurm_ipmi_conf.driver_type);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMIDisableAutoProbe");
	key_pair->value = xstrdup_printf("%u",
					 slurm_ipmi_conf.disable_auto_probe);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMIDriverAddress");
	key_pair->value = xstrdup_printf("%u", slurm_ipmi_conf.driver_address);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMIFrequency");
	key_pair->value = xstrdup_printf("%u", slurm_ipmi_conf.freq);
	list_append(*data, key_pair);

	/*
	 * Don't give out the key
	 * key_pair = xmalloc(sizeof(config_key_pair_t));
	 * key_pair->name = xstrdup("EnergyIPMIk_g");
	 * key_pair->value = xstrdup(slurm_ipmi_conf.k_g);
	 * list_append(*data, key_pair);
	 */

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMIk_g_len");
	key_pair->value = xstrdup_printf("%u", slurm_ipmi_conf.k_g_len);
	list_append(*data, key_pair);

	/*
	 * Don't give out the password
	 * key_pair = xmalloc(sizeof(config_key_pair_t));
	 * key_pair->name = xstrdup("EnergyIPMIPassword");
         * key_pair->value = xstrdup(slurm_ipmi_conf.password);
	 * list_append(*data, key_pair);
	 */

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMIPrivilegeLevel");
	key_pair->value = xstrdup_printf("%u", slurm_ipmi_conf.privilege_level);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMIProtocolVersion");
	key_pair->value = xstrdup_printf("%u",
					 slurm_ipmi_conf.protocol_version);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMIRegisterSpacing");
	key_pair->value = xstrdup_printf("%u",
					 slurm_ipmi_conf.register_spacing);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMIRetransmissionTimeout");
	key_pair->value = xstrdup_printf(
		"%u", slurm_ipmi_conf.retransmission_timeout);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMISessionTimeout");
	key_pair->value = xstrdup_printf("%u", slurm_ipmi_conf.session_timeout);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMITimeout");
	key_pair->value = xstrdup_printf("%u", slurm_ipmi_conf.timeout);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMIUsername");
	key_pair->value = xstrdup(slurm_ipmi_conf.username);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMIWorkaroundFlags");
	key_pair->value = xstrdup_printf(
		"%u", slurm_ipmi_conf.workaround_flags);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMIFlags");
	key_pair->value = xstrdup_printf("%u", slurm_ipmi_conf.ipmi_flags);
	list_append(*data, key_pair);
	
	return;
}
