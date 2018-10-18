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

#include "acct_gather_energy_xcc.h"
#include <math.h>
#include <sys/syscall.h>
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
#define IPMI_RAW_MAX_ARGS (65536*2) /* Max ipmi response length*/	
/*FIXME: Investigate which is the OVERFLOW limit for XCC*/
#define IPMI_XCC_OVERFLOW INFINITE /* XCC overflows at X */

const char plugin_name[] = "AcctGatherEnergy XCC plugin";
const char plugin_type[] = "acct_gather_energy/xcc";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;

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
	struct timeval first_read_time; /* First time in this thread */
	struct timeval prev_read_time;  /* Previous read time */
	struct timeval curr_read_time;  /* Current read timestamp by BMC */
	time_t poll_time;  /* Time when we gathered this sensor */
	uint32_t base_j; /* Initial energy sensor value (in joules) */ 
	uint64_t curr_j; /* Consumed joules in the last reading */
	uint64_t prev_j; /* Consumed joules in the previous reading */
	uint32_t low_j; /* The lowest watermark seen for consumed energy */
	uint32_t high_j; /* The highest watermark seen for consumed energy */
	uint32_t low_elapsed_s; /* Time elapsed on the lowest watermark */
	uint32_t high_elapsed_s; /* Time elapsed on the highest watermark */
	uint16_t overflows; /* Number of overflows of the counter, for debug */
} sensor_status_t;

/* Global vars */

uint8_t cmd_rq[8] = { 0x00, 0x3A, 0x32, 4, 2, 0, 0, 0 }; /* LUN, NetFN, CMD, Data[n]*/
unsigned int cmd_rq_len = 8;

static sensor_status_t * xcc_sensor = NULL;

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

/* Thread scope global vars */
__thread ipmi_ctx_t ipmi_ctx = NULL;

#if _DEBUG
static void _print_xcc_sensor()
{
	if (!xcc_sensor) {
		info("xcc_sensor: not initialized");
		return;
	}
	info("xcc_sensor:\n"
	     "first_read_time=%ld\n"
	     "prev_read_time=%ld\n"
	     "curr_read_time=%ld\n"
	     "polltime=%ld\n"
	     "base_j=%d\n"
	     "curr_j=%"PRIu64"\n"
	     "prev_j=%"PRIu64"\n"
	     "low_j=%d\n"
	     "high_j=%d\n"
	     "low_elapsed_s=%d\n"
	     "high_elapsed_s=%d\n"
	     "overflows=%d\n",
	     xcc_sensor->first_read_time.tv_sec,
	     xcc_sensor->prev_read_time.tv_sec,
	     xcc_sensor->curr_read_time.tv_sec,
	     xcc_sensor->poll_time,
	     xcc_sensor->base_j,
	     xcc_sensor->curr_j,
	     xcc_sensor->prev_j,
	     xcc_sensor->low_j,
	     xcc_sensor->high_j,
	     xcc_sensor->low_elapsed_s,
	     xcc_sensor->high_elapsed_s,
	     xcc_sensor->overflows);
}
#endif

static void _reset_slurm_ipmi_conf(slurm_ipmi_conf_t *slurm_ipmi_conf)
{
	if (slurm_ipmi_conf) {
		slurm_ipmi_conf->adjustment = false;
		slurm_ipmi_conf->authentication_type = 0;
		slurm_ipmi_conf->cipher_suite_id = 0;
		xfree(slurm_ipmi_conf->driver_device);
		slurm_ipmi_conf->driver_type = NO_VAL;
		slurm_ipmi_conf->disable_auto_probe = 0;
		slurm_ipmi_conf->driver_address = 0;
		slurm_ipmi_conf->freq = DEFAULT_IPMI_FREQ;
		xfree(slurm_ipmi_conf->password);
		slurm_ipmi_conf->password = xstrdup(DEFAULT_IPMI_PASS);
		slurm_ipmi_conf->privilege_level = 0;
		slurm_ipmi_conf->protocol_version = 0;
		slurm_ipmi_conf->register_spacing = 0;
		slurm_ipmi_conf->retransmission_timeout = 0;
		slurm_ipmi_conf->session_timeout = 0;
		slurm_ipmi_conf->timeout = DEFAULT_IPMI_TIMEOUT;
		xfree(slurm_ipmi_conf->username);
		slurm_ipmi_conf->username = xstrdup(DEFAULT_IPMI_USER);
		slurm_ipmi_conf->workaround_flags = 0; // See man 8 ipmi-raw
		slurm_ipmi_conf->ipmi_flags = IPMI_FLAGS_DEFAULT; //IPMI_FLAGS_DEBUG_DUMP
		slurm_ipmi_conf->target_channel_number_is_set = false;
		slurm_ipmi_conf->target_slave_address_is_set = false;
		slurm_ipmi_conf->target_channel_number = 0x00;
		slurm_ipmi_conf->target_slave_address = 0x20;
	}
}

