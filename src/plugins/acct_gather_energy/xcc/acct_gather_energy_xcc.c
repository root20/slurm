/*****************************************************************************\
 *  acct_gather_energy_xcc.c - slurm energy accounting plugin for xcc.
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
#include <math.h>

#include "src/common/slurm_xlator.h"
#include "src/common/slurm_acct_gather_energy.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/fd.h"

#include "src/slurmd/common/proctrack.h"

#include "src/slurmd/slurmd/slurmd.h"

/*
 * freeipmi includes for the lib
 */
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

#define DEFAULT_IPMI_FREQ 30
#define DEFAULT_IPMI_USER "USERID"
#define DEFAULT_IPMI_PASS "PASSW0RD"
#define DEFAULT_IPMI_TIMEOUT 10

#define IPMI_VERSION 2		/* Data structure version number */
#define MAX_LOG_ERRORS 5	/* Max sensor reading errors log messages */
#define XCC_MIN_RES 50        /* Minimum resolution for XCC readings, in ms */
#define IPMI_RAW_MAX_ARGS 256 /* Max XCC response length in bytes*/
/*FIXME: Investigate which is the OVERFLOW limit for XCC*/
#define IPMI_XCC_OVERFLOW INFINITE /* XCC overflows at X */

#define XCC_FLAG_NONE 0x00000000
#define XCC_FLAG_FAKE 0x00000001

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  Slurm uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "jobacct" for Slurm job completion logging) and <method>
 * is a description of how this plugin satisfies that application.  Slurm will
 * only load job completion logging plugins if the plugin_type string has a
 * prefix of "jobacct/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */

const char plugin_name[] = "AcctGatherEnergy IPMI plugin";
const char plugin_type[] = "acct_gather_energy/ipmi";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;

/*
 * internal variables
 */
typedef struct slurm_ipmi_conf {
	/* Adjust/approach the consumption
	 * in function of time between ipmi update and read call */
	bool adjustment;
	/* authentication type to use
	 *   IPMI_MONITORING_AUTHENTICATION_TYPE_NONE                  = 0x00,
	 *   IPMI_MONITORING_AUTHENTICATION_TYPE_STRAIGHT_PASSWORD_KEY = 0x01,
	 *   IPMI_MONITORING_AUTHENTICATION_TYPE_MD2                   = 0x02,
	 *   IPMI_MONITORING_AUTHENTICATION_TYPE_MD5                   = 0x03,
	 * Pass < 0 for default of IPMI_MONITORING_AUTHENTICATION_TYPE_MD5*/
	uint32_t authentication_type;
	/* Cipher suite identifier to determine authentication, integrity,
	 * and confidentiality algorithms to use.
	 * Supported Cipher Suite IDs
	 * (Key: A - Authentication Algorithm
	 *       I - Integrity Algorithm
	 *       C - Confidentiality Algorithm)
	 *   0 - A = None; I = None; C = None
	 *   1 - A = HMAC-SHA1; I = None; C = None
	 *   2 - A = HMAC-SHA1; I = HMAC-SHA1-96; C = None
	 *   3 - A = HMAC-SHA1; I = HMAC-SHA1-96; C = AES-CBC-128
	 *   6 - A = HMAC-MD5; I = None; C = None
	 *   7 - A = HMAC-MD5; I = HMAC-MD5-128; C = None
	 *   8 - A = HMAC-MD5; I = HMAC-MD5-128; C = AES-CBC-128
	 *   11 - A = HMAC-MD5; I = MD5-128; C = None
	 *   12 - A = HMAC-MD5; I = MD5-128; C = AES-CBC-128
	 *   15 - A = HMAC-SHA256; I = None; C = None
	 *   16 - A = HMAC-SHA256; I = HMAC-SHA256-128; C = None
	 *   17 - A = HMAC-SHA256; I = HMAC-SHA256-128; C = AES-CBC-128
	 * Pass < 0 for default.of 3.*/
	uint32_t cipher_suite_id;
	/* Flag informs the library if in-band driver information should be
	 * probed or not.*/
	uint32_t disable_auto_probe;
	/* Use this specified driver address instead of a probed one.*/
	uint32_t driver_address;
	/* Use this driver device for the IPMI driver.*/
	char *driver_device;
	/* Options for IPMI configuration*/
	/* Use a specific in-band driver.
	 *   IPMI_MONITORING_DRIVER_TYPE_KCS      = 0x00,
	 *   IPMI_MONITORING_DRIVER_TYPE_SSIF     = 0x01,
	 *   IPMI_MONITORING_DRIVER_TYPE_OPENIPMI = 0x02,
	 *   IPMI_MONITORING_DRIVER_TYPE_SUNBMC   = 0x03,
	 *    Pass < 0 for default of IPMI_MONITORING_DRIVER_TYPE_KCS.*/
	uint32_t driver_type;
	uint32_t flags;
	/* frequency for ipmi call*/
	uint32_t freq;
	uint32_t ipmi_flags;
	/* BMC password. Pass NULL ptr for default password.  Standard
	 * default is the null (e.g. empty) password.  Maximum length of 20
	 * bytes.*/
	char *password;
	/* privilege level to authenticate with.
	 * Supported privilege levels:
	 *   0 = IPMICONSOLE_PRIVILEGE_USER
	 *   1 = IPMICONSOLE_PRIVILEGE_OPERATOR
	 *   2 = IPMICONSOLE_PRIVILEGE_ADMIN
	 * Pass < 0 for default of IPMICONSOLE_PRIVILEGE_ADMIN.*/
	uint32_t privilege_level;
	/* Flag informs the library if in-band driver information should be
	 * probed or not.*/
	/* Indicate the IPMI protocol version to use
	 * IPMI_MONITORING_PROTOCOL_VERSION_1_5 = 0x00,
	 * IPMI_MONITORING_PROTOCOL_VERSION_2_0 = 0x01,
	 * Pass < 0 for default of IPMI_MONITORING_VERSION_1_5.*/
	uint32_t protocol_version;
	/* Use this register space instead of the probed one.*/
	uint32_t register_spacing;
	/* Specifies the packet retransmission timeout length in
	 * milliseconds.  Pass <= 0 to default 500 (0.5 seconds).*/
	uint32_t retransmission_timeout;
	/* Specifies the session timeout length in milliseconds.  Pass <= 0
	 * to default 60000 (60 seconds).*/
	uint32_t session_timeout;
	uint8_t target_channel_number;
	bool target_channel_number_is_set;
	uint8_t target_slave_address;
	bool target_slave_address_is_set;
	/* Timeout for the ipmi thread */
	uint32_t timeout;
	/* BMC username. Pass NULL ptr for default username.  Standard
	 * default is the null (e.g. empty) username.  Maximum length of 16
	 * bytes.*/
	char *username;
	/* Bitwise OR of flags indicating IPMI implementation changes.  Some
	 * BMCs which are non-compliant and may require a workaround flag
	 * for correct operation. Pass IPMICONSOLE_WORKAROUND_DEFAULT for
	 * default.  Standard default is 0, no modifications to the IPMI
	 * protocol.*/
	uint32_t workaround_flags;
} slurm_ipmi_conf_t;

