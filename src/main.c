/**
 * Main source file
 *
 * @package vzlogger
 * @copyright Copyright (c) 2011, The volkszaehler.org project
 * @license http://www.gnu.org/licenses/gpl.txt GNU Public License
 * @author Steffen Vogel <info@steffenvogel.de>
 */
/*
 * This file is part of volkzaehler.org
 *
 * volkzaehler.org is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * volkzaehler.org is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with volkszaehler.org. If not, see <http://www.gnu.org/licenses/>.
 */
 
#define VZ_VERSION "0.2"

#include <stdio.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <unistd.h>
#include <math.h>
#include <stdint.h>
#include <microhttpd.h>

#include "main.h"
#include "queue.h"
#include "api.h"
#include "local.h"

#include "protocols/obis.h"
 
static protocol_t protocols[] = {
	{"obis",	"Plaintext OBIS",	obis_get,	obis_init,	obis_close},
//	{"fluksousb", 	"FluksoUSB board", 	flukso_get,	flukso_init,	flukso_close},
//	{"onewire",	"Dallas 1-Wire sensors",onewire_get,	onewire_init, onewire_close},
	{NULL} /* stop condition for iterator */
};

static struct option long_options[] = {
	{"config", 	required_argument,	0,	'c'},
	{"daemon", 	required_argument,	0,	'd'},
	{"interval", 	required_argument,	0,	'i'},
	{"local", 	no_argument,		0,	'l'},
	{"local-port",	required_argument,	0,	'p'},
	{"help",	no_argument,		0,	'h'},
	{"verbose",	optional_argument,	0,	'v'},
	{NULL} /* stop condition for iterator */
};

static char * long_options_descs[] = {
	"config file with channel -> uuid mapping",
	"run as daemon",
	"interval in seconds to read meters",
	"activate local interface (tiny webserver)",
	"TCP port for local interface"	
	"show this help",
	"enable verbose output",
	NULL /* stop condition for iterator */
};

/* globals */
options_t opts;
channel_t * chans; /* mem gets allocated in parse_channels(), and freed in main() */

/**
 * Print availble options and some other usefull information
 */
void usage(char * argv[]) {
	char ** desc = long_options_descs;
	struct option * op = long_options;
	protocol_t * prot = protocols;

	printf("Usage: %s [options]\n\n", argv[0]);
	printf("  following options are available:\n");
	
	while (op->name && desc) {
		printf("\t--%-12s\t-%c\t%s\n", op->name, op->val, *desc);
		op++;
		desc++;
	}
	
	printf("\n");
	printf("  following protocol types are supported:\n");
	
	while (prot->name) {
		printf("\t%-12s\t%s\n", prot->name, prot->desc);
		prot++;
	}
	
	printf("\nvzlogger - volkszaehler.org logging utility %s\n", VZ_VERSION);
	printf("by Steffen Vogel <stv0g@0l.de>\n");
}

/**
 * Wrapper to log notices and errors
 */
void print(int level, char * format, channel_t *ch, ... ) {
	static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	va_list args;
	
	struct timeval now;
	struct tm * timeinfo;
	char buffer[16];
	
	if (level <= (signed int) opts.verbose) {
		gettimeofday(&now, NULL);
		timeinfo = localtime(&now.tv_sec);

		strftime(buffer, 16, "%b %d %H:%M:%S", timeinfo);

		pthread_mutex_lock(&mutex);
			fprintf((level > 0) ? stdout : stderr, "[%s.%3lu]", buffer, now.tv_usec / 1000);

			if (ch != NULL) {
				fprintf((level > 0) ? stdout : stderr, "[ch#%i]\t", ch->id);
			}
			else {
				fprintf((level > 0) ? stdout : stderr, "\t\t");
			}
			
			va_start(args, ch);
			vfprintf((level > 0) ? stdout : stderr, format, args);
			va_end(args);
			fprintf((level > 0) ? stdout : stderr, "\n");
		pthread_mutex_unlock(&mutex);
	}
}

/**
 * Parse options from command line
 */
options_t parse_options(int argc, char * argv[]) {
	options_t opts;
	
	/* setting default options */
	opts.daemon = FALSE;
	opts.local = FALSE;
	opts.verbose = 0;
	opts.port = 8080;
	opts.config = "vzlogger.conf";

	while (TRUE) {
		/* getopt_long stores the option index here. */
		int option_index = 0;

		int c = getopt_long(argc, argv, "i:c:p:lhdv:", long_options, &option_index);

		/* detect the end of the options. */
		if (c == -1)
			break;

		switch (c) {
			case 'v':
				opts.verbose = (optarg == NULL) ? 1 : atoi(optarg);
				break;
				
			case 'l':
				opts.local = TRUE;
				break;
				
			case 'd':
				opts.daemon = TRUE;
				break;
				
			case 'p': /* port for local interface */
				opts.port = atoi(optarg);
				break;

			case 'c': /* read config file */
				opts.config = (char *) malloc(strlen(optarg)+1);
				strcpy(opts.config, optarg);
				break;

			case 'h':
			case '?':
				usage(argv);
				exit((c == '?') ? EXIT_FAILURE : EXIT_SUCCESS);
		}
	}
	
	return opts;
}

