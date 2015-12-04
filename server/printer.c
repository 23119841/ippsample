/*
 * Printer object code for sample IPP server implementation.
 *
 * Copyright 2010-2015 by Apple Inc.
 *
 * These coded instructions, statements, and computer programs are the
 * property of Apple Inc. and are protected by Federal copyright
 * law.  Distribution and use rights are outlined in the file "LICENSE.txt"
 * which should have been included with this file.  If this file is
 * file is missing or damaged, see the license at "http://www.cups.org/".
 *
 * This file is subject to the Apple OS-Developed Software exception.
 */

#include "ippserver.h"


/*
 * Local functions...
 */

static int		compare_active_jobs(server_job_t *a, server_job_t *b);
static int		compare_completed_jobs(server_job_t *a, server_job_t *b);
static int		compare_devices(server_device_t *a, server_device_t *b);
static int		compare_jobs(server_job_t *a, server_job_t *b);
#ifdef HAVE_DNSSD
static void DNSSD_API	dnssd_callback(DNSServiceRef sdRef,
				       DNSServiceFlags flags,
				       DNSServiceErrorType errorCode,
				       const char *name,
				       const char *regtype,
				       const char *domain,
				       server_printer_t *printer);
#elif defined(HAVE_AVAHI)
static void		dnssd_callback(AvahiEntryGroup *p, AvahiEntryGroupState state, void *context);
static void		dnssd_client_cb(AvahiClient *c, AvahiClientState state, void *userdata);
#endif /* HAVE_DNSSD */
static int		register_printer(server_printer_t *printer, const char *location, const char *make, const char *model, const char *formats, const char *adminurl, const char *uuid, int color, int duplex, const char *regtype);


/*
 * 'serverCopyPrinterStateReasons()' - Copy printer-state-reasons values.
 */

void
serverCopyPrinterStateReasons(
    ipp_t          *ipp,		/* I - Attributes */
    ipp_tag_t      group_tag,		/* I - Group */
    server_printer_t *printer)		/* I - Printer */
{
  server_preason_t	creasons = printer->state_reasons | printer->dev_reasons;
					/* Combined reasons */


  if (creasons == SERVER_PREASON_NONE)
  {
    ippAddString(ipp, group_tag, IPP_CONST_TAG(IPP_TAG_KEYWORD), "printer-state-reasons", NULL, "none");
  }
  else
  {
    int			i,		/* Looping var */				num_reasons = 0;/* Number of reasons */
    server_preason_t	reason;		/* Current reason */
    const char		*reasons[32];	/* Reason strings */

    for (i = 0, reason = 1; i < (int)(sizeof(server_preasons) / sizeof(server_preasons[0])); i ++, reason <<= 1)
    {
      if (creasons & reason)
	reasons[num_reasons ++] = server_preasons[i];
    }

    ippAddStrings(ipp, group_tag, IPP_CONST_TAG(IPP_TAG_KEYWORD), "printer-state-reasons", num_reasons, NULL, reasons);
  }
}


/*
 * 'serverCreatePrinter()' - Create, register, and listen for connections to a
 *                      printer object.
 */

