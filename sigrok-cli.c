/*
 * This file is part of the sigrok project.
 *
 * Copyright (C) 2012 Bert Vermeulen <bert@biot.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sigrokdecode.h> /* First, so we avoid a _POSIX_C_SOURCE warning. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <libsigrok/libsigrok.h>
#include "sigrok-cli.h"
#include "config.h"

#define DEFAULT_OUTPUT_FORMAT "bits:width=64"

static struct sr_context *sr_ctx = NULL;

static uint64_t limit_samples = 0;
static uint64_t limit_frames = 0;
static struct sr_output_format *output_format = NULL;
static int default_output_format = FALSE;
static char *output_format_param = NULL;
static GHashTable *pd_ann_visible = NULL;
static struct sr_datastore *singleds = NULL;

static gboolean opt_version = FALSE;
static gint opt_loglevel = SR_LOG_WARN; /* Show errors+warnings per default. */
static gboolean opt_list_devs = FALSE;
static gboolean opt_wait_trigger = FALSE;
static gchar *opt_input_file = NULL;
static gchar *opt_output_file = NULL;
static gchar *opt_drv = NULL;
static gchar *opt_dev = NULL;
static gchar *opt_probes = NULL;
static gchar *opt_triggers = NULL;
static gchar *opt_pds = NULL;
static gchar *opt_pd_stack = NULL;
static gchar *opt_pd_annotations = NULL;
static gchar *opt_input_format = NULL;
static gchar *opt_output_format = NULL;
static gchar *opt_show = NULL;
static gchar *opt_time = NULL;
static gchar *opt_samples = NULL;
static gchar *opt_frames = NULL;
static gchar *opt_continuous = NULL;

static GOptionEntry optargs[] = {
	{"version", 'V', 0, G_OPTION_ARG_NONE, &opt_version,
			"Show version and support list", NULL},
	{"loglevel", 'l', 0, G_OPTION_ARG_INT, &opt_loglevel,
			"Set libsigrok/libsigrokdecode loglevel", NULL},
	{"list-devices", 'D', 0, G_OPTION_ARG_NONE, &opt_list_devs,
			"Scan for devices", NULL},
	{"driver", 0, 0, G_OPTION_ARG_STRING, &opt_drv,
			"Use only this driver", NULL},
	{"device", 'd', 0, G_OPTION_ARG_STRING, &opt_dev,
			"Use specified device", NULL},
	{"input-file", 'i', 0, G_OPTION_ARG_FILENAME, &opt_input_file,
			"Load input from file", NULL},
	{"input-format", 'I', 0, G_OPTION_ARG_STRING, &opt_input_format,
			"Input format", NULL},
	{"output-file", 'o', 0, G_OPTION_ARG_FILENAME, &opt_output_file,
			"Save output to file", NULL},
	{"output-format", 'O', 0, G_OPTION_ARG_STRING, &opt_output_format,
			"Output format", NULL},
	{"probes", 'p', 0, G_OPTION_ARG_STRING, &opt_probes,
			"Probes to use", NULL},
	{"triggers", 't', 0, G_OPTION_ARG_STRING, &opt_triggers,
			"Trigger configuration", NULL},
	{"wait-trigger", 'w', 0, G_OPTION_ARG_NONE, &opt_wait_trigger,
			"Wait for trigger", NULL},
	{"protocol-decoders", 'a', 0, G_OPTION_ARG_STRING, &opt_pds,
			"Protocol decoders to run", NULL},
	{"protocol-decoder-stack", 's', 0, G_OPTION_ARG_STRING, &opt_pd_stack,
			"Protocol decoder stack", NULL},
	{"protocol-decoder-annotations", 'A', 0, G_OPTION_ARG_STRING, &opt_pd_annotations,
			"Protocol decoder annotation(s) to show", NULL},
	{"show", 0, 0, G_OPTION_ARG_NONE, &opt_show,
			"Show device detail", NULL},
	{"time", 0, 0, G_OPTION_ARG_STRING, &opt_time,
			"How long to sample (ms)", NULL},
	{"samples", 0, 0, G_OPTION_ARG_STRING, &opt_samples,
			"Number of samples to acquire", NULL},
	{"frames", 0, 0, G_OPTION_ARG_STRING, &opt_frames,
			"Number of frames to acquire", NULL},
	{"continuous", 0, 0, G_OPTION_ARG_NONE, &opt_continuous,
			"Sample continuously", NULL},
	{NULL, 0, 0, 0, NULL, NULL, NULL}
};


/* Convert driver options hash to GSList of struct sr_hwopt. */
static GSList *hash_to_hwopt(GHashTable *hash)
{
	const struct sr_hwcap_option *hwo;
	struct sr_hwopt *hwopt;
	GList *gl, *keys;
	GSList *opts;
	char *key, *value;

	keys = g_hash_table_get_keys(hash);
	opts = NULL;
	for (gl = keys; gl; gl = gl->next) {
		key = gl->data;
		if (!(hwo = sr_drvopt_name_get(key))) {
			g_critical("Unknown option %s", key);
			return NULL;
		}
		hwopt = g_try_malloc(sizeof(struct sr_hwopt));
		hwopt->hwopt = hwo->hwcap;
		value = g_hash_table_lookup(hash, key);
		hwopt->value = g_strdup(value);
		opts = g_slist_append(opts, hwopt);
	}
	g_list_free(keys);

	return opts;
}