channel_t * parse_channels(char * filename, int * num_chans) {
	FILE *file = fopen(filename, "r"); /* open configuration */

	if (file == NULL) {
		perror(filename); /* why didn't the file open? */
		exit(EXIT_FAILURE);
	}
	
	char line[256];
	int j = 0;
	
	channel_t *chans = malloc(sizeof(channel_t) * MAX_CHANNELS);
	
	while (fgets(line, sizeof line, file) != NULL) { /* read a line */
		if (line[0] == '#' || line[0] == '\n') continue; /* skip comments */

		channel_t ch;
		protocol_t *prot;
		char *tok = strtok(line, ";");
			
		for (int i = 0; i < 7 && tok != NULL; i++) {
			switch(i) {
				case 0: /* middleware */
					ch.middleware = (char *) malloc(strlen(tok)+1);
					strcpy(ch.middleware, tok);
					break;
				
				case 1: /* uuid */
					ch.uuid = (char *) malloc(strlen(tok)+1);
					strcpy(ch.uuid, tok);
					break;
			
				case 2: /* protocol */
					prot = protocols; /* reset pointer */
					while (prot->name && strcmp(prot->name, tok) != 0) prot++; /* linear search */
					ch.prot = prot;
					break;
			
				case 3: /* interval */
					ch.interval = atoi(tok);
					break;
			
				case 4: /* options */
					ch.options = (char *) malloc(strlen(tok)+1);
					strcpy(ch.options, tok);
					break;
			}
	
			tok = strtok(NULL, ";");
		}
		
		ch.id = j;

		print(1, "Parsed (on %s)", &ch, ch.middleware);
		chans[j++] = ch;
	}
	
	fclose(file);
	*num_chans = j;
	
	return chans;
}

/**
 * Logging thread
 *
 * Logs buffered readings against middleware
 */
void *log_thread(void *arg) {
	channel_t *ch = (channel_t *) arg; /* casting argument */
	reading_t rd;
	
	CURLcode rc;
	
	print(1, "Started logging thread", ch);
	
	while (TRUE) {
		pthread_mutex_lock(&ch->mutex);
		while (queue_is_empty(&ch->queue)) { /* detect spurious wakeups */
			pthread_cond_wait(&ch->condition, &ch->mutex); /* wait for new data */
		}
		pthread_mutex_unlock(&ch->mutex);
		
		while (!queue_is_empty(&ch->queue)) {
			pthread_mutex_lock(&ch->mutex);
			queue_first(&ch->queue, &rd);
			pthread_mutex_unlock(&ch->mutex);
			
			rc = api_log(ch, rd); /* log reading */
	
			if (rc == CURLE_OK) {
				pthread_mutex_lock(&ch->mutex);
				queue_deque(&ch->queue, &rd); /* remove reading from buffer */
				pthread_mutex_unlock(&ch->mutex);
			}
			else {
				print(1, "Delaying next transmission for 15 minutes due to pervious error", ch);
				
				sleep(RETRY_PAUSE);
			}
		}
		pthread_testcancel(); /* test for cancelation request */
	}
	
	return NULL;
}

/**
 * Read thread
 * 
 * Aquires reading from meters/sensors
 */
void *read_thread(void *arg) {
	channel_t *ch = (channel_t *) arg; /* casting argument */
	
	print(1, "Started reading thread", ch);

	/* initalize channel */
	ch->handle = ch->prot->init_func(ch->options); /* init sensor/meter */
	
	while (TRUE) {
		reading_t rd = ch->prot->read_func(ch->handle); /* aquire reading */
		
		pthread_mutex_lock(&ch->mutex);
			if (opts.verbose > 4) queue_print(&ch->queue); /* Debugging */
		
			queue_enque(&ch->queue, rd);
			pthread_cond_broadcast(&ch->condition);
		pthread_mutex_unlock(&ch->mutex);
		
		print(1, "Value read: %.3f (next reading in %i secs)", ch, rd.value, ch->interval);
		
		pthread_testcancel(); /* test for cancelation request */
		sleep(ch->interval); /* else sleep and restart aquisition */
	}
	
	/* close channel */
	ch->prot->close_func(ch->handle);
	
	return NULL;
}

/**
 * The main loop
 */
int main(int argc, char * argv[]) {
	int num_chans = 0;
	struct MHD_Daemon * d;
	
	opts = parse_options(argc, argv); /* parse command line arguments */
	chans = parse_channels(opts.config, &num_chans); /* parse channels from configuration */
	
	print(1, "Started %s with verbosity level %i", NULL, argv[0], opts.verbose);
	
	curl_global_init(CURL_GLOBAL_ALL); /* global intialization for all threads */
	
	/* start threads */
	for (int i = 0; i < num_chans; i++) {
		channel_t * ch = &chans[i];
		
		queue_init(&ch->queue, (BUFFER_LENGTH / ch->interval) + 1); /* initialize queue to buffer 10 minutes of data */
	
		pthread_mutex_init(&ch->mutex, NULL);
		pthread_cond_init(&ch->condition, NULL);

		pthread_create(&ch->reading_thread, NULL, read_thread, (void *) ch);
		pthread_create(&ch->logging_thread, NULL, log_thread, (void *) ch);
	}
	
	if (opts.local) { /* start webserver */
		d = MHD_start_daemon(
			MHD_USE_THREAD_PER_CONNECTION,
			opts.port,
			NULL, NULL,
			handle_request,
			&num_chans,
			MHD_OPTION_END
		);
	}
	
	/* wait for all threads to terminate */
	for (int i = 0; i < num_chans; i++) {
		channel_t * ch = &chans[i];
		
		pthread_join(ch->reading_thread, NULL);
		pthread_join(ch->logging_thread, NULL);
		
		pthread_cond_destroy(&ch->condition);
		pthread_mutex_destroy(&ch->mutex);
	}
	
	if (opts.local) { /* stop webserver */
		MHD_stop_daemon(d);
	}
	
	free(chans);

	return 0;
}
