/*
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
    �2013 Semtech-Cycleo

Description:
	Configure Lora concentrator and record received packets in a log file
*/


/* -------------------------------------------------------------------------- */
/* --- DEPENDANCIES --------------------------------------------------------- */

/* fix an issue between POSIX and C99 */
#if __STDC_VERSION__ >= 199901L
	#define _XOPEN_SOURCE 600
#else
	#define _XOPEN_SOURCE 500
#endif

#include <stdint.h>		/* C99 types */
#include <stdbool.h>	/* bool type */
#include <stdio.h>		/* printf fprintf sprintf fopen fputs */

#include <string.h>		/* memset */
#include <signal.h>		/* sigaction */
#include <time.h>		/* time clock_gettime strftime gmtime clock_nanosleep*/
#include <unistd.h>		/* getopt access */
#include <stdlib.h>		/* atoi */

#include "parson.h"
#include "loragw_hal.h"

/* -------------------------------------------------------------------------- */
/* --- PRIVATE MACROS ------------------------------------------------------- */

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))
#define MSG(args...)	fprintf(stderr,"loragw_pkt_logger: " args) /* message that is destined to the user */

/* -------------------------------------------------------------------------- */
/* --- PRIVATE VARIABLES (GLOBAL) ------------------------------------------- */

/* signal handling variables */
struct sigaction sigact; /* SIGQUIT&SIGINT&SIGTERM signal handling */
static int exit_sig = 0; /* 1 -> application terminates cleanly (shut down hardware, close open files, etc) */
static int quit_sig = 0; /* 1 -> application terminates without shutting down the hardware */

/* configuration variables needed by the application  */
uint64_t lgwm = 0; /* Lora gateway MAC address */
char lgwm_str[17];

/* clock and log file management */
time_t now_time;
time_t log_start_time;
FILE * log_file = NULL;
char log_file_name[64];

/* -------------------------------------------------------------------------- */
/* --- PRIVATE FUNCTIONS DECLARATION ---------------------------------------- */

static void sig_handler(int sigio);

int parse_SX1301_configuration(const char * conf_file);

int parse_gateway_configuration(const char * conf_file);

void open_log(void);

void usage (void);

/* -------------------------------------------------------------------------- */
/* --- PRIVATE FUNCTIONS DEFINITION ----------------------------------------- */

static void sig_handler(int sigio) {
	if (sigio == SIGQUIT) {
		quit_sig = 1;;
	} else if ((sigio == SIGINT) || (sigio == SIGTERM)) {
		exit_sig = 1;
	}
}