static GSList *device_scan(void)
{
	struct sr_dev_driver **drivers, *driver;
	GHashTable *drvargs;
	GSList *drvopts, *devices, *tmpdevs, *l;
	int i;
	char *drvname;

	if (opt_drv) {
		drvargs = parse_generic_arg(opt_drv, TRUE);
		drvname = g_strdup(g_hash_table_lookup(drvargs, "sigrok_key"));
		g_hash_table_remove(drvargs, "sigrok_key");
		driver = NULL;
		drivers = sr_driver_list();
		for (i = 0; drivers[i]; i++) {
			if (strcmp(drivers[i]->name, drvname))
				continue;
			driver = drivers[i];
		}
		if (!driver) {
			g_critical("Driver %s not found.", drvname);
			return NULL;
		}
		g_free(drvname);
		if (sr_driver_init(sr_ctx, driver) != SR_OK) {
			g_critical("Failed to initialize driver.");
			return NULL;
		}
		drvopts = NULL;
		if (g_hash_table_size(drvargs) > 0)
			if (!(drvopts = hash_to_hwopt(drvargs)))
				/* Unknown options, already logged. */
				return NULL;
		devices = sr_driver_scan(driver, drvopts);
	} else {
		/* No driver specified, let them all scan on their own. */
		devices = NULL;
		drivers = sr_driver_list();
		for (i = 0; drivers[i]; i++) {
			driver = drivers[i];
			if (sr_driver_init(sr_ctx, driver) != SR_OK) {
				g_critical("Failed to initialize driver.");
				return NULL;
			}
			tmpdevs = sr_driver_scan(driver, NULL);
			for (l = tmpdevs; l; l = l->next)
				devices = g_slist_append(devices, l->data);
			g_slist_free(tmpdevs);
		}
	}

	return devices;
}

static void show_version(void)
{
	GSList *l;
	struct sr_dev_driver **drivers;
	struct sr_input_format **inputs;
	struct sr_output_format **outputs;
	struct srd_decoder *dec;
	int i;

	printf("sigrok-cli %s\n\n", VERSION);

	printf("Using libsigrok %s (lib version %s).\n",
	       sr_package_version_string_get(), sr_lib_version_string_get());
	printf("Using libsigrokdecode %s (lib version %s).\n\n",
	       srd_package_version_string_get(), srd_lib_version_string_get());

	printf("Supported hardware drivers:\n");
	drivers = sr_driver_list();
	for (i = 0; drivers[i]; i++) {
		printf("  %-20s %s\n", drivers[i]->name, drivers[i]->longname);
	}
	printf("\n");

	printf("Supported input formats:\n");
	inputs = sr_input_list();
	for (i = 0; inputs[i]; i++)
		printf("  %-20s %s\n", inputs[i]->id, inputs[i]->description);
	printf("\n");

	printf("Supported output formats:\n");
	outputs = sr_output_list();
	for (i = 0; outputs[i]; i++)
		printf("  %-20s %s\n", outputs[i]->id, outputs[i]->description);
	printf("\n");

	if (srd_init(NULL) == SRD_OK) {
		printf("Supported protocol decoders:\n");
		srd_decoder_load_all();
		for (l = srd_decoder_list(); l; l = l->next) {
			dec = l->data;
			printf("  %-20s %s\n", dec->id, dec->longname);
			/* Print protocol description upon "-l 3" or higher. */
			if (opt_loglevel >= SR_LOG_INFO)
				printf("  %-20s %s\n", "", dec->desc);
		}
		srd_exit();
	}
	printf("\n");
}

static void print_dev_line(const struct sr_dev_inst *sdi)
{
	struct sr_probe *probe;
	GSList *l;

	if (sdi->vendor && sdi->vendor[0])
		printf("%s ", sdi->vendor);
	if (sdi->model && sdi->model[0])
		printf("%s ", sdi->model);
	if (sdi->version && sdi->version[0])
		printf("%s ", sdi->version);
	if (sdi->probes) {
		if (g_slist_length(sdi->probes) == 1) {
			probe = sdi->probes->data;
			printf("with 1 probe: %s", probe->name);
		} else {
			printf("with %d probes:", g_slist_length(sdi->probes));
			for (l = sdi->probes; l; l = l->next) {
				probe = l->data;
				printf(" %s", probe->name);
			}
		}
	}
	printf("\n");
}

static void show_dev_list(void)
{
	struct sr_dev_inst *sdi;
	GSList *devices, *l;

	if (!(devices = device_scan()))
		return;

	printf("The following devices were found:\n");
	for (l = devices; l; l = l->next) {
		sdi = l->data;
		print_dev_line(sdi);
	}
	g_slist_free(devices);

}