server_printer_t *			/* O - Printer */
serverCreatePrinter(
    const char *resource,		/* I - Resource path for URIs */
    const char *name,			/* I - printer-name */
    const char *location,		/* I - printer-location */
    const char *make,			/* I - printer-make-and-model */
    const char *model,			/* I - printer-make-and-model */
    const char *icon,			/* I - printer-icons */
    const char *docformats,		/* I - document-format-supported */
    int        ppm,			/* I - Pages per minute in grayscale */
    int        ppm_color,		/* I - Pages per minute in color (0 for gray) */
    int        duplex,			/* I - 1 = duplex, 0 = simplex */
    int        pin,			/* I - Require PIN printing */
    const char *subtype,		/* I - Bonjour service subtype */
    ipp_t      *attrs,			/* I - Attributes */
    const char *command,		/* I - Command to run, if any */
    const char *proxy_user)		/* I - Proxy account username, if any */
{
  int			i;		/* Looping var */
  server_printer_t	*printer;	/* Printer */
  server_listener_t	*lis;		/* Current listener */
  cups_array_t		*uris;		/* Array of URIs */
  char			uri[1024],	/* Printer URI */
			*uriptr,	/* Current URI */
			**uriptrs,	/* All URIs */
			icons[1024],	/* printer-icons URI */
			adminurl[1024],	/* printer-more-info URI */
			supplyurl[1024],/* printer-supply-info-uri URI */
			device_id[1024],/* printer-device-id */
			make_model[128],/* printer-make-and-model */
			uuid[128];	/* printer-uuid */
  int			num_formats;	/* Number of document-format-supported values */
  char			*defformat,	/* document-format-default value */
			*formats[100],	/* document-format-supported values */
			*ptr;		/* Pointer into string */
  const char		*prefix;	/* Prefix string */
  int			k_supported;	/* Maximum file size supported */
#ifdef HAVE_STATVFS
  struct statvfs	spoolinfo;	/* FS info for spool directory */
  double		spoolsize;	/* FS size */
#elif defined(HAVE_STATFS)
  struct statfs		spoolinfo;	/* FS info for spool directory */
  double		spoolsize;	/* FS size */
#endif /* HAVE_STATVFS */
  ipp_attribute_t	*attr;		/* Attribute */
  static const int	orients[4] =	/* orientation-requested-supported values */
  {
    IPP_ORIENT_PORTRAIT,
    IPP_ORIENT_LANDSCAPE,
    IPP_ORIENT_REVERSE_LANDSCAPE,
    IPP_ORIENT_REVERSE_PORTRAIT
  };
  static const char * const versions[] =/* ipp-versions-supported values */
  {
    "1.0",
    "1.1",
    "2.0"
  };
  static const char * const features[] =/* ipp-features-supported values */
  {
    "document-object",
    "ipp-everywhere",
    "infrastructure-printer",
    "page-overrides"
  };
  static const int	ops[] =		/* operations-supported values */
  {
    IPP_OP_PRINT_JOB,
    IPP_OP_PRINT_URI,
    IPP_OP_VALIDATE_JOB,
    IPP_OP_CREATE_JOB,
    IPP_OP_SEND_DOCUMENT,
    IPP_OP_SEND_URI,
    IPP_OP_CANCEL_JOB,
    IPP_OP_GET_JOB_ATTRIBUTES,
    IPP_OP_GET_JOBS,
    IPP_OP_GET_PRINTER_ATTRIBUTES,
    IPP_OP_GET_PRINTER_SUPPORTED_VALUES,
    IPP_OP_CREATE_PRINTER_SUBSCRIPTIONS,
    IPP_OP_CREATE_JOB_SUBSCRIPTIONS,
    IPP_OP_GET_SUBSCRIPTION_ATTRIBUTES,
    IPP_OP_GET_SUBSCRIPTIONS,
    IPP_OP_RENEW_SUBSCRIPTION,
    IPP_OP_CANCEL_SUBSCRIPTION,
    IPP_OP_GET_NOTIFICATIONS,
    IPP_OP_GET_DOCUMENT_ATTRIBUTES,
    IPP_OP_GET_DOCUMENTS,
    IPP_OP_CANCEL_MY_JOBS,
    IPP_OP_CLOSE_JOB,
    IPP_OP_IDENTIFY_PRINTER,
    IPP_OP_VALIDATE_DOCUMENT,
    IPP_OP_ACKNOWLEDGE_DOCUMENT,
    IPP_OP_ACKNOWLEDGE_IDENTIFY_PRINTER,
    IPP_OP_ACKNOWLEDGE_JOB,
    IPP_OP_FETCH_DOCUMENT,
    IPP_OP_FETCH_JOB,
    IPP_OP_GET_OUTPUT_DEVICE_ATTRIBUTES,
    IPP_OP_UPDATE_ACTIVE_JOBS,
    IPP_OP_UPDATE_DOCUMENT_STATUS,
    IPP_OP_UPDATE_JOB_STATUS,
    IPP_OP_UPDATE_OUTPUT_DEVICE_ATTRIBUTES,
    IPP_OP_DEREGISTER_OUTPUT_DEVICE
  };
  static const char * const charsets[] =/* charset-supported values */
  {
    "us-ascii",
    "utf-8"
  };
  static const char * const compressions[] =/* compression-supported values */
  {
#ifdef HAVE_LIBZ
    "deflate",
    "gzip",
#endif /* HAVE_LIBZ */
    "none"
  };
  static const char * const identify_actions[] =
  {
    "display",
    "sound"
  };
  static const char * const job_creation[] =
  {					/* job-creation-attributes-supported values */
    "copies",
    "ipp-attribute-fidelity",
    "job-account-id",
    "job-accounting-user-id",
    "job-name",
    "job-password",
    "job-priority",
    "media",
    "media-col",
    "multiple-document-handling",
    "orientation-requested",
    "print-quality",
    "sides"
  };
  static const char * const media_col_supported[] =
  {					/* media-col-supported values */
    "media-bottom-margin",
    "media-left-margin",
    "media-right-margin",
    "media-size",
    "media-source",
    "media-top-margin",
    "media-type"
  };
  static const char * const multiple_document_handling[] =
  {					/* multiple-document-handling-supported values */
    "separate-documents-uncollated-copies",
    "separate-documents-collated-copies"
  };
  static const char * const notify_attributes[] =
  {					/* notify-attributes-supported */
    "printer-state-change-time",
    "notify-lease-expiration-time",
    "notify-subscriber-user-name"
  };
  static const char * const overrides[] =
  {					/* overrides-supported */
    "document-number",
    "pages"
  };
  static const char * const print_color_mode_supported[] =
  {					/* print-color-mode-supported values */
    "auto",
    "color",
    "monochrome"
  };
  static const int	print_quality_supported[] =
  {					/* print-quality-supported values */
    IPP_QUALITY_DRAFT,
    IPP_QUALITY_NORMAL,
    IPP_QUALITY_HIGH
  };
  static const int	pwg_raster_document_resolution_supported[] =
  {
    150,
    300
  };
  static const char * const pwg_raster_document_type_supported[] =
  {
    "black_1",
    "cmyk_8",
    "sgray_8",
    "srgb_8",
    "srgb_16"
  };
  static const char * const reference_uri_schemes_supported[] =
  {					/* reference-uri-schemes-supported */
    "file",
    "ftp",
    "http"
#ifdef HAVE_SSL
    , "https"
#endif /* HAVE_SSL */
  };
  static const char * const sides_supported[] =
  {					/* sides-supported values */
    "one-sided",
    "two-sided-long-edge",
    "two-sided-short-edge"
  };
  static const char * const urf_supported[] =
  {					/* urf-supported values */
    "CP1",
    "IS1-5-7",
    "MT1-2-3-4-5-6-8-9-10-11-12-13",
    "RS300",
    "SRGB24",
    "V1.4",
    "W8",
    "DM1"
  };
  static const char * const which_jobs[] =
  {					/* which-jobs-supported values */
    "completed",
    "not-completed",
    "aborted",
    "all",
    "canceled",
    "pending",
    "pending-held",
    "processing",
    "processing-stopped"
  };


 /*
  * Allocate memory for the printer...
  */

  if ((printer = calloc(1, sizeof(server_printer_t))) == NULL)
  {
    perror("ippserver: Unable to allocate memory for printer");
    return (NULL);
  }

  printer->resource       = strdup(resource);
  printer->name           = strdup(name);
  printer->dnssd_name     = strdup(printer->name);
  printer->start_time     = time(NULL);
  printer->config_time    = printer->start_time;
  printer->state          = IPP_PSTATE_IDLE;
  printer->state_reasons  = SERVER_PREASON_NONE;
  printer->state_time     = printer->start_time;
  printer->jobs           = cupsArrayNew3((cups_array_func_t)compare_jobs, NULL, NULL, 0, NULL, (cups_afree_func_t)serverDeleteJob);
  printer->active_jobs    = cupsArrayNew((cups_array_func_t)compare_active_jobs, NULL);
  printer->completed_jobs = cupsArrayNew((cups_array_func_t)compare_completed_jobs, NULL);
  printer->next_job_id    = 1;

  uris = cupsArrayNew3((cups_array_func_t)strcmp, NULL, NULL, 0, (cups_acopy_func_t)strdup, (cups_afree_func_t)free);
  for (lis = cupsArrayFirst(Listeners); lis; lis = cupsArrayNext(Listeners))
  {
    httpAssembleURI(HTTP_URI_CODING_ALL, uri, sizeof(uri), SERVER_IPP_SCHEME, NULL, lis->host, lis->port, resource);

    if (!cupsArrayFind(uris, uri))
      cupsArrayAdd(uris, uri);
  }

  uriptrs = calloc(cupsArrayCount(uris), sizeof(char *));
  for (i = 0, uriptr = cupsArrayFirst(uris); uriptr; i ++, uriptr = cupsArrayNext(uris))
    uriptrs[i] = uriptr;

  if (icon)
    printer->icon = strdup(icon);

  if (command)
    printer->command = strdup(command);

  if (proxy_user)
    printer->proxy_user = strdup(proxy_user);

  printer->devices = cupsArrayNew((cups_array_func_t)compare_devices, NULL);

  _cupsRWInit(&(printer->rwlock));

 /*
  * Prepare values for the printer attributes...
  */

  lis = cupsArrayFirst(Listeners);

  httpAssembleURIf(HTTP_URI_CODING_ALL, icons, sizeof(icons), SERVER_WEB_SCHEME, NULL, lis->host, lis->port, "%s/icon.png", resource + 4);
  httpAssembleURIf(HTTP_URI_CODING_ALL, adminurl, sizeof(adminurl), SERVER_WEB_SCHEME, NULL, lis->host, lis->port, "%s/", resource + 4);
  httpAssembleURIf(HTTP_URI_CODING_ALL, supplyurl, sizeof(supplyurl), SERVER_WEB_SCHEME, NULL, lis->host, lis->port, "%s/supplies", resource + 4);

  serverLogPrinter(SERVER_LOGLEVEL_DEBUG, printer, "printer-more-info=\"%s\"", adminurl);
  serverLogPrinter(SERVER_LOGLEVEL_DEBUG, printer, "printer-supply-info-uri=\"%s\"", supplyurl);
  serverLogPrinter(SERVER_LOGLEVEL_DEBUG, printer, "printer-uri=\"%s\"", cupsArrayFirst(uris));

  snprintf(make_model, sizeof(make_model), "%s %s", make, model);

  num_formats = 1;
  formats[0]  = strdup(docformats);
  defformat   = formats[0];
  for (ptr = strchr(formats[0], ','); ptr; ptr = strchr(ptr, ','))
  {
    *ptr++ = '\0';
    formats[num_formats++] = ptr;

    if (!strcasecmp(ptr, "application/octet-stream"))
      defformat = ptr;
  }

  snprintf(device_id, sizeof(device_id), "MFG:%s;MDL:%s;", make, model);
  ptr    = device_id + strlen(device_id);
  prefix = "CMD:";
  for (i = 0; i < num_formats; i ++)
  {
    if (!strcasecmp(formats[i], "application/pdf"))
      snprintf(ptr, sizeof(device_id) - (size_t)(ptr - device_id), "%sPDF", prefix);
    else if (!strcasecmp(formats[i], "application/postscript"))
      snprintf(ptr, sizeof(device_id) - (size_t)(ptr - device_id), "%sPS", prefix);
    else if (!strcasecmp(formats[i], "application/vnd.hp-PCL"))
      snprintf(ptr, sizeof(device_id) - (size_t)(ptr - device_id), "%sPCL", prefix);
    else if (!strcasecmp(formats[i], "image/jpeg"))
      snprintf(ptr, sizeof(device_id) - (size_t)(ptr - device_id), "%sJPEG", prefix);
    else if (!strcasecmp(formats[i], "image/png"))
      snprintf(ptr, sizeof(device_id) - (size_t)(ptr - device_id), "%sPNG", prefix);
    else if (strcasecmp(formats[i], "application/octet-stream"))
      snprintf(ptr, sizeof(device_id) - (size_t)(ptr - device_id), "%s%s", prefix, formats[i]);

    ptr += strlen(ptr);
    prefix = ",";
  }
  if (ptr < (device_id + sizeof(device_id) - 1))
  {
    *ptr++ = ';';
    *ptr = '\0';
  }

 /*
  * Get the maximum spool size based on the size of the filesystem used for
  * the spool directory.  If the host OS doesn't support the statfs call
  * or the filesystem is larger than 2TiB, always report INT_MAX.
  */

#ifdef HAVE_STATVFS
  if (statvfs(SpoolDirectory, &spoolinfo))
    k_supported = INT_MAX;
  else if ((spoolsize = (double)spoolinfo.f_frsize *
                        spoolinfo.f_blocks / 1024) > INT_MAX)
    k_supported = INT_MAX;
  else
    k_supported = (int)spoolsize;

#elif defined(HAVE_STATFS)
  if (statfs(SpoolDirectory, &spoolinfo))
    k_supported = INT_MAX;
  else if ((spoolsize = (double)spoolinfo.f_bsize *
                        spoolinfo.f_blocks / 1024) > INT_MAX)
    k_supported = INT_MAX;
  else
    k_supported = (int)spoolsize;

#else
  k_supported = INT_MAX;
#endif /* HAVE_STATVFS */

 /*
  * Create the printer attributes.  This list of attributes is sorted to improve
  * performance when the client provides a requested-attributes attribute...
  */

  if (attrs)
    printer->attrs = attrs;
  else
    printer->attrs = ippNew();

  /* charset-configured */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_CHARSET), "charset-configured", NULL, "utf-8");

  /* charset-supported */
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_CHARSET), "charset-supported", sizeof(charsets) / sizeof(charsets[0]), NULL, charsets);

  /* color-supported */
  if (!ippFindAttribute(printer->attrs, "color-supported", IPP_TAG_ZERO))
    ippAddBoolean(printer->attrs, IPP_TAG_PRINTER, "color-supported", ppm_color > 0);

  /* compression-supported */
  if (!ippFindAttribute(printer->attrs, "compression-supported", IPP_TAG_ZERO))
    ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "compression-supported", (int)(sizeof(compressions) / sizeof(compressions[0])), NULL, compressions);

  /* copies-default */
  if (!ippFindAttribute(printer->attrs, "copies-default", IPP_TAG_ZERO))
    ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "copies-default", 1);

  /* copies-supported */
  if (!ippFindAttribute(printer->attrs, "copies-supported", IPP_TAG_ZERO))
    ippAddRange(printer->attrs, IPP_TAG_PRINTER, "copies-supported", 1, 999);

  /* document-format-default */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_MIMETYPE,
               "document-format-default", NULL, defformat);

  /* document-format-supported */
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_MIMETYPE,
                "document-format-supported", num_formats, NULL,
		(const char * const *)formats);

  /* document-password-supported */
  if (!ippFindAttribute(printer->attrs, "document-password-supported", IPP_TAG_ZERO))
    ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "document-password-supported", 127);

  /* finishings-default */
  if (!ippFindAttribute(printer->attrs, "finishings-default", IPP_TAG_ZERO))
    ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_ENUM, "finishings-default", IPP_FINISHINGS_NONE);

  /* finishings-supported */
  if (!ippFindAttribute(printer->attrs, "finishings-supported", IPP_TAG_ZERO))
    ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_ENUM, "finishings-supported", IPP_FINISHINGS_NONE);

  /* generated-natural-language-supported */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_LANGUAGE), "generated-natural-language-supported", NULL, "en");

  /* identify-actions-default */
  if (!ippFindAttribute(printer->attrs, "identify-actions-default", IPP_TAG_ZERO))
    ippAddString (printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "identify-actions-default", NULL, "sound");

  /* identify-actions-supported */
  if (!ippFindAttribute(printer->attrs, "identify-actions-supported", IPP_TAG_ZERO))
    ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "identify-actions-supported", sizeof(identify_actions) / sizeof(identify_actions[0]), NULL, identify_actions);

  /* ipp-features-supported */
  if (!ippFindAttribute(printer->attrs, "ipp-features-supported", IPP_TAG_ZERO))
    ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "ipp-features-supported", sizeof(features) / sizeof(features[0]), NULL, features);

  /* ipp-versions-supported */
  if (!ippFindAttribute(printer->attrs, "ipp-versions-supported", IPP_TAG_ZERO))
    ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "ipp-versions-supported", sizeof(versions) / sizeof(versions[0]), NULL, versions);

  /* ippget-event-life */
  ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "ippget-event-life", 300);

  /* job-account-id-default */
  if (!ippFindAttribute(printer->attrs, "job-account-id-default", IPP_TAG_ZERO))
    ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_NAME), "job-account-id-default", NULL, "");

  /* job-account-id-supported */
  if (!ippFindAttribute(printer->attrs, "job-account-id-supported", IPP_TAG_ZERO))
    ippAddBoolean(printer->attrs, IPP_TAG_PRINTER, "job-account-id-supported", 1);

  /* job-accounting-user-id-default */
  if (!ippFindAttribute(printer->attrs, "job-accounting-user-id-default", IPP_TAG_ZERO))
    ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_NAME), "job-accounting-user-id-default", NULL, "");

  /* job-accounting-user-id-supported */
  if (!ippFindAttribute(printer->attrs, "job-accounting-user-id-supported", IPP_TAG_ZERO))
    ippAddBoolean(printer->attrs, IPP_TAG_PRINTER, "job-accounting-user-id-supported", 1);

  /* job-creation-attributes-supported */
  if (!ippFindAttribute(printer->attrs, "job-creation-attributes-supported", IPP_TAG_ZERO))
    ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "job-creation-attributes-supported", sizeof(job_creation) / sizeof(job_creation[0]), NULL, job_creation);

  /* job-ids-supported */
  ippAddBoolean(printer->attrs, IPP_TAG_PRINTER, "job-ids-supported", 1);

  /* job-k-octets-supported */
  ippAddRange(printer->attrs, IPP_TAG_PRINTER, "job-k-octets-supported", 0,
	      k_supported);

  /* job-password-supported */
  if (!ippFindAttribute(printer->attrs, "job-password-supported", IPP_TAG_ZERO))
    ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "job-password-supported", 4);

  /* job-priority-default */
  if (!ippFindAttribute(printer->attrs, "job-priority-default", IPP_TAG_ZERO))
    ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "job-priority-default", 50);

  /* job-priority-supported */
  if (!ippFindAttribute(printer->attrs, "job-priority-supported", IPP_TAG_ZERO))
    ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "job-priority-supported", 100);

  /* job-sheets-default */
  if (!ippFindAttribute(printer->attrs, "job-sheets-default", IPP_TAG_ZERO))
    ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_NAME), "job-sheets-default", NULL, "none");

  /* job-sheets-supported */
  if (!ippFindAttribute(printer->attrs, "job-sheets-supported", IPP_TAG_ZERO))
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_NAME), "job-sheets-supported", NULL, "none");