/* Struct to store the raw single data command reading */
typedef struct xcc_raw_single_data {
	uint16_t fifo_inx;
	uint32_t j;
	uint16_t mj;
	uint16_t ms;
	uint32_t s;
} xcc_raw_single_data_t;

/* array of struct to track the status of multiple sensors */
typedef struct sensor_status {
	uint32_t elapsed_s_high; /* Time elapsed on the highest watermark */
	uint32_t elapsed_s_low; /* Time elapsed on the lowest watermark */
	uint32_t j_base; /* Initial energy sensor value (in joules) */
	uint64_t j_curr; /* Consumed joules in the last reading */
	uint32_t j_high; /* The highest watermark seen for consumed energy */
	uint32_t j_low; /* The lowest watermark seen for consumed energy */
	uint64_t j_prev; /* Consumed joules in the previous reading */
	uint16_t overflows; /* Number of overflows of the counter, for debug */
	time_t poll_time;  /* Time when we grabbed the last BMC reading */
	/* Timestamp is when the BMC measured it, not when we grab it */
	struct timeval time_first; /* First reading timestamp */
	struct timeval time_prev;  /* Previous reading timestamp */
	struct timeval time_curr;  /* Current reading timestamp */
} sensor_status_t;

/*
 * This enum is directly related to the xcc_labels below.  If this is changed it
 * should be altered as well.
 */
enum {
	XCC_ENERGY = 0,
	XCC_MAX_POWER,
	XCC_MIN_POWER,
	XCC_AVG_POWER,
	XCC_CURR_POWER,
	XCC_LABEL_CNT
};

static char *xcc_labels[] = { "Energy",
			      "MaxPower",
			      "MinPower",
			      "AvgPower",
			      "CurrPower",
			      NULL };

static sensor_status_t * xcc_sensor = NULL;

/* LUN, NetFN, CMD, Data[n]*/
static uint8_t cmd_rq[8] = { 0x00, 0x3A, 0x32, 4, 2, 0, 0, 0 };
static unsigned int cmd_rq_len = 8;


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
static pthread_t thread_ipmi_id_launcher = 0;
static pthread_t thread_ipmi_id_run = 0;

/* Thread scope global vars */
__thread ipmi_ctx_t ipmi_ctx = NULL;