static void show_dev_detail(void)
{
	struct sr_dev_inst *sdi;
	const struct sr_hwcap_option *hwo;
	const struct sr_samplerates *samplerates;
	struct sr_rational *rationals;
	GSList *devices;
	uint64_t *integers;
	const int *hwopts, *hwcaps;
	int cap, num_devices, n, i;
	char *s, *title;
	const char *charopts, **stropts;

	if (!(devices = device_scan())) {
		g_critical("No devices found.");
		return;
	}

	num_devices = g_slist_length(devices);
	if (num_devices > 1) {
		if (!opt_dev) {
			g_critical("%d devices found. Use --list-devices to show them, "
					"and --device to select one.", num_devices);
			return;
		}
		/* opt_dev is NULL if not specified, which is fine. */
		n = strtol(opt_dev, NULL, 10);
		if (n >= num_devices) {
			g_critical("%d devices found, numbered starting from 0.",
					num_devices);
			return;
		}
		sdi = g_slist_nth_data(devices, n);
	} else
		sdi = g_slist_nth_data(devices, 0);

	print_dev_line(sdi);

	if (sr_info_get(sdi->driver, SR_DI_TRIGGER_TYPES, (const void **)&charopts,
			sdi) == SR_OK && charopts) {
		printf("Supported triggers: ");
		while (*charopts) {
			printf("%c ", *charopts);
			charopts++;
		}
		printf("\n");
	}

	if ((sr_info_get(sdi->driver, SR_DI_HWOPTS, (const void **)&hwopts,
			NULL) == SR_OK) && hwopts) {
		printf("Supported driver options:\n");
		for (i = 0; hwopts[i]; i++) {
			if (!(hwo = sr_drvopt_get(hwopts[i])))
				continue;
			printf("    %s\n", hwo->shortname);
		}
	}

	title = "Supported device options:\n";
	if ((sr_info_get(sdi->driver, SR_DI_HWCAPS, (const void **)&hwcaps,
			NULL) != SR_OK) || !hwcaps)
		/* Driver supports no device instance options. */
		return;

	for (cap = 0; hwcaps[cap]; cap++) {
		if (!(hwo = sr_devopt_get(hwcaps[cap])))
			continue;

		if (title) {
			printf("%s", title);
			title = NULL;
		}

		if (hwo->hwcap == SR_HWCAP_PATTERN_MODE) {
			/* Pattern generator modes */
			printf("    %s", hwo->shortname);
			if (sr_info_get(sdi->driver, SR_DI_PATTERNS,
					(const void **)&stropts, sdi) == SR_OK) {
				printf(" - supported patterns:\n");
				for (i = 0; stropts[i]; i++)
					printf("      %s\n", stropts[i]);
			} else {
				printf("\n");
			}

		} else if (hwo->hwcap == SR_HWCAP_SAMPLERATE) {
			/* Supported samplerates */
			printf("    %s", hwo->shortname);
			if (sr_info_get(sdi->driver, SR_DI_SAMPLERATES,
					(const void **)&samplerates, sdi) != SR_OK) {
				printf("\n");
				continue;
			}
			if (samplerates->step) {
				/* low */
				if (!(s = sr_samplerate_string(samplerates->low)))
					continue;
				printf(" (%s", s);
				g_free(s);
				/* high */
				if (!(s = sr_samplerate_string(samplerates->high)))
					continue;
				printf(" - %s", s);
				g_free(s);
				/* step */
				if (!(s = sr_samplerate_string(samplerates->step)))
					continue;
				printf(" in steps of %s)\n", s);
				g_free(s);
			} else {
				printf(" - supported samplerates:\n");
				for (i = 0; samplerates->list[i]; i++)
					printf("      %s\n", sr_samplerate_string(samplerates->list[i]));
			}

		} else if (hwo->hwcap == SR_HWCAP_BUFFERSIZE) {
			/* Supported buffer sizes */
			printf("    %s", hwo->shortname);
			if (sr_info_get(sdi->driver, SR_DI_BUFFERSIZES,
					(const void **)&integers, sdi) != SR_OK) {
				printf("\n");
				continue;
			}
			printf(" - supported buffer sizes:\n");
			for (i = 0; integers[i]; i++)
				printf("      %"PRIu64"\n", integers[i]);

		} else if (hwo->hwcap == SR_HWCAP_TIMEBASE) {
			/* Supported time bases */
			printf("    %s", hwo->shortname);
			if (sr_info_get(sdi->driver, SR_DI_TIMEBASES,
					(const void **)&rationals, sdi) != SR_OK) {
				printf("\n");
				continue;
			}
			printf(" - supported time bases:\n");
			for (i = 0; rationals[i].p && rationals[i].q; i++)
				printf("      %s\n", sr_period_string(
						rationals[i].p * rationals[i].q));

		} else if (hwo->hwcap == SR_HWCAP_TRIGGER_SOURCE) {
			/* Supported trigger sources */
			printf("    %s", hwo->shortname);
			if (sr_info_get(sdi->driver, SR_DI_TRIGGER_SOURCES,
					(const void **)&stropts, sdi) != SR_OK) {
				printf("\n");
				continue;
			}
			printf(" - supported trigger sources:\n");
			for (i = 0; stropts[i]; i++)
				printf("      %s\n", stropts[i]);

		} else if (hwo->hwcap == SR_HWCAP_FILTER) {
			/* Supported filters */
			printf("    %s", hwo->shortname);
			if (sr_info_get(sdi->driver, SR_DI_FILTERS,
					(const void **)&stropts, sdi) != SR_OK) {
				printf("\n");
				continue;
			}
			printf(" - supported filter targets:\n");
			for (i = 0; stropts[i]; i++)
				printf("      %s\n", stropts[i]);

		} else if (hwo->hwcap == SR_HWCAP_VDIV) {
			/* Supported volts/div values */
			printf("    %s", hwo->shortname);
			if (sr_info_get(sdi->driver, SR_DI_VDIVS,
					(const void **)&rationals, sdi) != SR_OK) {
				printf("\n");
				continue;
			}
			printf(" - supported volts/div:\n");
			for (i = 0; rationals[i].p && rationals[i].q; i++)
				printf("      %s\n", sr_voltage_string(	&rationals[i]));

		} else if (hwo->hwcap == SR_HWCAP_COUPLING) {
			/* Supported coupling settings */
			printf("    %s", hwo->shortname);
			if (sr_info_get(sdi->driver, SR_DI_COUPLING,
					(const void **)&stropts, sdi) != SR_OK) {
				printf("\n");
				continue;
			}
			printf(" - supported coupling options:\n");
			for (i = 0; stropts[i]; i++)
				printf("      %s\n", stropts[i]);

		} else {
			/* Everything else */
			printf("    %s\n", hwo->shortname);
		}
	}

}

static void show_pd_detail(void)
{
	GSList *l;
	struct srd_decoder *dec;
	char **pdtokens, **pdtok, **ann, *doc;
	struct srd_probe *p;

	pdtokens = g_strsplit(opt_pds, ",", -1);
	for (pdtok = pdtokens; *pdtok; pdtok++) {
		if (!(dec = srd_decoder_get_by_id(*pdtok))) {
			g_critical("Protocol decoder %s not found.", *pdtok);
			return;
		}
		printf("ID: %s\nName: %s\nLong name: %s\nDescription: %s\n",
				dec->id, dec->name, dec->longname, dec->desc);
		printf("License: %s\n", dec->license);
		printf("Annotations:\n");
		if (dec->annotations) {
			for (l = dec->annotations; l; l = l->next) {
				ann = l->data;
				printf("- %s\n  %s\n", ann[0], ann[1]);
			}
		} else {
			printf("None.\n");
		}
		/* TODO: Print supported decoder options. */
		printf("Required probes:\n");
		if (dec->probes) {
			for (l = dec->probes; l; l = l->next) {
				p = l->data;
				printf("- %s (%s): %s\n",
				       p->name, p->id, p->desc);
			}
		} else {
			printf("None.\n");
		}
		printf("Optional probes:\n");
		if (dec->opt_probes) {
			for (l = dec->opt_probes; l; l = l->next) {
				p = l->data;
				printf("- %s (%s): %s\n",
				       p->name, p->id, p->desc);
			}
		} else {
			printf("None.\n");
		}
		if ((doc = srd_decoder_doc_get(dec))) {
			printf("Documentation:\n%s\n",
			       doc[0] == '\n' ? doc + 1 : doc);
			g_free(doc);
		}
	}

	g_strfreev(pdtokens);
}