#if 0
  /* media-bottom-margin-supported */
  if (!ippFindAttribute(printer->attrs, "media-bottom-margin-supported", IPP_TAG_ZERO))
    ippAddIntegers(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "media-bottom-margin-supported", (int)(sizeof(media_xxx_margin_supported) / sizeof(media_xxx_margin_supported[0])), media_xxx_margin_supported);

  /* media-col-database */
  if (!ippFindAttribute(printer->attrs, "media-col-database", IPP_TAG_ZERO))
  {
    for (num_database = 0, i = 0;
	 i < (int)(sizeof(media_col_sizes) / sizeof(media_col_sizes[0]));
	 i ++)
    {
      if (media_col_sizes[i][2] == _IPP_ENV_ONLY)
	num_database += 3;		/* auto + manual + envelope */
      else if (media_col_sizes[i][2] == _IPP_PHOTO_ONLY)
	num_database += 6 * 3;		/* auto + photographic-* from auto, manual, and photo */
      else
	num_database += 2;		/* Regular + borderless */
    }

    media_col_database = ippAddCollections(printer->attrs, IPP_TAG_PRINTER, "media-col-database", num_database, NULL);
    for (media_col_index = 0, i = 0;
	 i < (int)(sizeof(media_col_sizes) / sizeof(media_col_sizes[0]));
	 i ++)
    {
      switch (media_col_sizes[i][2])
      {
	case _IPP_GENERAL :
	   /*
	    * Regular + borderless for the general class; no source/type
	    * selectors...
	    */

	    ippSetCollection(printer->attrs, &media_col_database, media_col_index ++, create_media_col(media_supported[i], NULL, NULL, media_col_sizes[i][0], media_col_sizes[i][1], media_xxx_margin_supported[1]));
	    ippSetCollection(printer->attrs, &media_col_database, media_col_index ++, create_media_col(media_supported[i], NULL, NULL, media_col_sizes[i][0], media_col_sizes[i][1], media_xxx_margin_supported[0]));
	    break;

	case _IPP_ENV_ONLY :
	   /*
	    * Regular margins for "auto", "manual", and "envelope" sources.
	    */

	    ippSetCollection(printer->attrs, &media_col_database, media_col_index ++, create_media_col(media_supported[i], "auto", "envelope", media_col_sizes[i][0], media_col_sizes[i][1], media_xxx_margin_supported[1]));
	    ippSetCollection(printer->attrs, &media_col_database, media_col_index ++, create_media_col(media_supported[i], "manual", "envelope", media_col_sizes[i][0], media_col_sizes[i][1], media_xxx_margin_supported[1]));
	    ippSetCollection(printer->attrs, &media_col_database, media_col_index ++, create_media_col(media_supported[i], "envelope", "envelope", media_col_sizes[i][0], media_col_sizes[i][1], media_xxx_margin_supported[1]));
	    break;
	case _IPP_PHOTO_ONLY :
	   /*
	    * Photos have specific media types and can only be printed via
	    * the auto, manual, and photo sources...
	    */

	    for (j = 0;
		 j < (int)(sizeof(media_type_supported) /
			   sizeof(media_type_supported[0]));
		 j ++)
	    {
	      if (strcmp(media_type_supported[j], "auto") && strncmp(media_type_supported[j], "photographic-", 13))
		continue;

	      ippSetCollection(printer->attrs, &media_col_database, media_col_index ++, create_media_col(media_supported[i], "auto", media_type_supported[j], media_col_sizes[i][0], media_col_sizes[i][1], media_xxx_margin_supported[0]));
	      ippSetCollection(printer->attrs, &media_col_database, media_col_index ++, create_media_col(media_supported[i], "manual", media_type_supported[j], media_col_sizes[i][0], media_col_sizes[i][1], media_xxx_margin_supported[0]));
	      ippSetCollection(printer->attrs, &media_col_database, media_col_index ++, create_media_col(media_supported[i], "photo", media_type_supported[j], media_col_sizes[i][0], media_col_sizes[i][1], media_xxx_margin_supported[0]));
	    }
	    break;
      }
    }
  }

  /* media-col-default */
  if (!ippFindAttribute(printer->attrs, "media-col-default", IPP_TAG_ZERO))
  {
    media_col_default = create_media_col(media_supported[0], media_source_supported[0], media_type_supported[0], media_col_sizes[0][0], media_col_sizes[0][1],media_xxx_margin_supported[1]);

    ippAddCollection(printer->attrs, IPP_TAG_PRINTER, "media-col-default",
		     media_col_default);
    ippDelete(media_col_default);
  }

  /* media-default */
  if (!ippFindAttribute(printer->attrs, "media-default", IPP_TAG_ZERO))
    ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "media-default", NULL, media_supported[0]);

  /* media-left-margin-supported */
  if (!ippFindAttribute(printer->attrs, "media-left-margin-supported", IPP_TAG_ZERO))
    ippAddIntegers(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "media-left-margin-supported", (int)(sizeof(media_xxx_margin_supported) / sizeof(media_xxx_margin_supported[0])), media_xxx_margin_supported);

  /* media-right-margin-supported */
  if (!ippFindAttribute(printer->attrs, "media-right-margin-supported", IPP_TAG_ZERO))
    ippAddIntegers(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "media-right-margin-supported", (int)(sizeof(media_xxx_margin_supported) / sizeof(media_xxx_margin_supported[0])), media_xxx_margin_supported);

  /* media-supported */
  if (!ippFindAttribute(printer->attrs, "media-supported", IPP_TAG_ZERO))
    ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "media-supported", (int)(sizeof(media_supported) / sizeof(media_supported[0])), NULL, media_supported);

  /* media-size-supported */
  if (!ippFindAttribute(printer->attrs, "media-size-supported", IPP_TAG_ZERO))
  {
    media_size_supported = ippAddCollections(printer->attrs, IPP_TAG_PRINTER, "media-size-supported", (int)(sizeof(media_col_sizes) / sizeof(media_col_sizes[0])), NULL);

    for (i = 0;
	 i < (int)(sizeof(media_col_sizes) / sizeof(media_col_sizes[0]));
	 i ++)
    {
      ipp_t *size = create_media_size(media_col_sizes[i][0], media_col_sizes[i][1]);

      ippSetCollection(printer->attrs, &media_size_supported, i, size);
      ippDelete(size);
    }
  }

  /* media-source-supported */
  if (!ippFindAttribute(printer->attrs, "media-source-supported", IPP_TAG_ZERO))
    ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "media-source-supported", (int)(sizeof(media_source_supported) / sizeof(media_source_supported[0])), NULL, media_source_supported);

  /* media-top-margin-supported */
  if (!ippFindAttribute(printer->attrs, "media-top-margin-supported", IPP_TAG_ZERO))
    ippAddIntegers(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "media-top-margin-supported", (int)(sizeof(media_xxx_margin_supported) / sizeof(media_xxx_margin_supported[0])), media_xxx_margin_supported);

  /* media-type-supported */
  if (!ippFindAttribute(printer->attrs, "media-type-supported", IPP_TAG_ZERO))
    ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "media-type-supported", (int)(sizeof(media_type_supported) / sizeof(media_type_supported[0])), NULL, media_type_supported);