/*
 * Returns whether this thread is running in slurmd.
 */
static bool _is_thread_launcher(void)
{
	static bool set = false;
	static bool run = false;

	if (!set) {
		set = true;
		run = run_in_daemon("slurmd");
	}

	return run;
}

/*
 * Returns whether this thread is running in slurmd
 * or in slurmstepd.
 */
static bool _run_in_daemon(void)
{
	static bool set = false;
	static bool run = false;

	if (!set) {
		set = true;
		run = run_in_daemon("slurmd,slurmstepd");
	}

	return run;
}

/*
 * Returns whether energy profiling is set.
 */
static bool _running_profile(void)
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

/*
 * _init_ipmi_config() initializes parameters for freeipmi library and then
 * opens a connection to the in-band device, thus setting up the ipmi_ctx
 * object. Slurm IPMI XCC plugin only supports in-band communications because
 * otherwise the network overhead associated by out-band communications does
 * not permit to mantain a 10mS ample rate, so losing accuracy.
 */
static int _init_ipmi_config (void)
{
	int ret = 0;
	unsigned int workaround_flags_mask =
		(IPMI_WORKAROUND_FLAGS_INBAND_ASSUME_IO_BASE_ADDRESS
		 | IPMI_WORKAROUND_FLAGS_INBAND_SPIN_POLL);

	if (ipmi_ctx != NULL) {
	  debug("ipmi_ctx already initialized\n");
	  return SLURM_SUCCESS;
	}

	if (!(ipmi_ctx = ipmi_ctx_create())) {
		error("ipmi_ctx_create: %s\n", strerror(errno));
		goto cleanup;
	}

	if (getuid() != 0) {
		error ("%s: error : must be root to open ipmi devices\n", __func__);
		goto cleanup;
	}

	/* XCC OEM commands always require to use in-band communication */
	if ((slurm_ipmi_conf.driver_type > 0 &&
	     slurm_ipmi_conf.driver_type != NO_VAL &&
	     slurm_ipmi_conf.driver_type != IPMI_DEVICE_KCS &&
	     slurm_ipmi_conf.driver_type != IPMI_DEVICE_SSIF &&
	     slurm_ipmi_conf.driver_type != IPMI_DEVICE_OPENIPMI &&
	     slurm_ipmi_conf.driver_type != IPMI_DEVICE_SUNBMC)
	    || (slurm_ipmi_conf.workaround_flags & ~workaround_flags_mask)) {
		/* IPMI ERROR PARAMETERS */
		error ("%s: error: XCC Lenovo plugin only supports in-band "
		       "communication, incorrect driver type or workaround "
		       "flags", __func__);

		debug("slurm_ipmi_conf.driver_type=%u"
		      "slurm_ipmi_conf.workaround_flags=%u",
		      slurm_ipmi_conf.driver_type,
		      slurm_ipmi_conf.workaround_flags);

		goto cleanup;
	}
	
	if (slurm_ipmi_conf.driver_type == NO_VAL)
	{
		if ((ret = ipmi_ctx_find_inband (ipmi_ctx,
						 NULL,
						 slurm_ipmi_conf.disable_auto_probe,
						 slurm_ipmi_conf.driver_address,
						 slurm_ipmi_conf.register_spacing,
						 slurm_ipmi_conf.driver_device,
						 slurm_ipmi_conf.workaround_flags,
						 slurm_ipmi_conf.ipmi_flags)) <= 0)
		{
			error ("%s: error on ipmi_ctx_find_inband: %s",
			       __func__, ipmi_ctx_errormsg(ipmi_ctx));

			debug("slurm_ipmi_conf.driver_type=%u\n"
			      "slurm_ipmi_conf.disable_auto_probe=%u\n"
			      "slurm_ipmi_conf.driver_address=%u\n"
			      "slurm_ipmi_conf.register_spacing=%u\n"
			      "slurm_ipmi_conf.driver_device=%s\n"
			      "slurm_ipmi_conf.workaround_flags=%u\n"
			      "slurm_ipmi_conf.ipmi_flags=%u\n",
			      slurm_ipmi_conf.driver_type,
			      slurm_ipmi_conf.disable_auto_probe,
			      slurm_ipmi_conf.driver_address,
			      slurm_ipmi_conf.register_spacing,
			      slurm_ipmi_conf.driver_device,
			      slurm_ipmi_conf.workaround_flags,
			      slurm_ipmi_conf.ipmi_flags);

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
			error ("%s: error on ipmi_ctx_open_inband: %s",
			       __func__, ipmi_ctx_errormsg (ipmi_ctx));

			debug("slurm_ipmi_conf.driver_type=%u\n"
			      "slurm_ipmi_conf.disable_auto_probe=%u\n"
			      "slurm_ipmi_conf.driver_address=%u\n"
			      "slurm_ipmi_conf.register_spacing=%u\n"
			      "slurm_ipmi_conf.driver_device=%s\n"
			      "slurm_ipmi_conf.workaround_flags=%u\n"
			      "slurm_ipmi_conf.ipmi_flags=%u\n",
			      slurm_ipmi_conf.driver_type,
			      slurm_ipmi_conf.disable_auto_probe,
			      slurm_ipmi_conf.driver_address,
			      slurm_ipmi_conf.register_spacing,
			      slurm_ipmi_conf.driver_device,
			      slurm_ipmi_conf.workaround_flags,
			      slurm_ipmi_conf.ipmi_flags);
			goto cleanup;
		}
	}

	if (slurm_ipmi_conf.target_channel_number_is_set
	    || slurm_ipmi_conf.target_slave_address_is_set)
	{
		if (ipmi_ctx_set_target(ipmi_ctx,
					 slurm_ipmi_conf.target_channel_number_is_set ?
					&slurm_ipmi_conf.target_channel_number : NULL,
					 slurm_ipmi_conf.target_slave_address_is_set ?
					&slurm_ipmi_conf.target_slave_address : NULL) < 0)
		  {
		    error ("%s: error on ipmi_ctx_set_target: %s",
			   __func__, ipmi_ctx_errormsg (ipmi_ctx));
		    goto cleanup;
		  }
	}

	return SLURM_SUCCESS;

cleanup:
	ipmi_ctx_close(ipmi_ctx);
	ipmi_ctx_destroy(ipmi_ctx);
	return SLURM_FAILURE;
}