static void _print_xcc_sensor()
{
	if (!xcc_sensor) {
		info("xcc_sensor: not initialized");
		return;
	}

	info("xcc_sensor:\n"
	     "first_reading_tstamp=%lu\n"
	     "prev_reading_tstamp=%lu\n"
	     "curr_reading_tstamp=%lu\n"
	     "polltime=%lu\n"
	     "base_j=%u\n"
	     "curr_j=%"PRIu64"\n"
	     "prev_j=%"PRIu64"\n"
	     "low_j=%u\n"
	     "high_j=%u\n"
	     "low_elapsed_s=%u\n"
	     "high_elapsed_s=%u\n"
	     "overflows=%u",
	     xcc_sensor->time_first.tv_sec,
	     xcc_sensor->time_prev.tv_sec,
	     xcc_sensor->time_curr.tv_sec,
	     xcc_sensor->poll_time,
	     xcc_sensor->j_base,
	     xcc_sensor->j_curr,
	     xcc_sensor->j_prev,
	     xcc_sensor->j_low,
	     xcc_sensor->j_high,
	     xcc_sensor->elapsed_s_low,
	     xcc_sensor->elapsed_s_high,
	     xcc_sensor->overflows);
}

static void _reset_slurm_ipmi_conf(slurm_ipmi_conf_t *slurm_ipmi_conf)
{
	if (slurm_ipmi_conf) {
		slurm_ipmi_conf->adjustment = false;
		slurm_ipmi_conf->authentication_type = 0;
		slurm_ipmi_conf->cipher_suite_id = 0;
		slurm_ipmi_conf->disable_auto_probe = 0;
		slurm_ipmi_conf->driver_address = 0;
		xfree(slurm_ipmi_conf->driver_device);
		slurm_ipmi_conf->driver_type = NO_VAL;
		slurm_ipmi_conf->flags = XCC_FLAG_NONE;
		slurm_ipmi_conf->freq = DEFAULT_IPMI_FREQ;
		slurm_ipmi_conf->ipmi_flags = IPMI_FLAGS_DEFAULT;
		xfree(slurm_ipmi_conf->password);
		slurm_ipmi_conf->password = xstrdup(DEFAULT_IPMI_PASS);
		slurm_ipmi_conf->privilege_level = 0;
		slurm_ipmi_conf->protocol_version = 0;
		slurm_ipmi_conf->register_spacing = 0;
		slurm_ipmi_conf->retransmission_timeout = 0;
		slurm_ipmi_conf->session_timeout = 0;
		slurm_ipmi_conf->target_channel_number = 0x00;
		slurm_ipmi_conf->target_channel_number_is_set = false;
		slurm_ipmi_conf->target_slave_address = 0x20;
		slurm_ipmi_conf->target_slave_address_is_set = false;
		slurm_ipmi_conf->timeout = DEFAULT_IPMI_TIMEOUT;
		xfree(slurm_ipmi_conf->username);
		slurm_ipmi_conf->username = xstrdup(DEFAULT_IPMI_USER);
		slurm_ipmi_conf->workaround_flags = 0; // See man 8 ipmi-raw
	}
}

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

/*
 * _init_ipmi_config initializes parameters for freeipmi library
 */