int parse_SX1301_configuration(const char * conf_file) {
	int i;
	const char conf_obj[] = "SX1301_conf";
	char param_name[32]; /* used to generate variable parameter names */
	struct lgw_conf_rxrf_s rfconf;
	struct lgw_conf_rxif_s ifconf;
	JSON_Value *root_val;
	JSON_Object *root = NULL;
	JSON_Object *conf = NULL;
	JSON_Value *val;
	uint32_t sf, bw;
	
	/* try to parse JSON */
	root_val = json_parse_file(conf_file);
	root = json_value_get_object(root_val);
	if (root == NULL) {
		MSG("ERROR: %s id not a valid JSON file\n", conf_file);
		exit(EXIT_FAILURE);
	}
	conf = json_object_get_object(root, conf_obj);
	if (conf == NULL) {
		MSG("INFO: %s does not contain a JSON object named %s\n", conf_file, conf_obj);
		return -1;
	} else {
		MSG("INFO: %s does contain a JSON object named %s, parsing SX1301 parameters\n", conf_file, conf_obj);
	}
	
	/* set configuration for RF chains */
	for (i = 0; i < LGW_RF_CHAIN_NB; ++i) {
		memset(&rfconf, 0, sizeof(rfconf)); /* initialize configuration structure */
		sprintf(param_name, "radio_%i", i); /* compose parameter path inside JSON structure */
		val = json_object_get_value(conf, param_name); /* fetch value (if possible) */
		if (json_value_get_type(val) != JSONObject) {
			MSG("INFO: no configuration for radio %i\n", i);
			continue;
		}
		/* there is an object to configure that radio, let's parse it */
		sprintf(param_name, "radio_%i.enable", i);
		val = json_object_dotget_value(conf, param_name);
		if (json_value_get_type(val) == JSONBoolean) {
			rfconf.enable = (bool)json_value_get_boolean(val);
		} else {
			rfconf.enable = false;
		}
		if (rfconf.enable == false) { /* radio disabled, nothing else to parse */
			MSG("INFO: radio %i disabled\n", i);
		} else  { /* radio enabled, will parse the other parameters */
			sprintf(param_name, "radio_%i.freq", i);
			rfconf.freq_hz = (uint32_t)json_object_dotget_number(conf, param_name);
			MSG("INFO: radio %i enabled, center frequency %u\n", i, rfconf.freq_hz);
		}
		/* all parameters parsed, submitting configuration to the HAL */
		if (lgw_rxrf_setconf(i, rfconf) != LGW_HAL_SUCCESS) {
			MSG("WARNING: invalid configuration for radio %i\n", i);
		}
	}
	
	/* set configuration for Lora multi-SF channels (bandwidth cannot be set) */
	for (i = 0; i < LGW_MULTI_NB; ++i) {
		memset(&ifconf, 0, sizeof(ifconf)); /* initialize configuration structure */
		sprintf(param_name, "chan_multiSF_%i", i); /* compose parameter path inside JSON structure */
		val = json_object_get_value(conf, param_name); /* fetch value (if possible) */
		if (json_value_get_type(val) != JSONObject) {
			MSG("INFO: no configuration for Lora multi-SF channel %i\n", i);
			continue;
		}
		/* there is an object to configure that Lora multi-SF channel, let's parse it */
		sprintf(param_name, "chan_multiSF_%i.enable", i);
		val = json_object_dotget_value(conf, param_name);
		if (json_value_get_type(val) == JSONBoolean) {
			ifconf.enable = (bool)json_value_get_boolean(val);
		} else {
			ifconf.enable = false;
		}
		if (ifconf.enable == false) { /* Lora multi-SF channel disabled, nothing else to parse */
			MSG("INFO: Lora multi-SF channel %i disabled\n", i);
		} else  { /* Lora multi-SF channel enabled, will parse the other parameters */
			sprintf(param_name, "chan_multiSF_%i.radio", i);
			ifconf.rf_chain = (uint32_t)json_object_dotget_number(conf, param_name);
			sprintf(param_name, "chan_multiSF_%i.if", i);
			ifconf.freq_hz = (int32_t)json_object_dotget_number(conf, param_name);
			// TODO: handle individual SF enabling and disabling (spread_factor)
			MSG("INFO: Lora multi-SF channel %i enabled, radio %i selected, IF %i Hz, 125 kHz bandwidth, SF 7 to 12\n", i, ifconf.rf_chain, ifconf.freq_hz);
		}
		/* all parameters parsed, submitting configuration to the HAL */
		if (lgw_rxif_setconf(i, ifconf) != LGW_HAL_SUCCESS) {
			MSG("WARNING: invalid configuration for Lora multi-SF channel %i\n", i);
		}
	}
	
	/* set configuration for Lora standard channel */
	memset(&ifconf, 0, sizeof(ifconf)); /* initialize configuration structure */
	val = json_object_get_value(conf, "chan_Lora_std"); /* fetch value (if possible) */
	if (json_value_get_type(val) != JSONObject) {
		MSG("INFO: no configuration for Lora standard channel\n");
	} else {
		val = json_object_dotget_value(conf, "chan_Lora_std.enable");
		if (json_value_get_type(val) == JSONBoolean) {
			ifconf.enable = (bool)json_value_get_boolean(val);
		} else {
			ifconf.enable = false;
		}
		if (ifconf.enable == false) {
			MSG("INFO: Lora standard channel %i disabled\n", i);
		} else  {
			ifconf.rf_chain = (uint32_t)json_object_dotget_number(conf, "chan_Lora_std.radio");
			ifconf.freq_hz = (int32_t)json_object_dotget_number(conf, "chan_Lora_std.if");
			bw = (uint32_t)json_object_dotget_number(conf, "chan_Lora_std.bandwidth");
			switch(bw) {
				case 500000: ifconf.bandwidth = BW_500KHZ; break;
				case 250000: ifconf.bandwidth = BW_250KHZ; break;
				case 125000: ifconf.bandwidth = BW_125KHZ; break;
				default: ifconf.bandwidth = BW_UNDEFINED;
			}
			sf = (uint32_t)json_object_dotget_number(conf, "chan_Lora_std.spread_factor");
			switch(sf) {
				case  7: ifconf.datarate = DR_LORA_SF7;  break;
				case  8: ifconf.datarate = DR_LORA_SF8;  break;
				case  9: ifconf.datarate = DR_LORA_SF9;  break;
				case 10: ifconf.datarate = DR_LORA_SF10; break;
				case 11: ifconf.datarate = DR_LORA_SF11; break;
				case 12: ifconf.datarate = DR_LORA_SF12; break;
				default: ifconf.datarate = DR_UNDEFINED;
			}
			MSG("INFO: Lora standard channel enabled, radio %i selected, IF %i Hz, %u Hz bandwidth, SF %u\n", ifconf.rf_chain, ifconf.freq_hz, bw, sf);
		}
		if (lgw_rxif_setconf(8, ifconf) != LGW_HAL_SUCCESS) {
			MSG("WARNING: invalid configuration for Lora standard channel\n");
		}
	}
	
	/* set configuration for FSK channel */
	memset(&ifconf, 0, sizeof(ifconf)); /* initialize configuration structure */
	val = json_object_get_value(conf, "chan_FSK"); /* fetch value (if possible) */
	if (json_value_get_type(val) != JSONObject) {
		MSG("INFO: no configuration for FSK channel\n");
	} else {
		val = json_object_dotget_value(conf, "chan_FSK.enable");
		if (json_value_get_type(val) == JSONBoolean) {
			ifconf.enable = (bool)json_value_get_boolean(val);
		} else {
			ifconf.enable = false;
		}
		if (ifconf.enable == false) {
			MSG("INFO: FSK channel %i disabled\n", i);
		} else  {
			ifconf.rf_chain = (uint32_t)json_object_dotget_number(conf, "chan_FSK.radio");
			ifconf.freq_hz = (int32_t)json_object_dotget_number(conf, "chan_FSK.if");
			bw = (uint32_t)json_object_dotget_number(conf, "chan_FSK.bandwidth");
			switch(bw) {
				case 500000: ifconf.bandwidth = BW_500KHZ; break;
				case 250000: ifconf.bandwidth = BW_250KHZ; break;
				case 125000: ifconf.bandwidth = BW_125KHZ; break;
				case  62500: ifconf.bandwidth = BW_62K5HZ; break;
				case  31200: ifconf.bandwidth = BW_31K2HZ; break;
				case  15600: ifconf.bandwidth = BW_15K6HZ; break;
				case   7800: ifconf.bandwidth = BW_7K8HZ;  break;
				default: ifconf.bandwidth = BW_UNDEFINED;
			}
			ifconf.datarate = (uint32_t)json_object_dotget_number(conf, "chan_FSK.datarate");
			MSG("INFO: FSK channel enabled, radio %i selected, IF %i Hz, %u Hz bandwidth, %u bps datarate\n", ifconf.rf_chain, ifconf.freq_hz, bw, ifconf.datarate);
		}
		if (lgw_rxif_setconf(9, ifconf) != LGW_HAL_SUCCESS) {
			MSG("WARNING: invalid configuration for FSK channel\n");
		}
	}
	json_value_free(root_val);
	return 0;
}