/* 
 * _read_ipmi_values() reads the XCC sensor doing an OEM call to ipmi.
 * It returns NULL if the reading was unable to complete.
 */
static xcc_raw_single_data_t * _read_ipmi_values(void)
{
	xcc_raw_single_data_t * xcc_reading;
	uint8_t *buf_rs;
	int i;
	int rs_len = 0;

	debug("FMOLL: I am inquiring the IPMI, tid %ld progrname %s, ipmi_ctx is %p and xcc_sensor is %p",
	      syscall(SYS_gettid), slurm_prog_name, ipmi_ctx, xcc_sensor);

	if (!IPMI_NET_FN_RQ_VALID(cmd_rq[1])) {
                error("Invalid netfn value\n");
		return 0;
	}

        buf_rs = xmalloc(IPMI_RAW_MAX_ARGS * sizeof (uint8_t));
        rs_len = ipmi_cmd_raw(ipmi_ctx,
                              cmd_rq[0], //Lun (logical unit number)
                              cmd_rq[1], //Net Function
                              &cmd_rq[2], //Command number + request data
                              cmd_rq_len - 2, //Length (in bytes)
                              buf_rs, //response buffer
                              IPMI_RAW_MAX_ARGS //max response length
                );

        debug3("ipmi_cmd_raw: %s\n", ipmi_ctx_errormsg(ipmi_ctx));

        if (rs_len < 0) {
		error("Invalid ipmi response length");
                xfree(buf_rs);
		return 0;	       
	}

	/* Because of memory alineation we must copy the data from the buffer */
	xcc_reading = xmalloc(sizeof(xcc_raw_single_data_t));
	memcpy(&xcc_reading->fifo_inx, buf_rs+2, 2);
        memcpy(&xcc_reading->j, buf_rs+4, 4);
        memcpy(&xcc_reading->mj, buf_rs+8, 2);
        memcpy(&xcc_reading->s, buf_rs+10, 4);
        memcpy(&xcc_reading->ms, buf_rs+14, 2);

#if _DEBUG	
        printf("sent: ");
        for (i = 0; i < cmd_rq_len; i++)
                printf("%02X ", cmd_rq[i]);
        printf("\nrcvd: ");
        for (i = 0; i < cmd_rq_len+100; i++)
                printf("%02X ", buf_rs[i]);
        printf("\n");
        
	printf("0x%04x xcc_reading->fifo_inx = %d,"
	       "0x%08x xcc_reading->j = %d ,"
               "0x%04x xcc_reading->mj = %d ,"
               "0x%08x xcc_reading->s = %d , "
               "0x%04x xcc_reading->ms %d\n",
	       xcc_reading->fifo_inx, xcc_reading->fifo_inx,
               xcc_reading->j, xcc_reading->j,
	       xcc_reading->mj, xcc_reading->mj,
	       xcc_reading->s, xcc_reading->s,
	       xcc_reading->ms, xcc_reading->ms);
#endif
	xfree(buf_rs);
	return xcc_reading;
}