#endif // 0

  /* media-col-supported */
  if (!ippFindAttribute(printer->attrs, "media-col-supported", IPP_TAG_ZERO))
    ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "media-col-supported", (int)(sizeof(media_col_supported) / sizeof(media_col_supported[0])), NULL, media_col_supported);

  /* multiple-document-handling-supported */
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "multiple-document-handling-supported", sizeof(multiple_document_handling) / sizeof(multiple_document_handling[0]), NULL, multiple_document_handling);

  /* multiple-document-jobs-supported */
  ippAddBoolean(printer->attrs, IPP_TAG_PRINTER, "multiple-document-jobs-supported", 0);

  /* multiple-operation-time-out */
  ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "multiple-operation-time-out", 60);

  /* multiple-operation-time-out-action */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "multiple-operation-time-out-action", NULL, "abort-job");

  /* natural-language-configured */
  ippAddString(printer->attrs, IPP_TAG_PRINTER,
               IPP_CONST_TAG(IPP_TAG_LANGUAGE),
               "natural-language-configured", NULL, "en");

  /* notify-attributes-supported */
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "notify-attributes-supported", sizeof(notify_attributes) / sizeof(notify_attributes[0]), NULL, notify_attributes);

  /* notify-events-default */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "notify-events-default", NULL, "job-completed");

  /* notify-events-supported */
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "notify-events-supported", sizeof(server_events) / sizeof(server_events[0]), NULL, server_events);

  /* notify-lease-duration-default */
  ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "notify-lease-duration-default", 86400);

  /* notify-lease-duration-supported */
  ippAddRange(printer->attrs, IPP_TAG_PRINTER, "notify-lease-duration-supported", 0, SERVER_NOTIFY_LEASE_DURATION_MAX);

  /* notify-max-events-supported */
  ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "notify-lease-duration-default", (int)(sizeof(server_events) / sizeof(server_events[0])));

  /* notify-pull-method-supported */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "notify-pull-method-supported", NULL, "ippget");

  /* number-up-default */
  if (!ippFindAttribute(printer->attrs, "number-up-default", IPP_TAG_ZERO))
    ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "number-up-default", 1);

  /* number-up-supported */
  if (!ippFindAttribute(printer->attrs, "number-up-supported", IPP_TAG_ZERO))
    ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "number-up-supported", 1);

  /* operations-supported */
  ippAddIntegers(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_ENUM, "operations-supported", sizeof(ops) / sizeof(ops[0]), ops);

  /* orientation-requested-default */
  if (!ippFindAttribute(printer->attrs, "orientation-requested-default", IPP_TAG_ZERO))
    ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_NOVALUE, "orientation-requested-default", 0);

  /* orientation-requested-supported */
  if (!ippFindAttribute(printer->attrs, "orientation-requested-supported", IPP_TAG_ZERO))
    ippAddIntegers(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_ENUM, "orientation-requested-supported", 4, orients);

  /* output-bin-default */
  if (!ippFindAttribute(printer->attrs, "output-bin-default", IPP_TAG_ZERO))
    ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "output-bin-default", NULL, "face-down");

  /* output-bin-supported */
  if (!ippFindAttribute(printer->attrs, "output-bin-supported", IPP_TAG_ZERO))
    ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "output-bin-supported", NULL, "face-down");

  /* overrides-supported */
  if (!ippFindAttribute(printer->attrs, "overrides-supported", IPP_TAG_ZERO))
    ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "overrides-supported", (int)(sizeof(overrides) / sizeof(overrides[0])), NULL, overrides);

  /* page-ranges-supported */
  if (!ippFindAttribute(printer->attrs, "page-ranges-supported", IPP_TAG_ZERO))
    ippAddBoolean(printer->attrs, IPP_TAG_PRINTER, "page-ranges-supported", 1);

  /* pages-per-minute */
  ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "pages-per-minute", ppm);

  /* pages-per-minute-color */
  if (ppm_color > 0)
    ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                  "pages-per-minute-color", ppm_color);

  /* pdl-override-supported */
  if (!ippFindAttribute(printer->attrs, "pdl-override-supported", IPP_TAG_ZERO))
    ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "pdl-override-supported", NULL, "attempted");

  /* preferred-attributes-supported */
  ippAddBoolean(printer->attrs, IPP_TAG_PRINTER, "preferred-attributes-supported", 0);

  /* print-color-mode-default */
  if (!ippFindAttribute(printer->attrs, "print-color-mode-default", IPP_TAG_ZERO))
    ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "print-color-mode-default", NULL, "auto");

  /* print-color-mode-supported */
  if (!ippFindAttribute(printer->attrs, "print-color-mode-supported", IPP_TAG_ZERO))
    ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "print-color-mode-supported", (int)(sizeof(print_color_mode_supported) / sizeof(print_color_mode_supported[0])), NULL, print_color_mode_supported);

  /* print-content-optimize-default */
  if (!ippFindAttribute(printer->attrs, "print-content-optimize-default", IPP_TAG_ZERO))
    ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "print-content-optimize-default", NULL, "auto");

  /* print-content-optimize-supported */
  if (!ippFindAttribute(printer->attrs, "print-content-optimize-supported", IPP_TAG_ZERO))
    ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "print-content-optimize-supported", NULL, "auto");

  /* print-rendering-intent-default */
  if (!ippFindAttribute(printer->attrs, "print-rendering-intent-default", IPP_TAG_ZERO))
    ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "print-rendering-intent-default", NULL, "auto");

  /* print-rendering-intent-supported */
  if (!ippFindAttribute(printer->attrs, "print-rendering-intent-supported", IPP_TAG_ZERO))
    ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "print-rendering-intent-supported", NULL, "auto");

  /* print-quality-default */
  if (!ippFindAttribute(printer->attrs, "print-quality-default", IPP_TAG_ZERO))
    ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_ENUM, "print-quality-default", IPP_QUALITY_NORMAL);

  /* print-quality-supported */
  if (!ippFindAttribute(printer->attrs, "print-quality-supported", IPP_TAG_ZERO))
    ippAddIntegers(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_ENUM, "print-quality-supported", (int)(sizeof(print_quality_supported) / sizeof(print_quality_supported[0])), print_quality_supported);

  /* printer-device-id */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_TEXT,
	       "printer-device-id", NULL, device_id);

  /* printer-get-attributes-supported */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "printer-get-attributes-supported", NULL, "document-format");

  /* printer-geo-location */
  if (!ippFindAttribute(printer->attrs, "printer-geo-location", IPP_TAG_ZERO))
    ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_UNKNOWN, "printer-geo-location", 0);

  /* printer-icons */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_URI,
               "printer-icons", NULL, icons);

  /* printer-is-accepting-jobs */
  ippAddBoolean(printer->attrs, IPP_TAG_PRINTER, "printer-is-accepting-jobs", 1);

  /* printer-info */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-info", NULL, name);

  /* printer-location */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_TEXT,
               "printer-location", NULL, location);

  /* printer-make-and-model */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_TEXT,
               "printer-make-and-model", NULL, make_model);

  /* printer-mandatory-job-attributes */
  if (pin && !ippFindAttribute(printer->attrs, "printer-mandatory-job-attributes", IPP_TAG_ZERO))
  {
    static const char * const names[] =
    {
      "job-account-id",
      "job-accounting-user-id",
      "job-password"
    };

    ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD,
                  "printer-mandatory-job-attributes",
                  (int)(sizeof(names) / sizeof(names[0])), NULL, names);
  }

  /* printer-more-info */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_URI, "printer-more-info", NULL, adminurl);

  /* printer-name */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_NAME, "printer-name", NULL, name);

  /* printer-organization */
  if (!ippFindAttribute(printer->attrs, "printer-organization", IPP_TAG_ZERO))
    ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_TEXT), "printer-organization", NULL, "Apple Inc.");

  /* printer-organizational-unit */
  if (!ippFindAttribute(printer->attrs, "printer-organizational-unit", IPP_TAG_ZERO))
    ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_TEXT), "printer-organizational-unit", NULL, "Printing Engineering");

  /* printer-resolution-default */
  if (!ippFindAttribute(printer->attrs, "printer-resolution-default", IPP_TAG_ZERO))
    ippAddResolution(printer->attrs, IPP_TAG_PRINTER, "printer-resolution-default", IPP_RES_PER_INCH, 600, 600);

  /* printer-resolution-supported */
  if (!ippFindAttribute(printer->attrs, "printer-resolutions-supported", IPP_TAG_ZERO))
    ippAddResolution(printer->attrs, IPP_TAG_PRINTER, "printer-resolution-supported", IPP_RES_PER_INCH, 600, 600);