static void datafeed_in(const struct sr_dev_inst *sdi,
		const struct sr_datafeed_packet *packet)
{
	static struct sr_output *o = NULL;
	static int logic_probelist[SR_MAX_NUM_PROBES] = { -1 };
	static struct sr_probe *analog_probelist[SR_MAX_NUM_PROBES];
	static uint64_t received_samples = 0;
	static int unitsize = 0;
	static int triggered = 0;
	static FILE *outfile = NULL;
	static int num_analog_probes = 0;
	struct sr_probe *probe;
	const struct sr_datafeed_logic *logic;
	const struct sr_datafeed_meta_logic *meta_logic;
	const struct sr_datafeed_analog *analog;
	const struct sr_datafeed_meta_analog *meta_analog;
	static int num_enabled_analog_probes = 0;
	int num_enabled_probes, sample_size, ret, i;
	uint64_t output_len, filter_out_len;
	uint8_t *output_buf, *filter_out;
	GString *out;

	/* If the first packet to come in isn't a header, don't even try. */
	if (packet->type != SR_DF_HEADER && o == NULL)
		return;

	sample_size = -1;
	switch (packet->type) {
	case SR_DF_HEADER:
		g_debug("cli: Received SR_DF_HEADER");
		/* Initialize the output module. */
		if (!(o = g_try_malloc(sizeof(struct sr_output)))) {
			g_critical("Output module malloc failed.");
			exit(1);
		}
		o->format = output_format;
		o->sdi = (struct sr_dev_inst *)sdi;
		o->param = output_format_param;
		if (o->format->init) {
			if (o->format->init(o) != SR_OK) {
				g_critical("Output format initialization failed.");
				exit(1);
			}
		}
		break;

	case SR_DF_END:
		g_debug("cli: Received SR_DF_END");
		if (!o) {
			g_debug("cli: double end!");
			break;
		}
		if (o->format->event) {
			o->format->event(o, SR_DF_END, &output_buf, &output_len);
			if (output_buf) {
				if (outfile)
					fwrite(output_buf, 1, output_len, outfile);
				g_free(output_buf);
				output_len = 0;
			}
		}
		if (limit_samples && received_samples < limit_samples)
			g_warning("Device only sent %" PRIu64 " samples.",
			       received_samples);
		if (opt_continuous)
			g_warning("Device stopped after %" PRIu64 " samples.",
			       received_samples);
		if (outfile && outfile != stdout)
			fclose(outfile);

		if (o->format->cleanup)
			o->format->cleanup(o);
		g_free(o);
		o = NULL;
		break;

	case SR_DF_TRIGGER:
		g_debug("cli: received SR_DF_TRIGGER");
		if (o->format->event)
			o->format->event(o, SR_DF_TRIGGER, &output_buf,
					 &output_len);
		triggered = 1;
		break;

	case SR_DF_META_LOGIC:
		g_message("cli: Received SR_DF_META_LOGIC");
		meta_logic = packet->payload;
		num_enabled_probes = 0;
		for (i = 0; i < meta_logic->num_probes; i++) {
			probe = g_slist_nth_data(sdi->probes, i);
			if (probe->enabled)
				logic_probelist[num_enabled_probes++] = probe->index;
		}
		logic_probelist[num_enabled_probes] = -1;
		/* How many bytes we need to store num_enabled_probes bits */
		unitsize = (num_enabled_probes + 7) / 8;

		outfile = stdout;
		if (opt_output_file) {
			if (default_output_format) {
				/* output file is in session format, which means we'll
				 * dump everything in the datastore as it comes in,
				 * and save from there after the session. */
				outfile = NULL;
				ret = sr_datastore_new(unitsize, &singleds);
				if (ret != SR_OK) {
					g_critical("Failed to create datastore.");
					exit(1);
				}
			} else {
				/* saving to a file in whatever format was set
				 * with --format, so all we need is a filehandle */
				outfile = g_fopen(opt_output_file, "wb");
			}
		}
		if (opt_pds)
			srd_session_start(num_enabled_probes, unitsize,
					meta_logic->samplerate);
		break;

	case SR_DF_LOGIC:
		logic = packet->payload;
		g_message("cli: received SR_DF_LOGIC, %"PRIu64" bytes", logic->length);
		sample_size = logic->unitsize;
		if (logic->length == 0)
			break;

		/* Don't store any samples until triggered. */
		if (opt_wait_trigger && !triggered)
			break;

		if (limit_samples && received_samples >= limit_samples)
			break;

		ret = sr_filter_probes(sample_size, unitsize, logic_probelist,
				logic->data, logic->length,
				&filter_out, &filter_out_len);
		if (ret != SR_OK)
			break;

		/* what comes out of the filter is guaranteed to be packed into the
		 * minimum size needed to support the number of samples at this sample
		 * size. however, the driver may have submitted too much -- cut off
		 * the buffer of the last packet according to the sample limit.
		 */
		if (limit_samples && (received_samples + logic->length / sample_size >
				limit_samples * sample_size))
			filter_out_len = limit_samples * sample_size - received_samples;

		if (singleds)
			sr_datastore_put(singleds, filter_out,
					filter_out_len, sample_size, logic_probelist);

		if (opt_output_file && default_output_format)
			/* saving to a session file, don't need to do anything else
			 * to this data for now. */
			goto cleanup;

		if (opt_pds) {
			if (srd_session_send(received_samples, (uint8_t*)filter_out,
					filter_out_len) != SRD_OK)
				sr_session_stop();
		} else {
			output_len = 0;
			if (o->format->data && packet->type == o->format->df_type)
				o->format->data(o, filter_out, filter_out_len, &output_buf, &output_len);
			if (output_buf) {
				fwrite(output_buf, 1, output_len, outfile);
				fflush(outfile);
				g_free(output_buf);
			}
		}

		cleanup:
		g_free(filter_out);
		received_samples += logic->length / sample_size;
		break;

	case SR_DF_META_ANALOG:
		g_message("cli: Received SR_DF_META_ANALOG");
		meta_analog = packet->payload;
		num_analog_probes = meta_analog->num_probes;
		num_enabled_analog_probes = 0;
		for (i = 0; i < num_analog_probes; i++) {
			probe = g_slist_nth_data(sdi->probes, i);
			if (probe->enabled)
				analog_probelist[num_enabled_analog_probes++] = probe;
		}

		outfile = stdout;
		if (opt_output_file) {
			if (default_output_format) {
				/* output file is in session format, which means we'll
				 * dump everything in the datastore as it comes in,
				 * and save from there after the session. */
				outfile = NULL;
				ret = sr_datastore_new(unitsize, &singleds);
				if (ret != SR_OK) {
					g_critical("Failed to create datastore.");
					exit(1);
				}
			} else {
				/* saving to a file in whatever format was set
				 * with --format, so all we need is a filehandle */
				outfile = g_fopen(opt_output_file, "wb");
			}
		}
		break;

	case SR_DF_ANALOG:
		analog = packet->payload;
		g_message("cli: received SR_DF_ANALOG, %d samples", analog->num_samples);
		if (analog->num_samples == 0)
			break;

		if (limit_samples && received_samples >= limit_samples)
			break;

		if (o->format->data && packet->type == o->format->df_type) {
			o->format->data(o, (const uint8_t *)analog->data,
					analog->num_samples * sizeof(float),
					&output_buf, &output_len);
			if (output_buf) {
				fwrite(output_buf, 1, output_len, outfile);
				fflush(outfile);
				g_free(output_buf);
			}
		}

		received_samples += analog->num_samples;
		break;

	case SR_DF_FRAME_BEGIN:
		g_debug("cli: received SR_DF_FRAME_BEGIN");
		if (o->format->event) {
			o->format->event(o, SR_DF_FRAME_BEGIN, &output_buf,
					 &output_len);
			if (output_buf) {
				fwrite(output_buf, 1, output_len, outfile);
				fflush(outfile);
				g_free(output_buf);
			}
		}
		break;

	case SR_DF_FRAME_END:
		g_debug("cli: received SR_DF_FRAME_END");
		if (o->format->event) {
			o->format->event(o, SR_DF_FRAME_END, &output_buf,
					 &output_len);
			if (output_buf) {
				fwrite(output_buf, 1, output_len, outfile);
				fflush(outfile);
				g_free(output_buf);
			}
		}
		break;

	default:
		g_message("received unknown packet type %d", packet->type);
	}

	if (o && o->format->recv) {
		out = o->format->recv(o, sdi, packet);
		if (out && out->len) {
			fwrite(out->str, 1, out->len, outfile);
			fflush(outfile);
		}
	}

}