int parse_gateway_configuration(const char * conf_file) {
	const char conf_obj[] = "gateway_conf";
	JSON_Value *root_val;
	JSON_Object *root = NULL;
	JSON_Object *conf = NULL;
	unsigned long long ull = 0;
	
	/* try to parse JSON */
	root_val = json_parse_file(conf_file);
	root = json_value_get_object(root_val);
	if (root == NULL) {
		MSG("ERROR: %s id not a valid JSON file\n", conf_file);
		exit(EXIT_FAILURE);
	}
	conf = json_object_get_object(root, conf_obj);
	if (conf == NULL) {
		MSG("INFO: %s does not contain a JSON object named %s\n", conf_file, conf_obj);
		return -1;
	} else {
		MSG("INFO: %s does contain a JSON object named %s, parsing gateway parameters\n", conf_file, conf_obj);
	}
	
	/* getting network parameters (only those necessary for the packet logger) */
	sscanf(json_object_dotget_string(conf, "gateway_ID"), "%llx", &ull);
	lgwm = ull;
	MSG("INFO: gateway MAC address is configured to %016llX\n", ull);
	
	json_value_free(root_val);
	return 0;
}

void open_log(void) {
	int i;
	char iso_date[20];
	
	strftime(iso_date,ARRAY_SIZE(iso_date),"%Y%m%dT%H%M%SZ",gmtime(&now_time)); /* format yyyymmddThhmmssZ */
	log_start_time = now_time; /* keep track of when the log was started, for log rotation */
	
	sprintf(log_file_name, "pktlog_%s_%s.csv", lgwm_str, iso_date);
	log_file = fopen(log_file_name, "a"); /* create log file, append if file already exist */
	if (log_file == NULL) {
		MSG("ERROR: impossible to create log file %s\n", log_file_name);
		exit(EXIT_FAILURE);
	}
	
	i = fprintf(log_file, "\"gateway ID\",\"node MAC\",\"UTC timestamp\",\"us count\",\"frequency\",\"RF chain\",\"RX chain\",\"status\",\"size\",\"modulation\",\"bandwidth\",\"datarate\",\"coderate\",\"RSSI\",\"SNR\",\"payload\"\n");
	if (i < 0) {
		MSG("ERROR: impossible to write to log file %s\n", log_file_name);
		exit(EXIT_FAILURE);
	}
	
	MSG("INFO: Now writing to log file %s\n", log_file_name);
	return;
}