//  /* printer-supply-description */
//  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_TEXT), "printer-supply-description", (int)(sizeof(printer_supplies) / sizeof(printer_supplies[0])), NULL, printer_supplies);

  /* printer-supply-info-uri */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_URI, "printer-supply-info-uri", NULL, supplyurl);

  /* printer-uri-supported */
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_URI, "printer-uri-supported", cupsArrayCount(uris), NULL, (const char **)uriptrs);

  /* printer-uuid */
  httpAssembleUUID(lis->host, lis->port, name, 0, uuid, sizeof(uuid));
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_URI, "printer-uuid", NULL, uuid);

  /* pwg-raster-document-xxx-supported */
  for (i = 0; i < num_formats; i ++)
    if (!strcasecmp(formats[i], "image/pwg-raster"))
      break;

  if (i < num_formats)
  {
    if (!ippFindAttribute(printer->attrs, "pwg-raster-document-resolution-supported", IPP_TAG_ZERO))
      ippAddResolutions(printer->attrs, IPP_TAG_PRINTER, "pwg-raster-document-resolution-supported", (int)(sizeof(pwg_raster_document_resolution_supported) / sizeof(pwg_raster_document_resolution_supported[0])), IPP_RES_PER_INCH, pwg_raster_document_resolution_supported, pwg_raster_document_resolution_supported);
    if (!ippFindAttribute(printer->attrs, "pwg-raster-document-sheet-back", IPP_TAG_ZERO))
      ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "pwg-raster-document-sheet-back", NULL, "normal");
    if (!ippFindAttribute(printer->attrs, "pwg-raster-document-type-supported", IPP_TAG_ZERO))
      ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "pwg-raster-document-type-supported", (int)(sizeof(pwg_raster_document_type_supported) / sizeof(pwg_raster_document_type_supported[0])), NULL, pwg_raster_document_type_supported);
  }

  /* reference-uri-scheme-supported */
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_URISCHEME), "reference-uri-schemes-supported", (int)(sizeof(reference_uri_schemes_supported) / sizeof(reference_uri_schemes_supported[0])), NULL, reference_uri_schemes_supported);

  /* sides-default */
  if (!ippFindAttribute(printer->attrs, "sides-default", IPP_TAG_ZERO))
    ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "sides-default", NULL, "one-sided");

  /* sides-supported */
  if (!ippFindAttribute(printer->attrs, "sides-supported", IPP_TAG_ZERO))
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "sides-supported", duplex ? 3 : 1, NULL, sides_supported);

  /* urf-supported */
  for (i = 0; i < num_formats; i ++)
    if (!strcasecmp(formats[i], "image/urf"))
      break;

  if (i < num_formats && !ippFindAttribute(printer->attrs, "urf-supported", IPP_TAG_ZERO))
    ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "urf-supported", (int)(sizeof(urf_supported) / sizeof(urf_supported[0])) - !duplex, NULL, urf_supported);

  /* uri-authentication-supported */
  attr = ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "uri-authentication-supported", NULL, proxy_user ? "basic"  : "none");
  for (i = 1; i < cupsArrayCount(uris); i ++)
    ippSetString(printer->attrs, &attr, i, proxy_user ? "basic"  : "none");

  /* uri-security-supported */