/* Register the given PDs for this session.
 * Accepts a string of the form: "spi:sck=3:sdata=4,spi:sck=3:sdata=5"
 * That will instantiate two SPI decoders on the clock but different data
 * lines.
 */
static int register_pds(struct sr_dev *dev, const char *pdstring)
{
	GHashTable *pd_opthash;
	struct srd_decoder_inst *di;
	int ret;
	char **pdtokens, **pdtok, *pd_name;

	(void)dev;

	ret = 0;
	pd_ann_visible = g_hash_table_new_full(g_str_hash, g_int_equal,
			g_free, NULL);
	pd_name = NULL;
	pd_opthash = NULL;
	pdtokens = g_strsplit(pdstring, ",", 0);
	for (pdtok = pdtokens; *pdtok; pdtok++) {
		if (!(pd_opthash = parse_generic_arg(*pdtok, TRUE))) {
			g_critical("Invalid protocol decoder option '%s'.", *pdtok);
			goto err_out;
		}

		pd_name = g_strdup(g_hash_table_lookup(pd_opthash, "sigrok_key"));
		g_hash_table_remove(pd_opthash, "sigrok_key");
		if (srd_decoder_load(pd_name) != SRD_OK) {
			g_critical("Failed to load protocol decoder %s.", pd_name);
			ret = 1;
			goto err_out;
		}
		if (!(di = srd_inst_new(pd_name, pd_opthash))) {
			g_critical("Failed to instantiate protocol decoder %s.", pd_name);
			ret = 1;
			goto err_out;
		}

		/* If no annotation list was specified, add them all in now.
		 * This will be pared down later to leave only the last PD
		 * in the stack.
		 */
		if (!opt_pd_annotations)
			g_hash_table_insert(pd_ann_visible,
					    g_strdup(di->inst_id), NULL);

		/* Any keys left in the options hash are probes, where the key
		 * is the probe name as specified in the decoder class, and the
		 * value is the probe number i.e. the order in which the PD's
		 * incoming samples are arranged. */
		if (srd_inst_probe_set_all(di, pd_opthash) != SRD_OK) {
			ret = 1;
			goto err_out;
		}
		g_hash_table_destroy(pd_opthash);
		pd_opthash = NULL;
	}

err_out:
	g_strfreev(pdtokens);
	if (pd_opthash)
		g_hash_table_destroy(pd_opthash);
	if (pd_name)
		g_free(pd_name);

	return ret;
}