/* describe command line options */
void usage(void) {
	MSG( "Available options:\n");
	MSG( " -h print this help\n");
	MSG( " -r <int> rotate log file every N seconds (-1 disable log rotation)\n");
}

/* -------------------------------------------------------------------------- */
/* --- MAIN FUNCTION -------------------------------------------------------- */

int main(int argc, char **argv)
{
	int i, j; /* loop and temporary variables */
	struct timespec sleep_time = {0, 3000000}; /* 3 ms */
	
	/* clock and log rotation management */
	int log_rotate_interval = 3600; /* by default, rotation every hour */
	int time_check = 0; /* variable used to limit the number of calls to time() function */
	unsigned long pkt_in_log = 0; /* count the number of packet written in each log file */
	
	/* configuration file related */
	const char global_conf_fname[] = "global_conf.json"; /* contain global (typ. network-wide) configuration */
	const char local_conf_fname[] = "local_conf.json"; /* contain node specific configuration, overwrite global parameters for parameters that are defined in both */
	const char debug_conf_fname[] = "debug_conf.json"; /* if present, all other configuration files are ignored */
	
	/* allocate memory for packet fetching and processing */
	struct lgw_pkt_rx_s rxpkt[16]; /* array containing up to 16 inbound packets metadata */
	struct lgw_pkt_rx_s *p; /* pointer on a RX packet */
	int nb_pkt;
	
	/* local timestamp variables until we get accurate GPS time */
	struct timespec fetch_time;
	char fetch_timestamp[30];
	struct tm * x;
	
	/* parse command line options */
	while ((i = getopt (argc, argv, "hr:")) != -1) {
		switch (i) {
			case 'h':
				usage();
				return EXIT_FAILURE;
				break;
			
			case 'r':
				log_rotate_interval = atoi(optarg);
				if ((log_rotate_interval == 0) || (log_rotate_interval < -1)) {
					MSG( "ERROR: Invalid argument for -r option\n");
					return EXIT_FAILURE;
				}
				break;
			
			default:
				MSG("ERROR: argument parsing use -h option for help\n");
				usage();
				return EXIT_FAILURE;
		}
	}
	
	/* configure signal handling */
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigact.sa_handler = sig_handler;
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	
	/* configuration files management */
	if (access(debug_conf_fname, R_OK) == 0) {
	/* if there is a debug conf, parse only the debug conf */
		MSG("INFO: found debug configuration file %s, other configuration files will be ignored\n", debug_conf_fname);
		parse_SX1301_configuration(debug_conf_fname);
		parse_gateway_configuration(debug_conf_fname);
	} else if (access(global_conf_fname, R_OK) == 0) {
	/* if there is a global conf, parse it and then try to parse local conf  */
		MSG("INFO: found global configuration file %s, trying to parse it\n", global_conf_fname);
		parse_SX1301_configuration(global_conf_fname);
		parse_gateway_configuration(global_conf_fname);
		if (access(local_conf_fname, R_OK) == 0) {
			MSG("INFO: found local configuration file %s, trying to parse it\n", local_conf_fname);
			parse_SX1301_configuration(local_conf_fname);
			parse_gateway_configuration(local_conf_fname);
		}
	} else if (access(local_conf_fname, R_OK) == 0) {
	/* if there is only a local conf, parse it and that's all */
		MSG("INFO: found local configuration file %s, trying to parse it\n", local_conf_fname);
		parse_SX1301_configuration(local_conf_fname);
		parse_gateway_configuration(local_conf_fname);
	} else {
		MSG("ERROR: failed to find any configuration file named %s, %s or %s\n", global_conf_fname, local_conf_fname, debug_conf_fname);
		return EXIT_FAILURE;
	}
	
	/* starting the concentrator */
	i = lgw_start();
	if (i == LGW_HAL_SUCCESS) {
		MSG("INFO: concentrator started, packet can now be received\n");
	} else {
		MSG("ERROR: failed to start the concentrator\n");
		return EXIT_FAILURE;
	}
	
	/* transform the MAC address into a string */
	sprintf(lgwm_str, "%016llX", lgwm);
	
	/* opening log file and writing CSV header*/
	time(&now_time);
	open_log();
	
	/* main loop */
	while ((quit_sig != 1) && (exit_sig != 1)) {
		/* fetch packets */
		nb_pkt = lgw_receive(ARRAY_SIZE(rxpkt), rxpkt);
		if (nb_pkt == LGW_HAL_ERROR) {
			MSG("ERROR: failed packet fetch, exiting\n");
			return EXIT_FAILURE;
		} else if (nb_pkt == 0) {
			clock_nanosleep(CLOCK_MONOTONIC, 0, &sleep_time, NULL); /* wait a short time if no packets */
		} else {
			/* local timestamp generation until we get accurate GPS time */
			clock_gettime(CLOCK_REALTIME, &fetch_time);
			x = gmtime(&(fetch_time.tv_sec));
			sprintf(fetch_timestamp,"%04i-%02i-%02i %02i:%02i:%02i.%03liZ",(x->tm_year)+1900,(x->tm_mon)+1,x->tm_mday,x->tm_hour,x->tm_min,x->tm_sec,(fetch_time.tv_nsec)/1000000); /* ISO 8601 format */
		}
		
		/* log packets */
		for (i=0; i < nb_pkt; ++i) {
			p = &rxpkt[i];
			
			/* writing gateway ID */
			fprintf(log_file, "%016llX,", lgwm);
			
			/* writing node MAC address */
			fputs("\"\",", log_file); // TODO: need to parse payload
			
			/* writing UTC timestamp*/
			fprintf(log_file, "\"%s\",", fetch_timestamp);
			// TODO: repalce with GPS time when available
			
			/* writing internal clock */
			fprintf(log_file, "%010u,", p->count_us);
			
			/* writing RX frequency */
			fputs("\"\",", log_file); // TODO: need updated HAL
			
			/* writing RF chain */
			fputs("\"\",", log_file); // TODO: need updated HAL
			
			/* writing RX modem/IF chain */
			fprintf(log_file, "%2d,", p->if_chain);
			
			/* writing status */
			switch(p->status) {
				case STAT_CRC_OK:	fputs("\"CRC_OK\" ,", log_file); break;
				case STAT_CRC_BAD:	fputs("\"CRC_BAD\",", log_file); break;
				case STAT_NO_CRC:	fputs("\"NO_CRC\" ,", log_file); break;
				case STAT_UNDEFINED:fputs("\"UNDEF\"  ,", log_file); break;
				default: fputs("\"ERR\"    ,", log_file);
			}
			
			/* writing payload size */
			fprintf(log_file, "%3u,", p->size);
			
			/* writing modulation */
			switch(p->modulation) {
				case MOD_LORA:	fputs("\"LORA\",", log_file); break;
				case MOD_FSK:	fputs("\"FSK\" ,", log_file); break;
				default: fputs("\"ERR\" ,", log_file);
			}
			
			/* writing bandwidth */
			switch(p->bandwidth) {
				case BW_500KHZ:	fputs("500000,", log_file); break;
				case BW_250KHZ:	fputs("250000,", log_file); break;
				case BW_125KHZ:	fputs("125000,", log_file); break;
				case BW_62K5HZ:	fputs("62500 ,", log_file); break;
				case BW_31K2HZ:	fputs("31200 ,", log_file); break;
				case BW_15K6HZ:	fputs("15600 ,", log_file); break;
				case BW_7K8HZ:	fputs("7800  ,", log_file); break;
				case BW_UNDEFINED: fputs("0     ,", log_file); break;
				default: fputs("-1    ,", log_file);
			}
			
			/* writing datarate */
			if (p->modulation == MOD_LORA) {
				switch (p->datarate) {
					case DR_LORA_SF7:	fputs("\"SF7\"   ,", log_file); break;
					case DR_LORA_SF8:	fputs("\"SF8\"   ,", log_file); break;
					case DR_LORA_SF9:	fputs("\"SF9\"   ,", log_file); break;
					case DR_LORA_SF10:	fputs("\"SF10\"  ,", log_file); break;
					case DR_LORA_SF11:	fputs("\"SF11\"  ,", log_file); break;
					case DR_LORA_SF12:	fputs("\"SF12\"  ,", log_file); break;
					default: fputs("\"ERR\"   ,", log_file);
				}
			} else if (p->modulation == MOD_FSK) {
				fprintf(log_file, "\"%6u\",", p->datarate);
			} else {
				fputs("\"ERR\"   ,", log_file);
			}
			
			/* writing coderate */
			switch (p->coderate) {
				case CR_LORA_4_5:	fputs("\"4/5\",", log_file); break;
				case CR_LORA_4_6:	fputs("\"2/3\",", log_file); break;
				case CR_LORA_4_7:	fputs("\"4/7\",", log_file); break;
				case CR_LORA_4_8:	fputs("\"1/2\",", log_file); break;
				case CR_UNDEFINED:	fputs("\"\"   ,", log_file); break;
				default: fputs("\"ERR\",", log_file);
			}
			
			/* writing packet RSSI */
			fprintf(log_file, "%+.0f,", p->rssi);
			
			/* writing packet average SNR */
			fprintf(log_file, "%+5.1f,", p->snr);
			
			/* writing hex-encoded payload (bundled in 32-bit words) */
			fputs("\"", log_file);
			for (j = 0; j < p->size; ++j) {
				if ((j > 0) && (j%4 == 0)) fputs("-", log_file);
				fprintf(log_file, "%02X", p->payload[j]);
			}
			fputs("\"\n", log_file);
			++pkt_in_log;
		}
		
		/* check time and rotate log file if necessary */
		++time_check;
		if (time_check >= 8) {
			time_check = 0;
			time(&now_time);
			if (difftime(now_time, log_start_time) > log_rotate_interval) {
				fclose(log_file);
				MSG("INFO: log file %s closed, %lu packet(s) recorded\n", log_file_name, pkt_in_log);
				pkt_in_log = 0;
				open_log();
			}
		}
	}
	
	if (exit_sig == 1) {
		/* clean up before leaving */
		i = lgw_stop();
		if (i == LGW_HAL_SUCCESS) {
			MSG("INFO: concentrator stopped successfully\n");
		} else {
			MSG("WARNING: failed to stop concentrator successfully\n");
		}
		fclose(log_file);
		MSG("INFO: log file %s closed, %lu packet(s) recorded\n", log_file_name, pkt_in_log);
	}
	
	MSG("INFO: Exiting packet logger program\n");
	return EXIT_SUCCESS;
}

/* --- EOF ------------------------------------------------------------------ */