#ifdef HAVE_SSL
  attr = ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "uri-security-supported", NULL, "tls");
  for (i = 1; i < cupsArrayCount(uris); i ++)
    ippSetString(printer->attrs, &attr, i, "tls");

#else
  attr = ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "uri-security-supported", NULL, "none");
  for (i = 1; i < cupsArrayCount(uris); i ++)
    ippSetString(printer->attrs, &attr, i, "none");
#endif /* HAVE_SSL */

  /* which-jobs-supported */
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "which-jobs-supported", sizeof(which_jobs) / sizeof(which_jobs[0]), NULL, which_jobs);

  free(formats[0]);

  serverLogAttributes("Printer", printer->attrs, 0);

 /*
  * Register the printer with Bonjour...
  */

  if (!register_printer(printer, location, make, model, docformats, adminurl, uuid + 9, ppm_color > 0, duplex, subtype))
    goto bad_printer;

 /*
  * Return it!
  */

  return (printer);


 /*
  * If we get here we were unable to create the printer...
  */

  bad_printer:

  serverDeletePrinter(printer);
  return (NULL);
}


/*
 * 'serverDeletePrinter()' - Unregister, close listen sockets, and free all memory
 *                      used by a printer object.
 */

void
serverDeletePrinter(server_printer_t *printer)	/* I - Printer */
{
  _cupsRWLockWrite(&printer->rwlock);

#if HAVE_DNSSD
  if (printer->printer_ref)
    DNSServiceRefDeallocate(printer->printer_ref);
  if (printer->ipp_ref)
    DNSServiceRefDeallocate(printer->ipp_ref);
  if (printer->http_ref)
    DNSServiceRefDeallocate(printer->http_ref);
#elif defined(HAVE_AVAHI)
  avahi_threaded_poll_lock(DNSSDMaster);

  if (printer->printer_ref)
    avahi_entry_group_free(printer->printer_ref);
  if (printer->ipp_ref)
    avahi_entry_group_free(printer->ipp_ref);
  if (printer->http_ref)
    avahi_entry_group_free(printer->http_ref);

  avahi_threaded_poll_unlock(DNSSDMaster);
#endif /* HAVE_DNSSD */

  if (printer->resource)
    free(printer->resource);
  if (printer->dnssd_name)
    free(printer->dnssd_name);
  if (printer->name)
    free(printer->name);
  if (printer->icon)
    free(printer->icon);
  if (printer->proxy_user)
    free(printer->proxy_user);

  ippDelete(printer->attrs);
  ippDelete(printer->dev_attrs);

  cupsArrayDelete(printer->active_jobs);
  cupsArrayDelete(printer->completed_jobs);
  cupsArrayDelete(printer->jobs);
  cupsArrayDelete(printer->subscriptions);

  _cupsRWDeinit(&printer->rwlock);

  free(printer);
}