int setup_pd_stack(void)
{
	struct srd_decoder_inst *di_from, *di_to;
	int ret, i;
	char **pds, **ids;

	/* Set up the protocol decoder stack. */
	pds = g_strsplit(opt_pds, ",", 0);
	if (g_strv_length(pds) > 1) {
		if (opt_pd_stack) {
			/* A stack setup was specified, use that. */
			g_strfreev(pds);
			pds = g_strsplit(opt_pd_stack, ",", 0);
			if (g_strv_length(pds) < 2) {
				g_strfreev(pds);
				g_critical("Specify at least two protocol decoders to stack.");
				return 1;
			}
		}

		/* First PD goes at the bottom of the stack. */
		ids = g_strsplit(pds[0], ":", 0);
		if (!(di_from = srd_inst_find_by_id(ids[0]))) {
			g_strfreev(ids);
			g_critical("Cannot stack protocol decoder '%s': "
					"instance not found.", pds[0]);
			return 1;
		}
		g_strfreev(ids);

		/* Every subsequent PD goes on top. */
		for (i = 1; pds[i]; i++) {
			ids = g_strsplit(pds[i], ":", 0);
			if (!(di_to = srd_inst_find_by_id(ids[0]))) {
				g_strfreev(ids);
				g_critical("Cannot stack protocol decoder '%s': "
						"instance not found.", pds[i]);
				return 1;
			}
			g_strfreev(ids);
			if ((ret = srd_inst_stack(di_from, di_to)) != SRD_OK)
				return 1;

			/* Don't show annotation from this PD. Only the last PD in
			 * the stack will be left on the annotation list (unless
			 * the annotation list was specifically provided).
			 */
			if (!opt_pd_annotations)
				g_hash_table_remove(pd_ann_visible,
						    di_from->inst_id);

			di_from = di_to;
		}
	}
	g_strfreev(pds);

	return 0;
}

int setup_pd_annotations(void)
{
	GSList *l;
	struct srd_decoder *dec;
	int ann;
	char **pds, **pdtok, **keyval, **ann_descr;

	/* Set up custom list of PDs and annotations to show. */
	if (opt_pd_annotations) {
		pds = g_strsplit(opt_pd_annotations, ",", 0);
		for (pdtok = pds; *pdtok && **pdtok; pdtok++) {
			ann = 0;
			keyval = g_strsplit(*pdtok, "=", 0);
			if (!(dec = srd_decoder_get_by_id(keyval[0]))) {
				g_critical("Protocol decoder '%s' not found.", keyval[0]);
				return 1;
			}
			if (!dec->annotations) {
				g_critical("Protocol decoder '%s' has no annotations.", keyval[0]);
				return 1;
			}
			if (g_strv_length(keyval) == 2) {
				for (l = dec->annotations; l; l = l->next, ann++) {
					ann_descr = l->data;
					if (!canon_cmp(ann_descr[0], keyval[1]))
						/* Found it. */
						break;
				}
				if (!l) {
					g_critical("Annotation '%s' not found "
							"for protocol decoder '%s'.", keyval[1], keyval[0]);
					return 1;
				}
			}
			g_debug("cli: showing protocol decoder annotation %d from '%s'", ann, keyval[0]);
			g_hash_table_insert(pd_ann_visible, g_strdup(keyval[0]), GINT_TO_POINTER(ann));
			g_strfreev(keyval);
		}
		g_strfreev(pds);
	}

	return 0;
}

int setup_output_format(void)
{
	GHashTable *fmtargs;
	GHashTableIter iter;
	gpointer key, value;
	struct sr_output_format **outputs;
	int i;
	char *fmtspec;

	if (!opt_output_format) {
		opt_output_format = DEFAULT_OUTPUT_FORMAT;
		/* we'll need to remember this so when saving to a file
		 * later, sigrok session format will be used.
		 */
		default_output_format = TRUE;
	}

	fmtargs = parse_generic_arg(opt_output_format, TRUE);
	fmtspec = g_hash_table_lookup(fmtargs, "sigrok_key");
	if (!fmtspec) {
		g_critical("Invalid output format.");
		return 1;
	}
	outputs = sr_output_list();
	for (i = 0; outputs[i]; i++) {
		if (strcmp(outputs[i]->id, fmtspec))
			continue;
		g_hash_table_remove(fmtargs, "sigrok_key");
		output_format = outputs[i];
		g_hash_table_iter_init(&iter, fmtargs);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			/* only supporting one parameter per output module
			 * for now, and only its value */
			output_format_param = g_strdup(value);
			break;
		}
		break;
	}
	if (!output_format) {
		g_critical("Invalid output format %s.", opt_output_format);
		return 1;
	}
	g_hash_table_destroy(fmtargs);

	return 0;
}

void show_pd_annotations(struct srd_proto_data *pdata, void *cb_data)
{
	int i;
	char **annotations;
	gpointer ann_format;

	/* 'cb_data' is not used in this specific callback. */
	(void)cb_data;

	if (!pd_ann_visible)
		return;

	if (!g_hash_table_lookup_extended(pd_ann_visible, pdata->pdo->di->inst_id,
			NULL, &ann_format))
		/* Not in the list of PDs whose annotations we're showing. */
		return;

	if (pdata->ann_format != GPOINTER_TO_INT(ann_format))
		/* We don't want this particular format from the PD. */
		return;

	annotations = pdata->data;
	if (opt_loglevel > SR_LOG_WARN)
		printf("%"PRIu64"-%"PRIu64" ", pdata->start_sample, pdata->end_sample);
	printf("%s: ", pdata->pdo->proto_id);
	for (i = 0; annotations[i]; i++)
		printf("\"%s\" ", annotations[i]);
	printf("\n");
	fflush(stdout);
}

static int select_probes(struct sr_dev_inst *sdi)
{
	struct sr_probe *probe;
	GSList *selected_probes, *l;

	if (!opt_probes)
		return SR_OK;

	if (!(selected_probes = parse_probestring(sdi, opt_probes)))
		return SR_ERR;

	for (l = sdi->probes; l; l = l->next) {
		probe = l->data;
		if (g_slist_find(selected_probes, probe))
			probe->enabled = TRUE;
		else
			probe->enabled = FALSE;
	}
	g_slist_free(selected_probes);

	return SR_OK;
}

/**
 * Return the input file format which the CLI tool should use.
 *
 * If the user specified -I / --input-format, use that one. Otherwise, try to
 * autodetect the format as good as possible. Failing that, return NULL.
 *
 * @param filename The filename of the input file. Must not be NULL.
 * @param opt The -I / --input-file option the user specified (or NULL).
 *
 * @return A pointer to the 'struct sr_input_format' that should be used,
 *         or NULL if no input format was selected or auto-detected.
 */