/* FIXME: Convert this function to a MACRO */
static uint32_t _elapsed_last_interval_s()
{
        uint32_t elapsed = xcc_sensor->curr_read_time.tv_sec -
	                   xcc_sensor->prev_read_time.tv_sec;
	return (elapsed < 0) ? 0 : elapsed;
}

/* FIXME: Convert this function to a MACRO */
static uint32_t _consumed_last_interval_j()
{
        uint32_t consumed =  xcc_sensor->curr_j - xcc_sensor->prev_j;
	return (consumed < 0) ? 0 : consumed;
}

/* 
 * _curr_watts() reads the xcc_sensor data and return the consumed watts since
 *  the last reading.
 */
static uint32_t _curr_watts()
{
	uint32_t joules,seconds;

	seconds =_elapsed_last_interval_s();
	joules = _consumed_last_interval_j();

#if _DEBUG
	info("%s, joules = %d, elapsed seconds = %d, watts: %d",
	     __func__,
	     joules, seconds, (uint32_t) round((double) joules/seconds));
#endif

	if (joules <= 0 || seconds <= 0)
		return 0;
	
	return (uint32_t) round((double) joules/seconds);
}

/*
 * _thread_update_node_energy() calls to _read_ipmi_values() to get the XCC
 * sensor values, and updates the sensor status struct about node consumption.
 */