/*
 * 'serverGetPrinterStateReasonsBits()' - Get the bits associated with "printer-state-reasons" values.
 */

server_preason_t			/* O - Bits */
serverGetPrinterStateReasonsBits(
    ipp_attribute_t *attr)		/* I - "printer-state-reasons" bits */
{
  int			i, j,		/* Looping vars */
			count;		/* Number of "printer-state-reasons" values */
  const char		*keyword;	/* "printer-state-reasons" value */
  server_preason_t	preasons = SERVER_PREASON_NONE;
					/* Bits for "printer-state-reasons" values */


  count = ippGetCount(attr);
  for (i = 0; i < count; i ++)
  {
    keyword = ippGetString(attr, i, NULL);

    for (j = 0; j < (int)(sizeof(server_preasons) / sizeof(server_preasons[0])); j ++)
    {
      if (!strcmp(keyword, server_preasons[j]))
      {
        preasons |= 1 << j;
	break;
      }
    }
  }

  return (preasons);
}


/*
 * 'compare_active_jobs()' - Compare two active jobs.
 */

static int				/* O - Result of comparison */
compare_active_jobs(server_job_t *a,	/* I - First job */
                    server_job_t *b)	/* I - Second job */
{
  int	diff;				/* Difference */


  if ((diff = b->priority - a->priority) == 0)
    diff = b->id - a->id;

  return (diff);
}


/*
 * 'compare_completed_jobs()' - Compare two completed jobs.
 */

static int				/* O - Result of comparison */
compare_completed_jobs(server_job_t *a,	/* I - First job */
                       server_job_t *b)	/* I - Second job */
{
  int	diff;				/* Difference */


  if ((diff = (int)(a->completed - b->completed)) == 0)
    diff = b->id - a->id;

  return (diff);
}


/*
 * 'compare_devices()' - Compare two devices...
 */

static int				/* O - Result of comparison */
compare_devices(server_device_t *a,	/* I - First device */
                server_device_t *b)	/* I - Second device */
{
  return (strcmp(a->uuid, b->uuid));
}


/*
 * 'compare_jobs()' - Compare two jobs.
 */

static int				/* O - Result of comparison */
compare_jobs(server_job_t *a,		/* I - First job */
             server_job_t *b)		/* I - Second job */
{
  return (b->id - a->id);
}


#ifdef HAVE_DNSSD
/*
 * 'dnssd_callback()' - Handle Bonjour registration events.
 */

static void DNSSD_API
dnssd_callback(
    DNSServiceRef       sdRef,		/* I - Service reference */
    DNSServiceFlags     flags,		/* I - Status flags */
    DNSServiceErrorType errorCode,	/* I - Error, if any */
    const char          *name,		/* I - Service name */
    const char          *regtype,	/* I - Service type */
    const char          *domain,	/* I - Domain for service */
    server_printer_t    *printer)	/* I - Printer */
{
  (void)sdRef;
  (void)flags;
  (void)domain;

  if (errorCode)
  {
    fprintf(stderr, "DNSServiceRegister for %s failed with error %d.\n",
            regtype, (int)errorCode);
    return;
  }
  else if (strcasecmp(name, printer->dnssd_name))
  {
    serverLogPrinter(SERVER_LOGLEVEL_INFO, printer, "Now using DNS-SD service name \"%s\".", name);

    /* No lock needed since only the main thread accesses/changes this */
    free(printer->dnssd_name);
    printer->dnssd_name = strdup(name);
  }
}


#elif defined(HAVE_AVAHI)
/*
 * 'dnssd_callback()' - Handle Bonjour registration events.
 */

static void
dnssd_callback(
    AvahiEntryGroup      *srv,		/* I - Service */
    AvahiEntryGroupState state,		/* I - Registration state */
    void                 *context)	/* I - Printer */
{
  (void)srv;
  (void)state;
  (void)context;
}


/*
 * 'dnssd_client_cb()' - Client callback for Avahi.
 *
 * Called whenever the client or server state changes...
 */

static void
dnssd_client_cb(
    AvahiClient      *c,		/* I - Client */
    AvahiClientState state,		/* I - Current state */
    void             *userdata)		/* I - User data (unused) */
{
  (void)userdata;

  if (!c)
    return;

  switch (state)
  {
    default :
        fprintf(stderr, "Ignore Avahi state %d.\n", state);
	break;

    case AVAHI_CLIENT_FAILURE:
	if (avahi_client_errno(c) == AVAHI_ERR_DISCONNECTED)
	{
	  fputs("Avahi server crashed, exiting.\n", stderr);
	  exit(1);
	}
	break;
  }
}
#endif /* HAVE_DNSSD */


/*
 * 'register_printer()' - Register a printer object via Bonjour.
 */