static struct sr_input_format *determine_input_file_format(
			const char *filename, const char *opt)
{
	int i;
	struct sr_input_format **inputs;

	/* If there are no input formats, return NULL right away. */
	inputs = sr_input_list();
	if (!inputs) {
		g_critical("No supported input formats available.");
		return NULL;
	}

	/* If the user specified -I / --input-format, use that one. */
	if (opt) {
		for (i = 0; inputs[i]; i++) {
			if (strcasecmp(inputs[i]->id, opt))
				continue;
			g_debug("Using user-specified input file format '%s'.",
					inputs[i]->id);
			return inputs[i];
		}

		/* The user specified an unknown input format, return NULL. */
		g_critical("Error: specified input file format '%s' is "
			"unknown.", opt);
		return NULL;
	}

	/* Otherwise, try to find an input module that can handle this file. */
	for (i = 0; inputs[i]; i++) {
		if (inputs[i]->format_match(filename))
			break;
	}

	/* Return NULL if no input module wanted to touch this. */
	if (!inputs[i]) {
		g_critical("Error: no matching input module found.");
		return NULL;
	}

	g_debug("cli: Autodetected '%s' input format for file '%s'.",
		inputs[i]->id, filename);
		
	return inputs[i];
}

static void load_input_file_format(void)
{
	GHashTable *fmtargs = NULL;
	struct stat st;
	struct sr_input *in;
	struct sr_input_format *input_format;
	char *fmtspec = NULL;

	if (opt_input_format) {
		fmtargs = parse_generic_arg(opt_input_format, TRUE);
		fmtspec = g_hash_table_lookup(fmtargs, "sigrok_key");
	}

	if (!(input_format = determine_input_file_format(opt_input_file,
						   fmtspec))) {
		/* The exact cause was already logged. */
		return;
	}

	if (fmtargs)
		g_hash_table_remove(fmtargs, "sigrok_key");

	if (stat(opt_input_file, &st) == -1) {
		g_critical("Failed to load %s: %s", opt_input_file,
			strerror(errno));
		exit(1);
	}

	/* Initialize the input module. */
	if (!(in = g_try_malloc(sizeof(struct sr_input)))) {
		g_critical("Failed to allocate input module.");
		exit(1);
	}
	in->format = input_format;
	in->param = fmtargs;
	if (in->format->init) {
		if (in->format->init(in) != SR_OK) {
			g_critical("Input format init failed.");
			exit(1);
		}
	}

	if (select_probes(in->sdi) > 0)
            return;

	sr_session_new();
	sr_session_datafeed_callback_add(datafeed_in);
	if (sr_session_dev_add(in->sdi) != SR_OK) {
		g_critical("Failed to use device.");
		sr_session_destroy();
		return;
	}

	input_format->loadfile(in, opt_input_file);
	if (opt_output_file && default_output_format) {
		if (sr_session_save(opt_output_file, in->sdi, singleds) != SR_OK)
			g_critical("Failed to save session.");
	}
	sr_session_destroy();

	if (fmtargs)
		g_hash_table_destroy(fmtargs);
}

static void load_input_file(void)
{

	if (sr_session_load(opt_input_file) == SR_OK) {
		/* sigrok session file */
		sr_session_datafeed_callback_add(datafeed_in);
		sr_session_start();
		sr_session_run();
		sr_session_stop();
	}
	else {
		/* fall back on input modules */
		load_input_file_format();
	}
}

static int set_dev_options(struct sr_dev_inst *sdi, GHashTable *args)
{
	const struct sr_hwcap_option *hwo;
	GHashTableIter iter;
	gpointer key, value;
	int ret;
	float tmp_float;
	uint64_t tmp_u64;
	struct sr_rational tmp_rat;
	gboolean tmp_bool;
	void *val;

	g_hash_table_iter_init(&iter, args);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (!(hwo = sr_devopt_name_get(key))) {
			g_critical("Unknown device option '%s'.", (char *) key);
			return SR_ERR;
		}

		if ((value == NULL) &&
			(hwo->type != SR_T_BOOL)) {
			g_critical("Option '%s' needs a value.", (char *)key);
			return SR_ERR;
		}
		val = NULL;
		switch (hwo->type) {
		case SR_T_UINT64:
			ret = sr_parse_sizestring(value, &tmp_u64);
			if (ret != SR_OK)
				break;
			val = &tmp_u64;
			break;
		case SR_T_CHAR:
			val = value;
			break;
		case SR_T_BOOL:
			if (!value)
				tmp_bool = TRUE;
			else
				tmp_bool = sr_parse_boolstring(value);
			val = &tmp_bool;
			break;
		case SR_T_FLOAT:
			tmp_float = strtof(value, NULL);
			val = &tmp_float;
			break;
		case SR_T_RATIONAL_PERIOD:
			if ((ret = sr_parse_period(value, &tmp_rat)) != SR_OK)
				break;
			val = &tmp_rat;
			break;
		case SR_T_RATIONAL_VOLT:
			if ((ret = sr_parse_voltage(value, &tmp_rat)) != SR_OK)
				break;
			val = &tmp_rat;
			break;
		default:
			ret = SR_ERR;
		}
		if (val)
			ret = sr_dev_config_set(sdi, hwo->hwcap, val);
		if (ret != SR_OK) {
			g_critical("Failed to set device option '%s'.", (char *)key);
			return ret;
		}
		else
			break;
	}

	return SR_OK;
}

static int set_limit_time(const struct sr_dev_inst *sdi)
{
	uint64_t time_msec;
	uint64_t *samplerate;

	time_msec = sr_parse_timestring(opt_time);
	if (time_msec == 0) {
		g_critical("Invalid time '%s'", opt_time);
		sr_session_destroy();
		return SR_ERR;
	}

	if (sr_driver_hwcap_exists(sdi->driver, SR_HWCAP_LIMIT_MSEC)) {
		if (sr_dev_config_set(sdi, SR_HWCAP_LIMIT_MSEC, &time_msec) != SR_OK) {
			g_critical("Failed to configure time limit.");
			sr_session_destroy();
			return SR_ERR;
		}
	}
	else {
		/* time limit set, but device doesn't support this...
		 * convert to samples based on the samplerate.
		 */
		limit_samples = 0;
		if (sr_dev_has_hwcap(sdi, SR_HWCAP_SAMPLERATE)) {
			sr_info_get(sdi->driver, SR_DI_CUR_SAMPLERATE,
					(const void **)&samplerate, sdi);
			limit_samples = (*samplerate) * time_msec / (uint64_t)1000;
		}
		if (limit_samples == 0) {
			g_critical("Not enough time at this samplerate.");
			sr_session_destroy();
			return SR_ERR;
		}

		if (sr_dev_config_set(sdi, SR_HWCAP_LIMIT_SAMPLES,
					&limit_samples) != SR_OK) {
			g_critical("Failed to configure time-based sample limit.");
			sr_session_destroy();
			return SR_ERR;
		}
	}

	return SR_OK;
}