static int _init_ipmi_config (void)
{
	int ret = 0;
	unsigned int workaround_flags_mask =
		(IPMI_WORKAROUND_FLAGS_INBAND_ASSUME_IO_BASE_ADDRESS
		 | IPMI_WORKAROUND_FLAGS_INBAND_SPIN_POLL);

	if (ipmi_ctx) {
		debug("ipmi_ctx already initialized\n");
		return SLURM_SUCCESS;
	}

	if (!(ipmi_ctx = ipmi_ctx_create())) {
		error("ipmi_ctx_create: %s\n", strerror(errno));
		goto cleanup;
	}

	if (getuid() != 0) {
		error ("%s: error : must be root to open ipmi devices\n",
		       __func__);
		goto cleanup;
	}

	/* XCC OEM commands always require to use in-band communication */
	if (((slurm_ipmi_conf.driver_type > 0) &&
	     (slurm_ipmi_conf.driver_type != NO_VAL) &&
	     (slurm_ipmi_conf.driver_type != IPMI_DEVICE_KCS) &&
	     (slurm_ipmi_conf.driver_type != IPMI_DEVICE_SSIF) &&
	     (slurm_ipmi_conf.driver_type != IPMI_DEVICE_OPENIPMI) &&
	     (slurm_ipmi_conf.driver_type != IPMI_DEVICE_SUNBMC))
	    || (slurm_ipmi_conf.workaround_flags & ~workaround_flags_mask)) {
		/* IPMI ERROR PARAMETERS */
		error ("%s: error: XCC Lenovo plugin only supports in-band communication, incorrect driver type or workaround flags",
		       __func__);

		debug("slurm_ipmi_conf.driver_type=%u slurm_ipmi_conf.workaround_flags=%u",
		      slurm_ipmi_conf.driver_type,
		      slurm_ipmi_conf.workaround_flags);

		goto cleanup;
	}

	if (slurm_ipmi_conf.driver_type == NO_VAL) {
		if ((ret = ipmi_ctx_find_inband(
			     ipmi_ctx,
			     NULL,
			     slurm_ipmi_conf.disable_auto_probe,
			     slurm_ipmi_conf.driver_address,
			     slurm_ipmi_conf.register_spacing,
			     slurm_ipmi_conf.driver_device,
			     slurm_ipmi_conf.workaround_flags,
			     slurm_ipmi_conf.ipmi_flags)) <= 0) {
			error("%s: error on ipmi_ctx_find_inband: %s",
			      __func__, ipmi_ctx_errormsg(ipmi_ctx));

			debug("slurm_ipmi_conf.driver_type=%u\n"
			      "slurm_ipmi_conf.disable_auto_probe=%u\n"
			      "slurm_ipmi_conf.driver_address=%u\n"
			      "slurm_ipmi_conf.register_spacing=%u\n"
			      "slurm_ipmi_conf.driver_device=%s\n"
			      "slurm_ipmi_conf.workaround_flags=%u\n"
			      "slurm_ipmi_conf.ipmi_flags=%u",
			      slurm_ipmi_conf.driver_type,
			      slurm_ipmi_conf.disable_auto_probe,
			      slurm_ipmi_conf.driver_address,
			      slurm_ipmi_conf.register_spacing,
			      slurm_ipmi_conf.driver_device,
			      slurm_ipmi_conf.workaround_flags,
			      slurm_ipmi_conf.ipmi_flags);

			goto cleanup;
		}
	} else {
		if ((ipmi_ctx_open_inband(ipmi_ctx,
					  slurm_ipmi_conf.driver_type,
					  slurm_ipmi_conf.disable_auto_probe,
					  slurm_ipmi_conf.driver_address,
					  slurm_ipmi_conf.register_spacing,
					  slurm_ipmi_conf.driver_device,
					  slurm_ipmi_conf.workaround_flags,
					  slurm_ipmi_conf.ipmi_flags) < 0)) {
			error ("%s: error on ipmi_ctx_open_inband: %s",
			       __func__, ipmi_ctx_errormsg (ipmi_ctx));

			debug("slurm_ipmi_conf.driver_type=%u\n"
			      "slurm_ipmi_conf.disable_auto_probe=%u\n"
			      "slurm_ipmi_conf.driver_address=%u\n"
			      "slurm_ipmi_conf.register_spacing=%u\n"
			      "slurm_ipmi_conf.driver_device=%s\n"
			      "slurm_ipmi_conf.workaround_flags=%u\n"
			      "slurm_ipmi_conf.ipmi_flags=%u",
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
	    || slurm_ipmi_conf.target_slave_address_is_set) {
		if (ipmi_ctx_set_target(
			    ipmi_ctx,
			    slurm_ipmi_conf.target_channel_number_is_set ?
			    &slurm_ipmi_conf.target_channel_number : NULL,
			    slurm_ipmi_conf.target_slave_address_is_set ?
			    &slurm_ipmi_conf.target_slave_address : NULL) < 0) {
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
 * _read_ipmi_values read the Power sensor and update last_update_watt and times
 */
static xcc_raw_single_data_t *_read_ipmi_values(void)
{
	xcc_raw_single_data_t *xcc_reading;
	uint8_t buf_rs[IPMI_RAW_MAX_ARGS];
	int rs_len = 0;

	if (!IPMI_NET_FN_RQ_VALID(cmd_rq[1])) {
                error("Invalid netfn value\n");
		return NULL;
	}

        rs_len = ipmi_cmd_raw(ipmi_ctx,
                              cmd_rq[0], // Lun (logical unit number)
                              cmd_rq[1], // Net Function
                              &cmd_rq[2], // Command number + request data
                              cmd_rq_len - 2, // Length (in bytes)
                              &buf_rs, // response buffer
                              IPMI_RAW_MAX_ARGS // max response length
                );

        debug3("ipmi_cmd_raw: %s\n", ipmi_ctx_errormsg(ipmi_ctx));

        if (rs_len < 0) {
		error("Invalid ipmi response length");
		return NULL;
	}

	/* Due to memory alineation we must copy the data from the buffer */
	xcc_reading = xmalloc(sizeof(xcc_raw_single_data_t));
	if (slurm_ipmi_conf.flags & XCC_FLAG_FAKE) {
		/* xcc_reading->fifo_inx = 0; */
		// Fake metric j
		/* xcc_reading->j = fake_past_read + 550 + rand() % 200; */
		/* fake_past_read = xcc_reading->j; */
		/* xcc_reading->mj = 0; */
		/* xcc_reading->s = time(NULL) - 15; //Fake metric timestamp */
		/* xcc_reading->ms = 0; */
		memcpy(&xcc_reading->fifo_inx, buf_rs+2, 2);
		memcpy(&xcc_reading->j, buf_rs+4, 4);
		memcpy(&xcc_reading->mj, buf_rs+8, 2);
		memcpy(&xcc_reading->s, buf_rs+10, 4);
		memcpy(&xcc_reading->ms, buf_rs+14, 2);
	} else {
		memcpy(&xcc_reading->fifo_inx, buf_rs+2, 2);
		memcpy(&xcc_reading->j, buf_rs+4, 4);
		memcpy(&xcc_reading->mj, buf_rs+8, 2);
		memcpy(&xcc_reading->s, buf_rs+10, 4);
		memcpy(&xcc_reading->ms, buf_rs+14, 2);
	}

	return xcc_reading;
}

inline static uint32_t _elapsed_last_interval_s()
{
        int elapsed = xcc_sensor->time_curr.tv_sec -
		xcc_sensor->time_prev.tv_sec;
	return (elapsed < 0) ? 0 : (uint32_t)elapsed;
}

inline static uint32_t _consumed_last_interval_j()
{
        int consumed = xcc_sensor->j_curr - xcc_sensor->j_prev;
	return (consumed < 0) ? 0 : (uint32_t)consumed;
}

/*
 * _curr_watts() reads the xcc_sensor data and return the consumed watts since
 *  the last reading.
 */
inline static uint32_t _curr_watts()
{
	uint32_t joules, seconds;

	seconds = _elapsed_last_interval_s();
	joules = _consumed_last_interval_j();

	if (joules == 0 || seconds == 0)
		return 0;

	return (uint32_t) round((double)joules/(double)seconds);
}

/*
 * _thread_update_node_energy calls _read_ipmi_values and updates all values
 * for node consumption
 */
static int _thread_update_node_energy(void)
{
	time_t now;
	int offset;
	uint32_t c_j, e_s;
	xcc_raw_single_data_t *xcc_raw = _read_ipmi_values();

	if (!xcc_raw) {
		error("%s could not read XCC ipmi values", __func__);
		return SLURM_FAILURE;
	}

	xcc_sensor->time_prev.tv_sec = xcc_sensor->time_curr.tv_sec;
	xcc_sensor->time_curr.tv_sec = xcc_raw->s;
	xcc_sensor->j_prev = xcc_sensor->j_curr;

	/* Detect an overflow */
	offset =  xcc_raw->j +
		(IPMI_XCC_OVERFLOW * xcc_sensor->overflows) -
		xcc_sensor->j_prev;
	if (offset < 0) {
		xcc_sensor->overflows++;
		xcc_sensor->j_curr = xcc_sensor->j_prev +
			((IPMI_XCC_OVERFLOW * xcc_sensor->overflows) -
			 xcc_sensor->j_prev) + xcc_raw->j;
	} else {
		xcc_sensor->j_curr = xcc_raw->j;
	}

	if (debug_flags & DEBUG_FLAG_ENERGY) {
		info("%s called, printing xcc info", __func__);
		_print_xcc_sensor();
	}

	/* Record the interval with highest/lowest consumption. */
	c_j = _consumed_last_interval_j();
	e_s = _elapsed_last_interval_s();

	now = time(NULL);
	/* Wait at least one cycle to set values*/
	if (((xcc_sensor->j_low == 0) || (xcc_sensor->j_low > c_j)) &&
	    ((now - xcc_sensor->poll_time) >= slurm_ipmi_conf.freq)) {
		xcc_sensor->j_low = c_j;
		xcc_sensor->elapsed_s_low = e_s;
	}
	if ((xcc_sensor->j_high < c_j) &&
	    ((now - xcc_sensor->poll_time) >= slurm_ipmi_conf.freq)) {
		xcc_sensor->j_high = c_j;
		xcc_sensor->elapsed_s_high = e_s;
	}

	/* Last metrics gathered now */
	xcc_sensor->poll_time = now;

	if (debug_flags & DEBUG_FLAG_ENERGY) {
		info("ipmi-thread: XCC current_watts: %u\n"
		     "consumed energy last interval: %u Joules\n"
		     "elapsed time last interval: %u Seconds\n"
		     "first read time unix timestamp: %ld\n"
		     "first read energy counter val: %u",
		     _curr_watts(),
		     _consumed_last_interval_j(),
		     _elapsed_last_interval_s(),
		     xcc_sensor->time_first.tv_sec,
		     xcc_sensor->j_base);
	}

	return SLURM_SUCCESS;
}

/*
 * _thread_init initializes values and conf for the ipmi thread
 */
static int _thread_init(void)
{
	static bool first = true;
	static bool first_init = SLURM_FAILURE;

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

	if (!first)
		return first_init;

	first = false;

	/* Let's fill the xcc_sensor with the first reading */
	if (!xcc_sensor) {
		xcc_raw_single_data_t *xcc_raw = _read_ipmi_values();
		if (!xcc_raw) {
			error("%s could not read XCC ipmi values", __func__);
			goto cleanup;
		}

		xcc_sensor = xmalloc(sizeof(sensor_status_t));
		xcc_sensor->poll_time = time(NULL);
		xcc_sensor->time_first.tv_sec = xcc_raw->s;
		xcc_sensor->time_prev.tv_sec = xcc_raw->s;
		xcc_sensor->time_curr.tv_sec = xcc_raw->s;
		xcc_sensor->j_base = xcc_raw->j;
		xcc_sensor->j_curr = xcc_raw->j;
		xcc_sensor->j_prev = xcc_raw->j;
		xcc_sensor->j_low = 0;
		xcc_sensor->j_high = 0;
		xfree(xcc_raw);
	}

	if (debug_flags & DEBUG_FLAG_ENERGY)
		info("%s thread init success", plugin_name);

	first_init = SLURM_SUCCESS;
	return SLURM_SUCCESS;
cleanup:
	info("%s thread init error", plugin_name);
	first_init = SLURM_FAILURE;
	ipmi_ctx_close(ipmi_ctx);
	ipmi_ctx_destroy(ipmi_ctx);

	return SLURM_FAILURE;
}

static int _ipmi_send_profile(void)
{
	uint16_t i;
	uint64_t data[XCC_LABEL_CNT];

	if (!_running_profile())
		return SLURM_SUCCESS;

	if (dataset_id < 0) {
		acct_gather_profile_dataset_t dataset[XCC_LABEL_CNT + 1];
		for (i = 0; i < XCC_LABEL_CNT; i++) {
			dataset[i].name = xcc_labels[i];
			dataset[i].type = PROFILE_FIELD_UINT64;
		}
		dataset[i].name = NULL;
		dataset[i].type = PROFILE_FIELD_NOT_SET;

		dataset_id = acct_gather_profile_g_create_dataset(
			"Energy", NO_PARENT, dataset);

		if (debug_flags & DEBUG_FLAG_ENERGY)
			debug("Energy: dataset created (id = %d)", dataset_id);
		if (dataset_id == SLURM_ERROR) {
			error("Energy: Failed to create the dataset for IPMI");
			return SLURM_ERROR;
		}
	}

	/* pack an array of uint64_t with current power of sensors */
	memset(data, 0, sizeof(data));
	data[XCC_ENERGY] = xcc_sensor->j_curr - xcc_sensor->j_base;
	data[XCC_MAX_POWER] = xcc_sensor->j_high/xcc_sensor->elapsed_s_high;
	data[XCC_MIN_POWER] = xcc_sensor->j_low/xcc_sensor->elapsed_s_low;
	data[XCC_AVG_POWER] = (xcc_sensor->j_curr - xcc_sensor->j_base) /
		(xcc_sensor->time_curr.tv_sec - xcc_sensor->time_first.tv_sec);
	data[XCC_CURR_POWER] = _curr_watts();
	if (debug_flags & DEBUG_FLAG_PROFILE)
		for (i = 0; i < XCC_LABEL_CNT; i++)
			info("PROFILE-Energy: %s=%"PRIu64,
			     xcc_labels[i], data[i]);

	return acct_gather_profile_g_add_sample_data(
		dataset_id, (void *)data,
		(time_t)xcc_sensor->time_curr.tv_sec);
}


/*
 * _thread_ipmi_run is the thread calling ipmi and launching _thread_ipmi_write
 */
static void *_thread_ipmi_run(void *no_data)
{
// need input (attr)
	struct timeval tvnow;
	struct timespec abs;

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

	slurm_cond_signal(&launch_cond);

	/* setup timer */
	gettimeofday(&tvnow, NULL);
	abs.tv_sec = tvnow.tv_sec;
	abs.tv_nsec = tvnow.tv_usec * 1000;

	//loop until slurm stop
	while (!flag_energy_accounting_shutdown) {
		slurm_mutex_lock(&ipmi_mutex);

		_thread_update_node_energy();

		/* Sleep until the next time. */
		abs.tv_sec += slurm_ipmi_conf.freq;
		slurm_cond_timedwait(&ipmi_cond, &ipmi_mutex, &abs);

		slurm_mutex_unlock(&ipmi_mutex);
	}

	if (debug_flags & DEBUG_FLAG_ENERGY)
		info("ipmi-thread: ended");

	return NULL;
}

static void *_thread_launcher(void *no_data)
{
	//what arg would countain? frequency, socket?
	struct timeval tvnow;
	struct timespec abs;

	slurm_thread_create(&thread_ipmi_id_run, _thread_ipmi_run, NULL);

	/* setup timer */
	gettimeofday(&tvnow, NULL);
	abs.tv_sec = tvnow.tv_sec + slurm_ipmi_conf.timeout;
	abs.tv_nsec = tvnow.tv_usec * 1000;

	slurm_mutex_lock(&launch_mutex);
	slurm_cond_timedwait(&launch_cond, &launch_mutex, &abs);
	slurm_mutex_unlock(&launch_mutex);

	if (!flag_thread_started) {
		error("%s threads failed to start in a timely manner",
		      plugin_name);

		flag_energy_accounting_shutdown = true;

		/*
		 * It is a known thing we can hang up on IPMI calls cancel if
		 * we must.
		 */
		pthread_cancel(thread_ipmi_id_run);

		/*
		 * Unlock just to make sure since we could have canceled the
		 * thread while in the lock.
		 */
		slurm_mutex_unlock(&ipmi_mutex);
	}

	return NULL;
}

static int _get_joules_task(uint16_t delta)
{
	acct_gather_energy_t *energy = NULL;
	uint16_t sensor_cnt = 0;
	int offset;

        /*
	 * 'delta' parameter means "use cache" if data is newer than delta
	 * seconds ago, otherwise just inquiry ipmi again.
	 */
	if (slurm_get_node_energy(NULL, delta, &sensor_cnt, &energy)) {
		error("%s: can't get info from slurmd", __func__);
		return SLURM_ERROR;
	}

	if (sensor_cnt != 1) {
		error("%s: received %u xcc sensors expected 1",
		      __func__, sensor_cnt);
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

		/*
		 * Record data for the step, so take all the previous
		 * consumption out of the mix setting the base_j and
		 * first poll time to the current value.
		 */
		xcc_sensor->poll_time = time(NULL);
		xcc_sensor->j_base = energy->consumed_energy;
		xcc_sensor->time_first.tv_sec = energy->poll_time;

		xcc_sensor->time_prev.tv_sec = energy->poll_time;
		xcc_sensor->time_curr.tv_sec = energy->poll_time;
		xcc_sensor->j_curr = xcc_sensor->j_base;
		xcc_sensor->j_prev = xcc_sensor->j_base;

		xcc_sensor->j_low = 0;
		xcc_sensor->j_high = 0;
		xcc_sensor->elapsed_s_low = 0;
		xcc_sensor->elapsed_s_high = 0;
	} else {
		uint32_t c_mj, e_ms;

	        /*
		 * Update this task's local sensor with the latest cached
		 * energy reading from the global sensor.
		 */
		xcc_sensor->poll_time = time(NULL);
		xcc_sensor->j_prev = xcc_sensor->j_curr;
		xcc_sensor->time_prev = xcc_sensor->time_curr;
		xcc_sensor->time_curr.tv_sec = energy->poll_time;

		/* Detect an overflow - slurmd may be restarted */
		offset = energy->consumed_energy +
			(IPMI_XCC_OVERFLOW * xcc_sensor->overflows) -
			xcc_sensor->j_prev;
		if (offset < 0) {
			xcc_sensor->overflows++;
			xcc_sensor->j_curr = xcc_sensor->j_prev +
				((IPMI_XCC_OVERFLOW * xcc_sensor->overflows) -
				 xcc_sensor->j_prev) + energy->consumed_energy;
		} else {
			xcc_sensor->j_curr = energy->consumed_energy;
		}

		/* Tally the interval with highest/lowest consumption */
		c_mj = _consumed_last_interval_j();
		e_ms = _elapsed_last_interval_s();

		if (xcc_sensor->j_low == 0 || xcc_sensor->j_low > c_mj) {
			xcc_sensor->j_low = c_mj;
			xcc_sensor->elapsed_s_low = e_ms;
		}
		if (xcc_sensor->j_high == 0 || xcc_sensor->j_high < c_mj) {
			xcc_sensor->j_high = c_mj;
			xcc_sensor->elapsed_s_high = e_ms;
		}
	}

	acct_gather_energy_destroy(energy);

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
		     xcc_sensor->time_first.tv_sec,
		     xcc_sensor->j_base);
	}

	return SLURM_SUCCESS;
}

/*
 * _get_node_energy() move the xcc_sensor to the energy struct.
 *
 * FIXME: In the future, the energy struct may be extended to include
 * the already calculated fields from xcc_sensor.
 */
static void _get_node_energy(acct_gather_energy_t *energy)
{
	if (!xcc_sensor || !energy)
		return;

	memset(energy, 0, sizeof(acct_gather_energy_t));

	if (xcc_sensor->j_low == 0 || xcc_sensor->elapsed_s_low == 0)
		energy->base_watts = 0;
	else
		energy->base_watts = xcc_sensor->j_low /
			xcc_sensor->elapsed_s_low;

	energy->previous_consumed_energy = xcc_sensor->j_prev;
	energy->consumed_energy = xcc_sensor->j_curr;
	energy->base_consumed_energy = xcc_sensor->j_base;
	energy->poll_time = xcc_sensor->time_curr.tv_sec;
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

	return SLURM_SUCCESS;
}

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
			*last_poll = xcc_sensor->poll_time;
		else
			*last_poll = 0;
		slurm_mutex_unlock(&ipmi_mutex);
		break;
	case ENERGY_DATA_SENSOR_CNT:
		*sensor_cnt = 1;
		break;
	case ENERGY_DATA_STRUCT:
		slurm_mutex_lock(&ipmi_mutex);
		_get_node_energy(energy);
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
		_get_node_energy(energy);
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
//	s_p_options_t *full_options_ptr;
	s_p_options_t options[] = {
		{"EnergyIPMIAuthenticationType", S_P_UINT32},
		{"EnergyIPMICalcAdjustment", S_P_BOOLEAN},
		{"EnergyIPMICipherSuiteId", S_P_UINT32},
		{"EnergyIPMIDisableAutoProbe", S_P_UINT32},
		{"EnergyIPMIDriverAddress", S_P_UINT32},
		{"EnergyIPMIDriverDevice", S_P_STRING},
		{"EnergyIPMIDriverType", S_P_UINT32},
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
		{"EnergyXCCFake", S_P_BOOLEAN},
		{NULL} };

	transfer_s_p_options(full_options, options, full_options_cnt);
}

extern void acct_gather_energy_p_conf_set(s_p_hashtbl_t *tbl)
{
	bool tmp_bool;

	/* Set initial values */
	_reset_slurm_ipmi_conf(&slurm_ipmi_conf);

	if (tbl) {
		/* ipmi initialisation parameters */
		s_p_get_uint32(&slurm_ipmi_conf.authentication_type,
			       "EnergyIPMIAuthenticationType", tbl);
		s_p_get_boolean(&(slurm_ipmi_conf.adjustment),
				"EnergyIPMICalcAdjustment", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.cipher_suite_id,
			       "EnergyIPMICipherSuiteId", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.disable_auto_probe,
			       "EnergyIPMIDisableAutoProbe", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.driver_address,
			       "EnergyIPMIDriverAddress", tbl);
		s_p_get_string(&slurm_ipmi_conf.driver_device,
			       "EnergyIPMIDriverDevice", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.driver_type,
			       "EnergyIPMIDriverType", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.driver_type,
			       "EnergyIPMIDriverType", tbl);
		s_p_get_uint32(&slurm_ipmi_conf.freq,
			       "EnergyIPMIFrequency", tbl);
		if ((int)slurm_ipmi_conf.freq <= 0)
			fatal("EnergyIPMIFrequency must be a positive integer in acct_gather.conf.");
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
		s_p_get_boolean(&tmp_bool, "EnergyXCCFake", tbl);
		if (tmp_bool) {
			slurm_ipmi_conf.flags |= XCC_FLAG_FAKE;
			/*
			 * This is just to do a random query and get error if
			 * ipmi is not initialized
			 */
			cmd_rq[0] = 0x00;
			cmd_rq[1] = 0x04;
			cmd_rq[2] = 0x2d;
			cmd_rq[3] = 0x36;
			cmd_rq_len = 4;
		}
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
	key_pair->name = xstrdup("EnergyIPMIAuthenticationType");
	key_pair->value = xstrdup_printf("%u",
					 slurm_ipmi_conf.authentication_type);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMICalcAdjustment");
	key_pair->value = xstrdup(slurm_ipmi_conf.adjustment
				  ? "Yes" : "No");
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMICipherSuiteId");
	key_pair->value = xstrdup_printf("%u", slurm_ipmi_conf.cipher_suite_id);
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
	key_pair->name = xstrdup("EnergyIPMIDriverDevice");
	key_pair->value = xstrdup(slurm_ipmi_conf.driver_device);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("EnergyIPMIDriverType");
	key_pair->value = xstrdup_printf("%u", slurm_ipmi_conf.driver_type);
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

	return;

}