static int				/* O - 1 on success, 0 on error */
register_printer(
    server_printer_t *printer,		/* I - Printer */
    const char       *location,		/* I - Location */
    const char       *make,		/* I - Manufacturer */
    const char       *model,		/* I - Model name */
    const char       *formats,		/* I - Supported formats */
    const char       *adminurl,		/* I - Web interface URL */
    const char       *uuid,		/* I - Printer UUID */
    int              color,		/* I - 1 = color, 0 = monochrome */
    int              duplex,		/* I - 1 = duplex, 0 = simplex */
    const char       *subtype)		/* I - Service subtype */
{
#if defined(HAVE_DNSSD) || defined(HAVE_AVAHI)
  server_txt_t		ipp_txt;	/* Bonjour IPP TXT record */
#endif /* HAVE_DNSSD || HAVE_AVAHI */
#ifdef HAVE_DNSSD
  DNSServiceErrorType	error;		/* Error from Bonjour */
  char			make_model[256],/* Make and model together */
			product[256],	/* Product string */
			regtype[256];	/* Bonjour service type */
  server_listener_t	*lis = cupsArrayFirst(Listeners);
					/* Listen socket */


 /*
  * Build the TXT record for IPP...
  */

  snprintf(make_model, sizeof(make_model), "%s %s", make, model);
  snprintf(product, sizeof(product), "(%s)", model);

  TXTRecordCreate(&ipp_txt, 1024, NULL);
  TXTRecordSetValue(&ipp_txt, "rp", (uint8_t)strlen(printer->resource), printer->resource);
  TXTRecordSetValue(&ipp_txt, "ty", (uint8_t)strlen(make_model), make_model);
  TXTRecordSetValue(&ipp_txt, "adminurl", (uint8_t)strlen(adminurl), adminurl);
  if (*location)
    TXTRecordSetValue(&ipp_txt, "note", (uint8_t)strlen(location), location);
  TXTRecordSetValue(&ipp_txt, "product", (uint8_t)strlen(product), product);
  TXTRecordSetValue(&ipp_txt, "pdl", (uint8_t)strlen(formats), formats);
  TXTRecordSetValue(&ipp_txt, "Color", 1, color ? "T" : "F");
  TXTRecordSetValue(&ipp_txt, "Duplex", 1, duplex ? "T" : "F");
  TXTRecordSetValue(&ipp_txt, "usb_MFG", (uint8_t)strlen(make), make);
  TXTRecordSetValue(&ipp_txt, "usb_MDL", (uint8_t)strlen(model), model);
  TXTRecordSetValue(&ipp_txt, "UUID", (uint8_t)strlen(uuid), uuid);
#  ifdef HAVE_SSL
  TXTRecordSetValue(&ipp_txt, "TLS", 3, "1.2");
#  endif /* HAVE_SSL */
  if (strstr(formats, "image/urf"))
    TXTRecordSetValue(&ipp_txt, "URF", 66, "CP1,IS1-5-7,MT1-2-3-4-5-6-8-9-10-11-12-13,RS300,SRGB24,V1.4,W8,DM1");

  TXTRecordSetValue(&ipp_txt, "txtvers", 1, "1");
  TXTRecordSetValue(&ipp_txt, "qtotal", 1, "1");

 /*
  * Register the _printer._tcp (LPD) service type with a port number of 0 to
  * defend our service name but not actually support LPD...
  */

  printer->printer_ref = DNSSDMaster;

  if ((error = DNSServiceRegister(&(printer->printer_ref),
                                  kDNSServiceFlagsShareConnection,
                                  0 /* interfaceIndex */, printer->dnssd_name,
				  "_printer._tcp", NULL /* domain */,
				  NULL /* host */, 0 /* port */, 0 /* txtLen */,
				  NULL /* txtRecord */,
			          (DNSServiceRegisterReply)dnssd_callback,
			          printer)) != kDNSServiceErr_NoError)
  {
    serverLogPrinter(SERVER_LOGLEVEL_ERROR, printer, "Unable to register \"%s._printer._tcp\": %d", printer->dnssd_name, error);
    return (0);
  }

 /*
  * Then register the _ipp._tcp (IPP) service type with the real port number to
  * advertise our IPP printer...
  */

  printer->ipp_ref = DNSSDMaster;

  if (subtype && *subtype)
    snprintf(regtype, sizeof(regtype), SERVER_IPP_TYPE ",%s", subtype);
  else
    strlcpy(regtype, SERVER_IPP_TYPE, sizeof(regtype));

  if ((error = DNSServiceRegister(&(printer->ipp_ref),
                                  kDNSServiceFlagsShareConnection,
                                  0 /* interfaceIndex */, printer->dnssd_name,
				  regtype, NULL /* domain */,
				  NULL /* host */, htons(lis->port),
				  TXTRecordGetLength(&ipp_txt),
				  TXTRecordGetBytesPtr(&ipp_txt),
			          (DNSServiceRegisterReply)dnssd_callback,
			          printer)) != kDNSServiceErr_NoError)
  {
    serverLogPrinter(SERVER_LOGLEVEL_ERROR, printer, "Unable to register \"%s.%s\": %d", printer->dnssd_name, regtype, error);
    return (0);
  }

 /*
  * Similarly, register the _http._tcp,_printer (HTTP) service type with the
  * real port number to advertise our IPP printer...
  */

  printer->http_ref = DNSSDMaster;

  if ((error = DNSServiceRegister(&(printer->http_ref),
                                  kDNSServiceFlagsShareConnection,
                                  0 /* interfaceIndex */, printer->dnssd_name,
				  SERVER_WEB_TYPE ",_printer", NULL /* domain */,
				  NULL /* host */, htons(lis->port),
				  0 /* txtLen */, NULL, /* txtRecord */
			          (DNSServiceRegisterReply)dnssd_callback,
			          printer)) != kDNSServiceErr_NoError)
  {
    serverLogPrinter(SERVER_LOGLEVEL_ERROR, printer, "Unable to register \"%s.%s\": %d", printer->dnssd_name, SERVER_WEB_TYPE ",_printer", error);
    return (0);
  }

  TXTRecordDeallocate(&ipp_txt);

#elif defined(HAVE_AVAHI)
  char		temp[256];		/* Subtype service string */

 /*
  * Create the TXT record...
  */

  ipp_txt = NULL;
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "rp=%s", printer->resource);
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "ty=%s %s", make, model);
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "adminurl=%s", adminurl);
  if (*location)
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "note=%s", location);
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "product=(%s)", model);
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "pdl=%s", formats);
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "Color=%s", color ? "T" : "F");
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "Duplex=%s", duplex ? "T" : "F");
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "usb_MFG=%s", make);
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "usb_MDL=%s", model);
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "UUID=%s", uuid);
#  ifdef HAVE_SSL
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "TLS=1.2");
#  endif /* HAVE_SSL */
  if (strstr(formats, "image/urf"))
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "URF=CP1,IS1-5-7,MT1-2-3-4-5-6-8-9-10-11-12-13,RS300,SRGB24,V1.4,W8,DM1");
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "txtvers=1");
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "qtotal=1");

 /*
  * Register _printer._tcp (LPD) with port 0 to reserve the service name...
  */

  avahi_threaded_poll_lock(DNSSDMaster);

  printer->ipp_ref = avahi_entry_group_new(DNSSDClient, dnssd_callback, NULL);

  avahi_entry_group_add_service_strlst(printer->ipp_ref, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, printer->dnssd_name, "_printer._tcp", NULL, NULL, 0, NULL);

 /*
  * Then register the _ipp._tcp (IPP)...
  */

  avahi_entry_group_add_service_strlst(printer->ipp_ref, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, printer->dnssd_name, SERVER_IPP_TYPE, NULL, NULL, lis->port, ipp_txt);
  if (subtype && *subtype)
  {
    snprintf(temp, sizeof(temp), "%s._sub." SERVER_IPP_TYPE, subtype);
    avahi_entry_group_add_service_subtype(printer->ipp_ref, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, printer->dnssd_name, SERVER_IPP_TYPE, NULL, temp);
  }

 /*
  * Finally _http.tcp (HTTP) for the web interface...
  */

  avahi_entry_group_add_service_strlst(printer->ipp_ref, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, printer->dnssd_name, SERVER_WEB_TYPE, NULL, NULL, lis->port, NULL);
  avahi_entry_group_add_service_subtype(printer->ipp_ref, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, printer->dnssd_name, SERVER_WEB_TYPE, NULL, "_printer._sub." SERVER_WEB_TYPE);

 /*
  * Commit it...
  */

  avahi_entry_group_commit(printer->ipp_ref);
  avahi_threaded_poll_unlock(DNSSDMaster);

  avahi_string_list_free(ipp_txt);
#endif /* HAVE_DNSSD */

  return (1);
}