static void run_session(void)
{
	GSList *devices;
	GHashTable *devargs;
	struct sr_dev_inst *sdi;
	int max_probes, i;
	char **triggerlist;

	devices = device_scan();
	if (!devices) {
		g_critical("No devices found.");
		return;
	}
	if (g_slist_length(devices) > 1) {
		g_critical("sigrok-cli only supports one device for capturing.");
		return;
	}
	sdi = devices->data;

	sr_session_new();
	sr_session_datafeed_callback_add(datafeed_in);

	if (sr_session_dev_add(sdi) != SR_OK) {
		g_critical("Failed to use device.");
		sr_session_destroy();
		return;
	}

	if (opt_dev) {
		if ((devargs = parse_generic_arg(opt_dev, FALSE))) {
			if (set_dev_options(sdi, devargs) != SR_OK)
				return;
			g_hash_table_destroy(devargs);
		}
	}

	if (select_probes(sdi) != SR_OK) {
		g_critical("Failed to set probes.");
		sr_session_destroy();
		return;
	}

	if (opt_triggers) {
		if (!(triggerlist = sr_parse_triggerstring(sdi, opt_triggers))) {
			sr_session_destroy();
			return;
		}
		max_probes = g_slist_length(sdi->probes);
		for (i = 0; i < max_probes; i++) {
			if (triggerlist[i]) {
				sr_dev_trigger_set(sdi, i, triggerlist[i]);
				g_free(triggerlist[i]);
			}
		}
		g_free(triggerlist);
	}

	if (opt_continuous) {
		if (!sr_driver_hwcap_exists(sdi->driver, SR_HWCAP_CONTINUOUS)) {
			g_critical("This device does not support continuous sampling.");
			sr_session_destroy();
			return;
		}
	}

	if (opt_time) {
		if (set_limit_time(sdi) != SR_OK) {
			sr_session_destroy();
			return;
		}
	}

	if (opt_samples) {
		if ((sr_parse_sizestring(opt_samples, &limit_samples) != SR_OK)
				|| (sr_dev_config_set(sdi, SR_HWCAP_LIMIT_SAMPLES,
						&limit_samples) != SR_OK)) {
			g_critical("Failed to configure sample limit.");
			sr_session_destroy();
			return;
		}
	}

	if (opt_frames) {
		if ((sr_parse_sizestring(opt_frames, &limit_frames) != SR_OK)
				|| (sr_dev_config_set(sdi, SR_HWCAP_LIMIT_FRAMES,
						&limit_frames) != SR_OK)) {
			g_critical("Failed to configure frame limit.");
			sr_session_destroy();
			return;
		}
	}

	if (sr_session_start() != SR_OK) {
		g_critical("Failed to start session.");
		sr_session_destroy();
		return;
	}

	if (opt_continuous)
		add_anykey();

	sr_session_run();

	if (opt_continuous)
		clear_anykey();

	if (opt_output_file && default_output_format) {
		if (sr_session_save(opt_output_file, sdi, singleds) != SR_OK)
			g_critical("Failed to save session.");
	}
	sr_session_destroy();
	g_slist_free(devices);

}

static void logger(const gchar *log_domain, GLogLevelFlags log_level,
		   const gchar *message, gpointer cb_data)
{
	(void)log_domain;
	(void)cb_data;

	/*
	 * All messages, warnings, errors etc. go to stderr (not stdout) in
	 * order to not mess up the CLI tool data output, e.g. VCD output.
	 */
	if (log_level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING)
			|| opt_loglevel > SR_LOG_WARN) {
		fprintf(stderr, "%s\n", message);
		fflush(stderr);
	}
}

int main(int argc, char **argv)
{
	int ret = 1;
	GOptionContext *context;
	GError *error;

	g_log_set_default_handler(logger, NULL);

	error = NULL;
	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, optargs, NULL);

	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_critical("%s", error->message);
		goto done;
	}

	/* Set the loglevel (amount of messages to output) for libsigrok. */
	if (sr_log_loglevel_set(opt_loglevel) != SR_OK)
		goto done;

	/* Set the loglevel (amount of messages to output) for libsigrokdecode. */
	if (srd_log_loglevel_set(opt_loglevel) != SRD_OK)
		goto done;

	if (sr_init(&sr_ctx) != SR_OK)
		goto done;

	if (opt_pds) {
		if (srd_init(NULL) != SRD_OK)
			goto done;
		if (register_pds(NULL, opt_pds) != 0)
			goto done;
		if (srd_pd_output_callback_add(SRD_OUTPUT_ANN,
				show_pd_annotations, NULL) != SRD_OK)
			goto done;
		if (setup_pd_stack() != 0)
			goto done;
		if (setup_pd_annotations() != 0)
			goto done;
	}

	if (setup_output_format() != 0)
		goto done;

	if (opt_version)
		show_version();
	else if (opt_list_devs)
		show_dev_list();
	else if (opt_pds && opt_show)
		show_pd_detail();
	else if (opt_show)
		show_dev_detail();
	else if (opt_input_file)
		load_input_file();
	else if (opt_samples || opt_time || opt_frames || opt_continuous)
		run_session();
	else
		printf("%s", g_option_context_get_help(context, TRUE, NULL));

	if (opt_pds)
		srd_exit();

	ret = 0;

done:
	if (sr_ctx)
		sr_exit(sr_ctx);

	g_option_context_free(context);

	return ret;
}