static int _thread_update_node_energy(void)
{
	xcc_raw_single_data_t * xcc_raw;
	uint32_t offset;

	xcc_raw = _read_ipmi_values();

	if (!xcc_raw) {
		error("%s could not read XCC ipmi values", __func__);
		return SLURM_FAILURE;
	}

	xcc_sensor->poll_time = time(NULL);
	xcc_sensor->prev_read_time.tv_sec = xcc_sensor->curr_read_time.tv_sec;
	xcc_sensor->curr_read_time.tv_sec = xcc_raw->s;
	xcc_sensor->prev_j = xcc_sensor->curr_j;

	/* Detect an overflow */
	offset =  xcc_raw->j +
	          (IPMI_XCC_OVERFLOW * xcc_sensor->overflows) -
                  xcc_sensor->prev_j;
	if (offset < 0) {
		xcc_sensor->overflows++;
		xcc_sensor->curr_j = xcc_sensor->prev_j +
		  ((IPMI_XCC_OVERFLOW * xcc_sensor->overflows) -
		   xcc_sensor->prev_j) + xcc_raw->j;
	} else {
		xcc_sensor->curr_j = xcc_raw->j;
	}

#if _DEBUG
	info("%s called, printing xcc info", __func__);
	_print_xcc_sensor();
#endif
	/* Record the interval with highest/lowest consumption. */
	uint32_t c_j = _consumed_last_interval_j();
	uint32_t e_s = _elapsed_last_interval_s();

	if (xcc_sensor->low_j == 0 || xcc_sensor->low_j > c_j) {
		xcc_sensor->low_j = c_j;
		xcc_sensor->low_elapsed_s = e_s;
	}
	if (xcc_sensor->high_j == 0 || xcc_sensor->high_j < c_j) {
		xcc_sensor->high_j = c_j;
		xcc_sensor->high_elapsed_s = e_s;
	}

	if (debug_flags & DEBUG_FLAG_ENERGY) {
		info("ipmi-thread: XCC current_watts: %u\n"
		     "consumed energy last interval: %u Joules\n"
		     "elapsed time last interval: %u Seconds\n"
		     "first read time unix timestamp: %ld\n"
		     "first read energy counter val: %u\n",
		     _curr_watts(),
		     _consumed_last_interval_j(),
		     _elapsed_last_interval_s(),
		     xcc_sensor->first_read_time.tv_sec,
		     xcc_sensor->base_j);
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
	xcc_raw_single_data_t * xcc_raw;
	
	debug("FMOLL: I am %ld and first=%d and ipmi_ctx=%p and progrname is %s and xcc_sensor is %p",
	      syscall(SYS_gettid), first, ipmi_ctx, slurm_prog_name, xcc_sensor);

	if (!first) {
	  /*
	   * If we are here we are a new slurmd thread serving
	   * a request. In that case we must init a new ipmi_ctx,
	   * update the sensor and return because the freeipmi lib
	   * context cannot be shared among threads.
	   */
	  if (_init_ipmi_config() != SLURM_SUCCESS)
	    if (debug_flags & DEBUG_FLAG_ENERGY) {
	      info("%s thread init error on _init_ipmi_config()",
		   plugin_name);
	      goto cleanup; 
	    }
	  return first_init;
	}
	first = false;

	if (_init_ipmi_config() != SLURM_SUCCESS) {
		if (debug_flags & DEBUG_FLAG_ENERGY)
			info("%s thread init error on _init_ipmi_config()",
			     plugin_name);
		goto cleanup;
	}

	xcc_raw = _read_ipmi_values();	
	if (!xcc_raw) {
		error("%s could not read XCC ipmi values", __func__);
		goto cleanup;
	}

	if (!xcc_sensor) {
	  xcc_sensor = xmalloc(sizeof(sensor_status_t));
	  memset(xcc_sensor, 0, sizeof(sensor_status_t));
	  
	  /* Let's fill the xcc_sensor with the first reading */
	  xcc_sensor->poll_time = time(NULL);
	  xcc_sensor->first_read_time.tv_sec = xcc_raw->s;
	  xcc_sensor->prev_read_time.tv_sec = xcc_raw->s;
	  xcc_sensor->curr_read_time.tv_sec = xcc_raw->s;
	  xcc_sensor->base_j = xcc_raw->j;
	  xcc_sensor->curr_j = xcc_raw->j;
	  xcc_sensor->prev_j = xcc_raw->j;
	} else {
	  debug3("There is a xcc_sensor already initialized, we are a child: %ld", syscall(SYS_gettid));
	}	 

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

/*
 * _ipmi_send_profile() fills a new dataset with the sensor information and
 * sends it to the profiling plugin.
 */
static int _ipmi_send_profile(void)
{
	int i;
	uint64_t data[4]; // Energy,[Max|Min|Avg]Power

	if (!_running_profile())
		return SLURM_SUCCESS;

	/* Create the dataset structure in profile plugin and get the id */
	if (dataset_id < 0) {
		acct_gather_profile_dataset_t dataset[5];
		dataset[0].name = xstrdup("Energy");
		dataset[1].name = xstrdup("MaxPower");
		dataset[2].name = xstrdup("MinPower");
		dataset[3].name = xstrdup("AvgPower");
		dataset[4].name = xstrdup("CurrPower");

		dataset[0].type = PROFILE_FIELD_UINT64;
		dataset[1].type = PROFILE_FIELD_UINT64;
		dataset[2].type = PROFILE_FIELD_UINT64;
		dataset[3].type = PROFILE_FIELD_UINT64;
		dataset[4].type = PROFILE_FIELD_UINT64;

		dataset[5].name = NULL;
		dataset[5].type = PROFILE_FIELD_NOT_SET;

		dataset_id = acct_gather_profile_g_create_dataset(
			"Energy", NO_PARENT, dataset);

		for (i = 0; i < 6; ++i)
			xfree(dataset[i].name);

		if (debug_flags & DEBUG_FLAG_ENERGY)
			debug("Energy: XCC dataset created (id = %d)", dataset_id);
		
		if (dataset_id == SLURM_ERROR) {
			error("Energy: Failed to create the dataset for XCC");
			return SLURM_ERROR;
		}
	}

	/* Pack and send the data to the dataset id.*/
	memset(data, 0, sizeof(data));
	data[0] = xcc_sensor->curr_j - xcc_sensor->base_j;	
	data[1] = xcc_sensor->high_j/xcc_sensor->high_elapsed_s;
	data[2] = xcc_sensor->low_j/xcc_sensor->low_elapsed_s;
	data[3] = (xcc_sensor->curr_j - xcc_sensor->base_j) /
  	           (xcc_sensor->curr_read_time.tv_sec -
		    xcc_sensor->first_read_time.tv_sec);	
	data[4] = _curr_watts();
	if (debug_flags & DEBUG_FLAG_PROFILE) {
		info("PROFILE-Energy: ConsumedEnergy=%"PRIu64"", data[0]);
		info("PROFILE-Energy: MaxPower=%"PRIu64"", data[1]);
		info("PROFILE-Energy: MinPower=%"PRIu64"", data[2]);
		info("PROFILE-Energy: AvgPower=%"PRIu64"", data[3]);
		info("PROFILE-Energy: CurrPower=%"PRIu64"", data[4]);
	}
	
	return acct_gather_profile_g_add_sample_data(dataset_id, (void *)data,
						     (time_t)xcc_sensor->
						     curr_read_time.tv_sec);
}


/*
 * _thread_ipmi_run() stays in a loop until shutdown, just updating the node
 * energy reading with a call to _thread_update_node_energy() and then waiting
 * for EnergyIPMIFrequency (acct_gather.conf) seconds.
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
	later.tv_nsec = 0;

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

/*
 * _thread_launcher manages the main ipmi thread and remains waiting
 * until it fails or fini() is called. After fini() it will cancel
 * the launched  main ipmi thread.
 */
static void *_thread_launcher(void *no_data)
{
	struct timeval now;
	struct timespec timeout;

	slurm_thread_create(&thread_ipmi_id_run, _thread_ipmi_run, NULL);

	/* Wait for thread launch success for a max. of EnergyIPMITimeout */
	gettimeofday(&now, NULL);
	timeout.tv_sec = now.tv_sec + slurm_ipmi_conf.timeout;
        timeout.tv_nsec = 0;

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
 *  _get_joules_task() mantains the local sensor information for this
 * specific task and remove global node calculations out of the mix.
 * The data is obtained from the cache (slurmd sensor xcc_sensor) and
 * depending in the delta value may call ENERGY_DATA_JOULES_TASK to get
 * a new measurement, or ENERGY_DATA_STRUCT to just gather the current
 * cached information.
 */
static int _get_joules_task(uint16_t delta)
{
	acct_gather_energy_t *energy = NULL;
	uint16_t sensor_cnt = 0;
	uint32_t offset;

	debug("FMOLL: I am in get_joules_task, tid %ld "
	      "xcc_sensor is %p, progrname is %s", syscall(SYS_gettid),
	      xcc_sensor, slurm_prog_name);

        /* FIXME: 'delta' parameter
	 * Means "use cache" if data is newer than delta seconds ago, otherwise
	 * just query the sensor again
	 */

	if (slurm_get_node_energy(NULL, delta, &sensor_cnt, &energy)) {
		error("_get_joules_task: can't get info from slurmd");
		return SLURM_ERROR;
	}

	if (sensor_cnt != 1) {
		error("_get_joules_task: received %u xcc sensors expected 1",
		      sensor_cnt);
		acct_gather_energy_destroy(energy);
		return SLURM_ERROR;
	}

	/* 
	 * Here we are in a task/job, therefore we must record the start
	 * time of now, and take all the previous consumption out of the mix.
	 * If the xcc_sensor is still not init'ed (new slurmstepd fork),
	 * we create it.
	 */
	if (!xcc_sensor) {
		xcc_sensor = xmalloc(sizeof(sensor_status_t));
		memset(xcc_sensor, 0, sizeof(sensor_status_t));

		/*
		 * Record data for the step, so take all the previous
		 * consumption out of the mix setting the base_j and
		 * first poll time to the current value.
		 */
		xcc_sensor->poll_time = time(NULL);
		xcc_sensor->base_j = energy->consumed_energy;
		xcc_sensor->first_read_time.tv_sec = energy->poll_time;

		xcc_sensor->prev_read_time.tv_sec = energy->poll_time;
		xcc_sensor->curr_read_time.tv_sec = energy->poll_time;
		xcc_sensor->curr_j = xcc_sensor->base_j;
		xcc_sensor->prev_j = xcc_sensor->base_j;

		xcc_sensor->low_j = 0;
		xcc_sensor->high_j = 0;
		xcc_sensor->low_elapsed_s = 0;
		xcc_sensor->high_elapsed_s = 0;
	} else {
	        /*
		 * Update this task's local sensor with the latest cached energy reading
		 * from the global sensor.
		 */
		xcc_sensor->poll_time = time(NULL);
		xcc_sensor->prev_j = xcc_sensor->curr_j;
		xcc_sensor->prev_read_time = xcc_sensor->curr_read_time;
 		xcc_sensor->curr_read_time.tv_sec = energy->poll_time;

		/* Detect an overflow - slurmd may be restarted */
		offset =  energy->consumed_energy +
	          (IPMI_XCC_OVERFLOW * xcc_sensor->overflows) -
                  xcc_sensor->prev_j;
		if (offset < 0) {
		  xcc_sensor->overflows++;
		  xcc_sensor->curr_j = xcc_sensor->prev_j +
		    ((IPMI_XCC_OVERFLOW * xcc_sensor->overflows) -
		     xcc_sensor->prev_j) + energy->consumed_energy;
		} else {
		  xcc_sensor->curr_j = energy->consumed_energy;
		}

		/* Tally the interval with highest/lowest consumption */
		uint32_t c_mj = _consumed_last_interval_j();
		uint32_t e_ms = _elapsed_last_interval_s();
		
		if (xcc_sensor->low_j == 0 || xcc_sensor->low_j > c_mj) {
			xcc_sensor->low_j = c_mj;
			xcc_sensor->low_elapsed_s = e_ms;
		}
		if (xcc_sensor->high_j == 0 || xcc_sensor->high_j < c_mj) {
			xcc_sensor->high_j = c_mj;
			xcc_sensor->high_elapsed_s = e_ms;
		}
	}
	
	if (debug_flags & DEBUG_FLAG_ENERGY) {
		info("%s: XCC local task current_watts: %u, \n"
		     "consumed energy last interval: %u Joules\n"
		     "elapsed time last interval: %u Seconds\n"
		     "first read time unix timestamp: %ld\n"
		     "first read energy counter val: %u",
		     __func__,
		     _curr_watts(),
		     _consumed_last_interval_j(),
		     _elapsed_last_interval_s(),
		     xcc_sensor->first_read_time.tv_sec,
		     xcc_sensor->base_j);
	}

	return SLURM_SUCCESS;
}

/*
 * _xcc_to_energy() translates the xcc_sensor data to an energy struct.
 * 
 * FIXME: In the future, the energy struct may be extended to include
 * all the fields we have here, like [Max|Min|Avg]Power.
 */
static void _xcc_to_energy(acct_gather_energy_t *energy)
{
	if (!xcc_sensor || !energy)
		return;

	memset(energy, 0, sizeof(acct_gather_energy_t));

	if (xcc_sensor->low_j == 0 || xcc_sensor->low_elapsed_s == 0)
		energy->base_watts = 0;
	else
		energy->base_watts = xcc_sensor->low_j /
				     xcc_sensor->low_elapsed_s;

	energy->previous_consumed_energy = xcc_sensor->prev_j;
	energy->consumed_energy = (xcc_sensor->curr_j - xcc_sensor->base_j);
	energy->base_consumed_energy = xcc_sensor->low_j;
	energy->poll_time = xcc_sensor->curr_read_time.tv_sec;
	energy->current_watts = _curr_watts();
}

/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
extern int init(void)
{
	debug_flags = slurm_get_debug_flags();
	return SLURM_SUCCESS;
}

extern int fini(void)
{
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
		ipmi_ctx_destroy(ipmi_ctx);
	_reset_slurm_ipmi_conf(&slurm_ipmi_conf);

	slurm_mutex_unlock(&ipmi_mutex);

	if (thread_ipmi_id_run)
		pthread_join(thread_ipmi_id_run, NULL);

	xfree(xcc_sensor);

	return SLURM_SUCCESS;
}

/* The energy is updated by the thread, so do nothing here. */ 
extern int acct_gather_energy_p_update_node_energy(void)
{
	int rc = SLURM_SUCCESS;
	xassert(_run_in_daemon());
	return rc;
}

extern int acct_gather_energy_p_get_data(enum acct_energy_type data_type,
					 void *data)
{
	int rc = SLURM_SUCCESS;

	/* This buffer can be one of these types */
	acct_gather_energy_t *energy = (acct_gather_energy_t *)data;
	time_t *last_poll = (time_t *)data;
	uint16_t *sensor_cnt = (uint16_t *)data;

	xassert(_run_in_daemon());

	debug("FMOLL: heyheyhey my name is %s tid %ld", slurm_prog_name, syscall(SYS_gettid));
	switch (data_type) {
	case ENERGY_DATA_NODE_ENERGY:
	  /*
	   * We must return the xcc_sensor reading. Usually this
	   * case will be issued only for slurmd and not for a task.
	   */
	  debug("CALLED ENERGY_DATA_NODE_ENERGY in %s", __func__);
	  slurm_mutex_lock(&ipmi_mutex);
	  if (!energy)
			energy = xmalloc(sizeof(acct_gather_energy_t));
		_xcc_to_energy(energy);
		slurm_mutex_unlock(&ipmi_mutex);
		break;
	case ENERGY_DATA_JOULES_TASK:
	  /*
	   * Here we are asked to return a new reading, so if we are the
	   * slurmd thread we should just run a ipmi call and return the energy.
	   *
	   * If we are a task, we call to _get_joules_task to inquiry
	   * slurmd for a the reading.
	   */
	  debug("CALLED ENERGY_DATA_JOULES_TASK in %s", __func__);
		slurm_mutex_lock(&ipmi_mutex);
		if (_is_thread_launcher()) {
			if (_thread_init() == SLURM_SUCCESS)
				_thread_update_node_energy();
		} else {
		  /* 
		   * If more than 60 seconds elapsed since last timestamp,
		   * ask for new reading.
		   */
		        _get_joules_task(10);
		}
		if (!energy)
			energy = xmalloc(sizeof(acct_gather_energy_t));
		_xcc_to_energy(energy);
		slurm_mutex_unlock(&ipmi_mutex);
		break;
	case ENERGY_DATA_STRUCT:
	  /*
	   * Return the xcc_sensor, be it from slurmd with global
	   * node data, or be it from slurmstepd.
	   */
	  debug("CALLED ENERGY_DATA_STRUCT in %s", __func__);
		slurm_mutex_lock(&ipmi_mutex);
		if (!energy)
			energy = xmalloc(sizeof(acct_gather_energy_t));
		_xcc_to_energy(energy);
		slurm_mutex_unlock(&ipmi_mutex);
		break;
	case ENERGY_DATA_NODE_ENERGY_UP:
	  /*
	   * This case requests an update of the the sensor cache, so we
	   * must run the ipmi call or inquiry slurmd for fresh data.
	   * Usually called per node.
	   * If we are a task, force a sensor reading only if more than
	   * 10 seconds elapsed since last poll.
	   */
	  debug("CALLED ENERGY_DATA_NODE_ENERGY_UP in %s", __func__);
		slurm_mutex_lock(&ipmi_mutex);
		if (_is_thread_launcher()) {
			if (_thread_init() == SLURM_SUCCESS)
				_thread_update_node_energy();
		} else {
			_get_joules_task(10);
		}
		_xcc_to_energy(energy);
		slurm_mutex_unlock(&ipmi_mutex);
		break;
	case ENERGY_DATA_LAST_POLL:
	  /* Last timestamp we gathered sensor data */
	  debug("CALLED ENERGY_DATA_LAST_POLL in %s", __func__);
		slurm_mutex_lock(&ipmi_mutex);
		if (xcc_sensor)
			*last_poll = xcc_sensor->poll_time;
		else
			*last_poll = 0;
		slurm_mutex_unlock(&ipmi_mutex);
		break;
	case ENERGY_DATA_SENSOR_CNT:
	  debug("CALLED ENERGY_DATA_SENSOR_CNT in %s", __func__);
		*sensor_cnt = 1;
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

/* First entry function for slurmd startup */
extern void acct_gather_energy_p_conf_set(s_p_hashtbl_t *tbl)
{
	/* Set initial values */
	_reset_slurm_ipmi_conf(&slurm_ipmi_conf);

	if (tbl) {
		/* ipmi initialization parameters */
		s_p_get_boolean(&(slurm_ipmi_conf.adjustment),
				"EnergyIPMICalcAdjustment", tbl);
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
			       "EnergyIPMIUsername", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.workaround_flags,
			       "EnergyIPMIWorkaroundFlags", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.ipmi_flags,
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

