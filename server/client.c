/*
 * Client code for sample IPP server implementation.
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

static void		html_escape(server_client_t *client, const char *s,
			            size_t slen);
static void		html_footer(server_client_t *client);
static void		html_header(server_client_t *client, const char *title);
static void		html_printf(server_client_t *client, const char *format, ...) __attribute__((__format__(__printf__, 2, 3)));
static int		parse_options(server_client_t *client, cups_option_t **options);


/*
 * 'serverCreateClient()' - Accept a new network connection and create a client object.
 */

server_client_t *			/* O - Client */
serverCreateClient(int sock)		/* I - Listen socket */
{
  server_client_t	*client;	/* Client */


  if ((client = calloc(1, sizeof(server_client_t))) == NULL)
  {
    perror("Unable to allocate memory for client");
    return (NULL);
  }

//  client->printer = printer;

 /*
  * Accept the client and get the remote address...
  */

  if ((client->http = httpAcceptConnection(sock, 1)) == NULL)
  {
    serverLogClient(SERVER_LOGLEVEL_ERROR, client, "Unable to accept client connection: %s", cupsLastErrorString());

    free(client);

    return (NULL);
  }

  httpGetHostname(client->http, client->hostname, sizeof(client->hostname));

  serverLogClient(SERVER_LOGLEVEL_INFO, client, "Accepted connection from \"%s\".", client->hostname);

  return (client);
}


/*
 * 'serverCreateListener()' - Create a listener socket.
 */

int					/* O - 1 on success, 0 on error */
serverCreateListeners(const char *host,	/* I - Hostname, IP address, or "*" for any address */
                      int        port)	/* I - Port number */
{
  int			sock;		/* Listener socket */
  http_addrlist_t	*addrlist,	/* Listen address(es) */
			*addr;		/* Current address */
  char			service[32],	/* Service port */
			local[256];	/* Local hostname */
  server_listener_t	*lis;		/* New listener */


  snprintf(service, sizeof(service), "%d", port);
  if ((addrlist = httpAddrGetList(host, AF_UNSPEC, service)) == NULL)
  {
    fprintf(stderr, "ippserver: Unable to resolve Listen address \"%s\": %s\n", host, cupsLastErrorString());
    return (0);
  }

  if (!strcmp(host, "*"))
  {
    httpGetHostname(NULL, local, sizeof(local));
    host = local;
  }

  for (addr = addrlist; addr; addr = addr->next)
  {
    if ((sock = httpAddrListen(&(addrlist->addr), port)) < 0)
      continue;

    lis = calloc(1, sizeof(server_listener_t));
    lis->fd = sock;
    strlcpy(lis->host, host, sizeof(lis->host));
    lis->port = port;

    if (!Listeners)
      Listeners = cupsArrayNew(NULL, NULL);

    cupsArrayAdd(Listeners, lis);
  }

  httpAddrFreeList(addrlist);

  return (1);
}


/*
 * 'serverDeleteClient()' - Close the socket and free all memory used by a client object.
 */

void
serverDeleteClient(server_client_t *client)	/* I - Client */
{
  serverLogClient(SERVER_LOGLEVEL_INFO, client, "Closing connection from \"%s\".", client->hostname);

 /*
  * Flush pending writes before closing...
  */

  httpFlushWrite(client->http);

 /*
  * Free memory...
  */

  httpClose(client->http);

  ippDelete(client->request);
  ippDelete(client->response);

  free(client);
}


/*
 * 'serverProcessClient()' - Process client requests on a thread.
 */

void *					/* O - Exit status */
serverProcessClient(
    server_client_t *client)		/* I - Client */
{
 /*
  * Loop until we are out of requests or timeout (30 seconds)...
  */

#ifdef HAVE_SSL
  int first_time = 1;			/* First time request? */
#endif /* HAVE_SSL */

  while (httpWait(client->http, 30000))
  {
#ifdef HAVE_SSL
    if (first_time)
    {
     /*
      * See if we need to negotiate a TLS connection...
      */

      char buf[1];			/* First byte from client */

      if (recv(httpGetFd(client->http), buf, 1, MSG_PEEK) == 1 && (!buf[0] || !strchr("DGHOPT", buf[0])))
      {
        fprintf(stderr, "%s Starting HTTPS session.\n", client->hostname);

	if (httpEncryption(client->http, HTTP_ENCRYPTION_ALWAYS))
	{
	  fprintf(stderr, "%s Unable to encrypt connection: %s\n", client->hostname, cupsLastErrorString());
	  break;
        }

        fprintf(stderr, "%s Connection now encrypted.\n", client->hostname);
      }

      first_time = 0;
    }
#endif /* HAVE_SSL */

    if (!serverProcessHTTP(client))
      break;
  }

 /*
  * Close the conection to the client and return...
  */

  serverDeleteClient(client);

  return (NULL);
}


/*
 * 'serverProcessHTTP()' - Process a HTTP request.
 */

int					/* O - 1 on success, 0 on failure */
serverProcessHTTP(
    server_client_t *client)		/* I - Client connection */
{
  char			uri[1024];	/* URI */
  http_state_t		http_state;	/* HTTP state */
  http_status_t		http_status;	/* HTTP status */
  ipp_state_t		ipp_state;	/* State of IPP transfer */
  char			scheme[32],	/* Method/scheme */
			userpass[128],	/* Username:password */
			hostname[HTTP_MAX_HOST];
					/* Hostname */
  int			port;		/* Port number */
  const char		*encoding;	/* Content-Encoding value */
  static const char * const http_states[] =
  {					/* Strings for logging HTTP method */
    "WAITING",
    "OPTIONS",
    "GET",
    "GET_SEND",
    "HEAD",
    "POST",
    "POST_RECV",
    "POST_SEND",
    "PUT",
    "PUT_RECV",
    "DELETE",
    "TRACE",
    "CONNECT",
    "STATUS",
    "UNKNOWN_METHOD",
    "UNKNOWN_VERSION"
  };


 /*
  * Clear state variables...
  */

  ippDelete(client->request);
  ippDelete(client->response);

  client->request   = NULL;
  client->response  = NULL;
  client->operation = HTTP_STATE_WAITING;

 /*
  * Read a request from the connection...
  */

  while ((http_state = httpReadRequest(client->http, uri,
                                       sizeof(uri))) == HTTP_STATE_WAITING)
    usleep(1);

 /*
  * Parse the request line...
  */

  if (http_state == HTTP_STATE_ERROR)
  {
    if (httpError(client->http) == EPIPE)
      fprintf(stderr, "%s Client closed connection.\n", client->hostname);
    else
      fprintf(stderr, "%s Bad request line (%s).\n", client->hostname,
              strerror(httpError(client->http)));

    return (0);
  }
  else if (http_state == HTTP_STATE_UNKNOWN_METHOD)
  {
    fprintf(stderr, "%s Bad/unknown operation.\n", client->hostname);
    serverRespondHTTP(client, HTTP_STATUS_BAD_REQUEST, NULL, NULL, 0);
    return (0);
  }
  else if (http_state == HTTP_STATE_UNKNOWN_VERSION)
  {
    fprintf(stderr, "%s Bad HTTP version.\n", client->hostname);
    serverRespondHTTP(client, HTTP_STATUS_BAD_REQUEST, NULL, NULL, 0);
    return (0);
  }

  fprintf(stderr, "%s %s %s\n", client->hostname, http_states[http_state],
          uri);

 /*
  * Separate the URI into its components...
  */

  if (httpSeparateURI(HTTP_URI_CODING_MOST, uri, scheme, sizeof(scheme),
		      userpass, sizeof(userpass),
		      hostname, sizeof(hostname), &port,
		      client->uri, sizeof(client->uri)) < HTTP_URI_STATUS_OK &&
      (http_state != HTTP_STATE_OPTIONS || strcmp(uri, "*")))
  {
    fprintf(stderr, "%s Bad URI \"%s\".\n", client->hostname, uri);
    serverRespondHTTP(client, HTTP_STATUS_BAD_REQUEST, NULL, NULL, 0);
    return (0);
  }

  if ((client->options = strchr(client->uri, '?')) != NULL)
    *(client->options)++ = '\0';

 /*
  * Process the request...
  */

  client->start     = time(NULL);
  client->operation = httpGetState(client->http);

 /*
  * Parse incoming parameters until the status changes...
  */

  while ((http_status = httpUpdate(client->http)) == HTTP_STATUS_CONTINUE);

  if (http_status != HTTP_STATUS_OK)
  {
    serverRespondHTTP(client, HTTP_STATUS_BAD_REQUEST, NULL, NULL, 0);
    return (0);
  }

  if (!httpGetField(client->http, HTTP_FIELD_HOST)[0] &&
      httpGetVersion(client->http) >= HTTP_VERSION_1_1)
  {
   /*
    * HTTP/1.1 and higher require the "Host:" field...
    */

    serverRespondHTTP(client, HTTP_STATUS_BAD_REQUEST, NULL, NULL, 0);
    return (0);
  }

 /*
  * Handle HTTP Upgrade...
  */

  if (!strcasecmp(httpGetField(client->http, HTTP_FIELD_CONNECTION),
                        "Upgrade"))
  {
#ifdef HAVE_SSL
    if (strstr(httpGetField(client->http, HTTP_FIELD_UPGRADE), "TLS/") != NULL && !httpIsEncrypted(client->http))
    {
      if (!serverRespondHTTP(client, HTTP_STATUS_SWITCHING_PROTOCOLS, NULL, NULL, 0))
        return (0);

      fprintf(stderr, "%s Upgrading to encrypted connection.\n", client->hostname);

      if (httpEncryption(client->http, HTTP_ENCRYPTION_REQUIRED))
      {
        fprintf(stderr, "%s Unable to encrypt connection: %s\n", client->hostname, cupsLastErrorString());
	return (0);
      }

      fprintf(stderr, "%s Connection now encrypted.\n", client->hostname);
    }
    else
#endif /* HAVE_SSL */

    if (!serverRespondHTTP(client, HTTP_STATUS_NOT_IMPLEMENTED, NULL, NULL, 0))
      return (0);
  }

 /*
  * Handle HTTP Expect...
  */

  if (httpGetExpect(client->http) &&
      (client->operation == HTTP_STATE_POST ||
       client->operation == HTTP_STATE_PUT))
  {
    if (httpGetExpect(client->http) == HTTP_STATUS_CONTINUE)
    {
     /*
      * Send 100-continue header...
      */

      if (!serverRespondHTTP(client, HTTP_STATUS_CONTINUE, NULL, NULL, 0))
	return (0);
    }
    else
    {
     /*
      * Send 417-expectation-failed header...
      */

      if (!serverRespondHTTP(client, HTTP_STATUS_EXPECTATION_FAILED, NULL, NULL, 0))
	return (0);
    }
  }

 /*
  * Handle new transfers...
  */

  encoding = httpGetContentEncoding(client->http);

  switch (client->operation)
  {
    case HTTP_STATE_OPTIONS :
       /*
	* Do OPTIONS command...
	*/

	return (serverRespondHTTP(client, HTTP_STATUS_OK, NULL, NULL, 0));

    case HTTP_STATE_HEAD :
        if (!strcmp(client->uri, "/icon.png"))
	  return (serverRespondHTTP(client, HTTP_STATUS_OK, NULL, "image/png", 0));
	else if (!strcmp(client->uri, "/") || !strcmp(client->uri, "/media") || !strcmp(client->uri, "/supplies"))
	  return (serverRespondHTTP(client, HTTP_STATUS_OK, NULL, "text/html", 0));
	else
	  return (serverRespondHTTP(client, HTTP_STATUS_NOT_FOUND, NULL, NULL, 0));

    case HTTP_STATE_GET :
        if (!strcmp(client->uri, "/icon.png"))
	{
	 /*
	  * Send PNG icon file.
	  */

          int		fd;		/* Icon file */
	  struct stat	fileinfo;	/* Icon file information */
	  char		buffer[4096];	/* Copy buffer */
	  ssize_t	bytes;		/* Bytes */

          fprintf(stderr, "Icon file is \"%s\".\n", client->printer->icon);

          if (!stat(client->printer->icon, &fileinfo) &&
	      (fd = open(client->printer->icon, O_RDONLY)) >= 0)
	  {
	    if (!serverRespondHTTP(client, HTTP_STATUS_OK, NULL, "image/png",
	                      (size_t)fileinfo.st_size))
	    {
	      close(fd);
	      return (0);
	    }

	    while ((bytes = read(fd, buffer, sizeof(buffer))) > 0)
	      httpWrite2(client->http, buffer, (size_t)bytes);

	    httpFlushWrite(client->http);

	    close(fd);
	  }
	  else
	    return (serverRespondHTTP(client, HTTP_STATUS_NOT_FOUND, NULL, NULL, 0));
	}
	else if (!strcmp(client->uri, "/"))
	{
	 /*
	  * Show web status page...
	  */

          server_job_t	*job;		/* Current job */
	  int		i;		/* Looping var */
	  server_preason_t reason;	/* Current reason */
	  static const char * const reasons[] =
	  {				/* Reason strings */
	    "Other",
	    "Cover Open",
	    "Input Tray Missing",
	    "Marker Supply Empty",
	    "Marker Supply Low",
	    "Marker Waste Almost Full",
	    "Marker Waste Full",
	    "Media Empty",
	    "Media Jam",
	    "Media Low",
	    "Media Needed",
	    "Moving to Paused",
	    "Paused",
	    "Spool Area Full",
	    "Toner Empty",
	    "Toner Low"
	  };

          if (!serverRespondHTTP(client, HTTP_STATUS_OK, encoding, "text/html", 0))
	    return (0);

          html_header(client, client->printer->name);
          html_printf(client,
		      "<p><img align=\"right\" src=\"/icon.png\" width=\"64\" height=\"64\"><b>ippserver (" CUPS_SVERSION ")</b></p>\n"
		      "<p>%s, %d job(s).", client->printer->state == IPP_PSTATE_IDLE ? "Idle" : client->printer->state == IPP_PSTATE_PROCESSING ? "Printing" : "Stopped", cupsArrayCount(client->printer->jobs));
	  for (i = 0, reason = 1; i < (int)(sizeof(reasons) / sizeof(reasons[0])); i ++, reason <<= 1)
	    if (client->printer->state_reasons & reason)
	      html_printf(client, "\n<br>&nbsp;&nbsp;&nbsp;&nbsp;%s", reasons[i]);
	  html_printf(client, "</p>\n");
	  
          if (cupsArrayCount(client->printer->jobs) > 0)
	  {
            _cupsRWLockRead(&(client->printer->rwlock));

	    html_printf(client, "<table class=\"striped\" summary=\"Jobs\"><thead><tr><th>Job #</th><th>Name</th><th>Owner</th><th>When</th></tr></thead><tbody>\n");
	    for (job = (server_job_t *)cupsArrayFirst(client->printer->jobs); job; job = (server_job_t *)cupsArrayNext(client->printer->jobs))
	    {
	      char	when[256],	/* When job queued/started/finished */
			hhmmss[64];	/* Time HH:MM:SS */

              switch (job->state)
	      {
	        case IPP_JSTATE_PENDING :
	        case IPP_JSTATE_HELD :
		    snprintf(when, sizeof(when), "Queued at %s", serverTimeString(job->created, hhmmss, sizeof(hhmmss)));
		    break;
	        case IPP_JSTATE_PROCESSING :
	        case IPP_JSTATE_STOPPED :
		    snprintf(when, sizeof(when), "Started at %s", serverTimeString(job->processing, hhmmss, sizeof(hhmmss)));
		    break;
	        case IPP_JSTATE_ABORTED :
		    snprintf(when, sizeof(when), "Aborted at %s", serverTimeString(job->completed, hhmmss, sizeof(hhmmss)));
		    break;
	        case IPP_JSTATE_CANCELED :
		    snprintf(when, sizeof(when), "Canceled at %s", serverTimeString(job->completed, hhmmss, sizeof(hhmmss)));
		    break;
	        case IPP_JSTATE_COMPLETED :
		    snprintf(when, sizeof(when), "Completed at %s", serverTimeString(job->completed, hhmmss, sizeof(hhmmss)));
		    break;
	      }

	      html_printf(client, "<tr><td>%d</td><td>%s</td><td>%s</td><td>%s</td></tr>\n", job->id, job->name, job->username, when);
	    }
	    html_printf(client, "</tbody></table>\n");

	    _cupsRWUnlock(&(client->printer->rwlock));
	  }
          html_footer(client);

	  return (1);
	}
#if 0 /* TODO: Pull media and supply info from device attrs */
	else if (!strcmp(client->uri, "/media"))
	{
	 /*
	  * Show web media page...
	  */

	  int		i,		/* Looping var */
			num_options;	/* Number of form options */
	  cups_option_t	*options;	/* Form options */
          static const char * const sizes[] =
	  {				/* Size strings */
	    "ISO A4",
	    "ISO A5",
	    "ISO A6",
	    "DL Envelope",
	    "US Legal",
	    "US Letter",
	    "#10 Envelope",
	    "3x5 Photo",
	    "3.5x5 Photo",
	    "4x6 Photo",
	    "5x7 Photo"
	  };
	  static const char * const types[] =
					  /* Type strings */
	  {
	    "Auto",
	    "Cardstock",
	    "Envelope",
	    "Labels",
	    "Other",
	    "Glossy Photo",
	    "High-Gloss Photo",
	    "Matte Photo",
	    "Satin Photo",
	    "Semi-Gloss Photo",
	    "Plain",
	    "Letterhead",
	    "Transparency"
	  };
	  static const int sheets[] =	/* Number of sheets */
	  {
	    250,
	    100,
	    25,
	    5,
	    0
	  };

          if (!serverRespondHTTP(client, HTTP_STATUS_OK, encoding, "text/html", 0))
	    return (0);

          html_header(client, client->printer->name);

	  if ((num_options = parse_options(client, &options)) > 0)
	  {
	   /*
	    * WARNING: A real printer/server implementation MUST NOT implement
	    * media updates via a GET request - GET requests are supposed to be
	    * idempotent (without side-effects) and we obviously are not
	    * authenticating access here.  This form is provided solely to
	    * enable testing and development!
	    */

	    const char	*val;		/* Form value */

	    if ((val = cupsGetOption("main_size", num_options, options)) != NULL)
	      client->printer->main_size = atoi(val);
	    if ((val = cupsGetOption("main_type", num_options, options)) != NULL)
	      client->printer->main_type = atoi(val);
	    if ((val = cupsGetOption("main_level", num_options, options)) != NULL)
	      client->printer->main_level = atoi(val);

	    if ((val = cupsGetOption("envelope_size", num_options, options)) != NULL)
	      client->printer->envelope_size = atoi(val);
	    if ((val = cupsGetOption("envelope_level", num_options, options)) != NULL)
	      client->printer->envelope_level = atoi(val);

	    if ((val = cupsGetOption("photo_size", num_options, options)) != NULL)
	      client->printer->photo_size = atoi(val);
	    if ((val = cupsGetOption("photo_type", num_options, options)) != NULL)
	      client->printer->photo_type = atoi(val);
	    if ((val = cupsGetOption("photo_level", num_options, options)) != NULL)
	      client->printer->photo_level = atoi(val);

            if ((client->printer->main_level < 100 && client->printer->main_level > 0) || (client->printer->envelope_level < 25 && client->printer->envelope_level > 0) || (client->printer->photo_level < 25 && client->printer->photo_level > 0))
	      client->printer->state_reasons |= SERVER_PREASON_MEDIA_LOW;
	    else
	      client->printer->state_reasons &= (server_preason_t)~SERVER_PREASON_MEDIA_LOW;

            if ((client->printer->main_level == 0 && client->printer->main_size > _IPP_MEDIA_SIZE_NONE) || (client->printer->envelope_level == 0 && client->printer->envelope_size > _IPP_MEDIA_SIZE_NONE) || (client->printer->photo_level == 0 && client->printer->photo_size > _IPP_MEDIA_SIZE_NONE))
	    {
	      client->printer->state_reasons |= SERVER_PREASON_MEDIA_EMPTY;
	      if (client->printer->active_job)
	        client->printer->state_reasons |= SERVER_PREASON_MEDIA_NEEDED;
	    }
	    else
	      client->printer->state_reasons &= (server_preason_t)~(SERVER_PREASON_MEDIA_EMPTY | SERVER_PREASON_MEDIA_NEEDED);

	    html_printf(client, "<blockquote>Media updated.</blockquote>\n");
          }

          html_printf(client, "<form method=\"GET\" action=\"/media\">\n");

          html_printf(client, "<table class=\"form\" summary=\"Media\">\n");
          html_printf(client, "<tr><th>Main Tray:</th><td><select name=\"main_size\"><option value=\"-1\">None</option>");
          for (i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i ++)
	    if (!strstr(sizes[i], "Envelope") && !strstr(sizes[i], "Photo"))
	      html_printf(client, "<option value=\"%d\"%s>%s</option>", i, i == client->printer->main_size ? " selected" : "", sizes[i]);
	  html_printf(client, "</select> <select name=\"main_type\"><option value=\"-1\">None</option>");
          for (i = 0; i < (int)(sizeof(types) / sizeof(types[0])); i ++)
	    if (!strstr(types[i], "Photo"))
	      html_printf(client, "<option value=\"%d\"%s>%s</option>", i, i == client->printer->main_type ? " selected" : "", types[i]);
	  html_printf(client, "</select> <select name=\"main_level\">");
          for (i = 0; i < (int)(sizeof(sheets) / sizeof(sheets[0])); i ++)
	    html_printf(client, "<option value=\"%d\"%s>%d sheets</option>", sheets[i], sheets[i] == client->printer->main_level ? " selected" : "", sheets[i]);
	  html_printf(client, "</select></td></tr>\n");

          html_printf(client,
		      "<tr><th>Envelope Feeder:</th><td><select name=\"envelope_size\"><option value=\"-1\">None</option>");
          for (i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i ++)
	    if (strstr(sizes[i], "Envelope"))
	      html_printf(client, "<option value=\"%d\"%s>%s</option>", i, i == client->printer->envelope_size ? " selected" : "", sizes[i]);
	  html_printf(client, "</select> <select name=\"envelope_level\">");
          for (i = 0; i < (int)(sizeof(sheets) / sizeof(sheets[0])); i ++)
	    html_printf(client, "<option value=\"%d\"%s>%d sheets</option>", sheets[i], sheets[i] == client->printer->envelope_level ? " selected" : "", sheets[i]);
	  html_printf(client, "</select></td></tr>\n");

          html_printf(client,
		      "<tr><th>Photo Tray:</th><td><select name=\"photo_size\"><option value=\"-1\">None</option>");
          for (i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i ++)
	    if (strstr(sizes[i], "Photo"))
	      html_printf(client, "<option value=\"%d\"%s>%s</option>", i, i == client->printer->photo_size ? " selected" : "", sizes[i]);
	  html_printf(client, "</select> <select name=\"photo_type\"><option value=\"-1\">None</option>");
          for (i = 0; i < (int)(sizeof(types) / sizeof(types[0])); i ++)
	    if (strstr(types[i], "Photo"))
	      html_printf(client, "<option value=\"%d\"%s>%s</option>", i, i == client->printer->photo_type ? " selected" : "", types[i]);
	  html_printf(client, "</select> <select name=\"photo_level\">");
          for (i = 0; i < (int)(sizeof(sheets) / sizeof(sheets[0])); i ++)
	    html_printf(client, "<option value=\"%d\"%s>%d sheets</option>", sheets[i], sheets[i] == client->printer->photo_level ? " selected" : "", sheets[i]);
	  html_printf(client, "</select></td></tr>\n");

	  html_printf(client, "<tr><td></td><td><input type=\"submit\" value=\"Update Media\"></td></tr></table></form>\n");
          html_footer(client);

	  return (1);
	}
	else if (!strcmp(client->uri, "/supplies"))
	{
	 /*
	  * Show web supplies page...
	  */

          int		i, j,		/* Looping vars */
			num_options;	/* Number of form options */
	  cups_option_t	*options;	/* Form options */
	  static const int levels[] = { 0, 5, 10, 25, 50, 75, 90, 95, 100 };

          if (!serverRespondHTTP(client, HTTP_STATUS_OK, encoding, "text/html", 0))
	    return (0);

          html_header(client, client->printer->name);

	  if ((num_options = parse_options(client, &options)) > 0)
	  {
	   /*
	    * WARNING: A real printer/server implementation MUST NOT implement
	    * supply updates via a GET request - GET requests are supposed to be
	    * idempotent (without side-effects) and we obviously are not
	    * authenticating access here.  This form is provided solely to
	    * enable testing and development!
	    */

	    char	name[64];	/* Form field */
	    const char	*val;		/* Form value */

            client->printer->state_reasons &= (server_preason_t)~(SERVER_PREASON_MARKER_SUPPLY_EMPTY | SERVER_PREASON_MARKER_SUPPLY_LOW | SERVER_PREASON_MARKER_WASTE_ALMOST_FULL | SERVER_PREASON_MARKER_WASTE_FULL | SERVER_PREASON_TONER_EMPTY | SERVER_PREASON_TONER_LOW);

	    for (i = 0; i < (int)(sizeof(printer_supplies) / sizeof(printer_supplies[0])); i ++)
	    {
	      snprintf(name, sizeof(name), "supply_%d", i);
	      if ((val = cupsGetOption(name, num_options, options)) != NULL)
	      {
		int level = client->printer->supplies[i] = atoi(val);
					/* New level */

		if (i < 4)
		{
		  if (level == 0)
		    client->printer->state_reasons |= SERVER_PREASON_TONER_EMPTY;
		  else if (level < 10)
		    client->printer->state_reasons |= SERVER_PREASON_TONER_LOW;
		}
		else
		{
		  if (level == 100)
		    client->printer->state_reasons |= SERVER_PREASON_MARKER_WASTE_FULL;
		  else if (level > 90)
		    client->printer->state_reasons |= SERVER_PREASON_MARKER_WASTE_ALMOST_FULL;
		}
	      }
            }

	    html_printf(client, "<blockquote>Supplies updated.</blockquote>\n");
          }

          html_printf(client, "<form method=\"GET\" action=\"/supplies\">\n");

	  html_printf(client, "<table class=\"form\" summary=\"Supplies\">\n");
	  for (i = 0; i < (int)(sizeof(printer_supplies) / sizeof(printer_supplies[0])); i ++)
	  {
	    html_printf(client, "<tr><th>%s:</th><td><select name=\"supply_%d\">", printer_supplies[i], i);
	    for (j = 0; j < (int)(sizeof(levels) / sizeof(levels[0])); j ++)
	      html_printf(client, "<option value=\"%d\"%s>%d%%</option>", levels[j], levels[j] == client->printer->supplies[i] ? " selected" : "", levels[j]);
	    html_printf(client, "</select></td></tr>\n");
	  }
	  html_printf(client, "<tr><td></td><td><input type=\"submit\" value=\"Update Supplies\"></td></tr>\n</table>\n</form>\n");
          html_footer(client);

	  return (1);
	}
#endif /* 0 */
	else
	  return (serverRespondHTTP(client, HTTP_STATUS_NOT_FOUND, NULL, NULL, 0));
	break;

    case HTTP_STATE_POST :
	if (strcmp(httpGetField(client->http, HTTP_FIELD_CONTENT_TYPE),
	           "application/ipp"))
        {
	 /*
	  * Not an IPP request...
	  */

	  return (serverRespondHTTP(client, HTTP_STATUS_BAD_REQUEST, NULL, NULL, 0));
	}

       /*
        * Read the IPP request...
	*/

	client->request = ippNew();

        while ((ipp_state = ippRead(client->http,
                                    client->request)) != IPP_STATE_DATA)
	{
	  if (ipp_state == IPP_STATE_ERROR)
	  {
            fprintf(stderr, "%s IPP read error (%s).\n", client->hostname,
	            cupsLastErrorString());
	    serverRespondHTTP(client, HTTP_STATUS_BAD_REQUEST, NULL, NULL, 0);
	    return (0);
	  }
	}

       /*
        * Now that we have the IPP request, process the request...
	*/

        return (serverProcessIPP(client));

    default :
        break; /* Anti-compiler-warning-code */
  }

  return (1);
}


/*
 * 'serverRespondHTTP()' - Send a HTTP response.
 */

int					/* O - 1 on success, 0 on failure */
serverRespondHTTP(
    server_client_t *client,		/* I - Client */
    http_status_t code,			/* I - HTTP status of response */
    const char    *content_encoding,	/* I - Content-Encoding of response */
    const char    *type,		/* I - MIME media type of response */
    size_t        length)		/* I - Length of response */
{
  char	message[1024];			/* Text message */


  fprintf(stderr, "%s %s\n", client->hostname, httpStatus(code));

  if (code == HTTP_STATUS_CONTINUE)
  {
   /*
    * 100-continue doesn't send any headers...
    */

    return (httpWriteResponse(client->http, HTTP_STATUS_CONTINUE) == 0);
  }

 /*
  * Format an error message...
  */

  if (!type && !length && code != HTTP_STATUS_OK && code != HTTP_STATUS_SWITCHING_PROTOCOLS)
  {
    snprintf(message, sizeof(message), "%d - %s\n", code, httpStatus(code));

    type   = "text/plain";
    length = strlen(message);
  }
  else
    message[0] = '\0';

 /*
  * Send the HTTP response header...
  */

  httpClearFields(client->http);

  if (code == HTTP_STATUS_METHOD_NOT_ALLOWED ||
      client->operation == HTTP_STATE_OPTIONS)
    httpSetField(client->http, HTTP_FIELD_ALLOW, "GET, HEAD, OPTIONS, POST");

  if (type)
  {
    if (!strcmp(type, "text/html"))
      httpSetField(client->http, HTTP_FIELD_CONTENT_TYPE,
                   "text/html; charset=utf-8");
    else
      httpSetField(client->http, HTTP_FIELD_CONTENT_TYPE, type);

    if (content_encoding)
      httpSetField(client->http, HTTP_FIELD_CONTENT_ENCODING, content_encoding);
  }

  httpSetLength(client->http, length);

  if (httpWriteResponse(client->http, code) < 0)
    return (0);

 /*
  * Send the response data...
  */

  if (message[0])
  {
   /*
    * Send a plain text message.
    */

    if (httpPrintf(client->http, "%s", message) < 0)
      return (0);

    if (httpWrite2(client->http, "", 0) < 0)
      return (0);
  }
  else if (client->response)
  {
   /*
    * Send an IPP response...
    */

    ippSetState(client->response, IPP_STATE_IDLE);

    if (ippWrite(client->http, client->response) != IPP_STATE_DATA)
      return (0);

    if (client->fetch_file >= 0)
    {
      ssize_t	bytes;			/* Bytes read */
      char	buffer[32768];		/* Buffer */

      if (client->fetch_compression)
        httpSetField(client->http, HTTP_FIELD_CONTENT_ENCODING, "gzip");

      while ((bytes = read(client->fetch_file, buffer, sizeof(buffer))) > 0)
        httpWrite2(client->http, buffer, (size_t)bytes);

      httpWrite2(client->http, "", 0);
      close(client->fetch_file);
      client->fetch_file = -1;
    }
  }

  return (1);
}


/*
 * 'serverRun()' - Run the server.
 */

void
serverRun(void)
{
#if 0
  int		num_fds;		/* Number of file descriptors */
  struct pollfd	polldata[3];		/* poll() data */
  int		timeout;		/* Timeout for poll() */
  server_client_t	*client;		/* New client */


 /*
  * Setup poll() data for the Bonjour service socket and IPv4/6 listeners...
  */

  polldata[0].fd     = printer->ipv4;
  polldata[0].events = POLLIN;

  polldata[1].fd     = printer->ipv6;
  polldata[1].events = POLLIN;

  num_fds = 2;

#ifdef HAVE_DNSSD
  polldata[num_fds   ].fd     = DNSServiceRefSockFD(DNSSDMaster);
  polldata[num_fds ++].events = POLLIN;
#endif /* HAVE_DNSSD */

 /*
  * Loop until we are killed or have a hard error...
  */

  for (;;)
  {
    if (cupsArrayCount(printer->jobs))
      timeout = 10;
    else
      timeout = -1;

    if (poll(polldata, (nfds_t)num_fds, timeout) < 0 && errno != EINTR)
    {
      perror("poll() failed");
      break;
    }

    if (polldata[0].revents & POLLIN)
    {
      if ((client = serverCreateClient(printer, printer->ipv4)) != NULL)
      {
	if (!_cupsThreadCreate((_cups_thread_func_t)serverProcessClient, client))
	{
	  perror("Unable to create client thread");
	  serverDeleteClient(client);
	}
      }
    }

    if (polldata[1].revents & POLLIN)
    {
      if ((client = serverCreateClient(printer, printer->ipv6)) != NULL)
      {
	if (!_cupsThreadCreate((_cups_thread_func_t)serverProcessClient, client))
	{
	  perror("Unable to create client thread");
	  serverDeleteClient(client);
	}
      }
    }

#ifdef HAVE_DNSSD
    if (polldata[2].revents & POLLIN)
      DNSServiceProcessResult(DNSSDMaster);
#endif /* HAVE_DNSSD */

   /*
    * Clean out old jobs...
    */

    serverCleanJobs(printer);
  }
#endif // 0
}


/*
 * 'html_escape()' - Write a HTML-safe string.
 */

static void
html_escape(server_client_t *client,	/* I - Client */
	    const char    *s,		/* I - String to write */
	    size_t        slen)		/* I - Number of characters to write */
{
  const char	*start,			/* Start of segment */
		*end;			/* End of string */


  start = s;
  end   = s + (slen > 0 ? slen : strlen(s));

  while (*s && s < end)
  {
    if (*s == '&' || *s == '<')
    {
      if (s > start)
        httpWrite2(client->http, start, (size_t)(s - start));

      if (*s == '&')
        httpWrite2(client->http, "&amp;", 5);
      else
        httpWrite2(client->http, "&lt;", 4);

      start = s + 1;
    }

    s ++;
  }

  if (s > start)
    httpWrite2(client->http, start, (size_t)(s - start));
}


/*
 * 'html_footer()' - Show the web interface footer.
 *
 * This function also writes the trailing 0-length chunk.
 */

static void
html_footer(server_client_t *client)	/* I - Client */
{
  html_printf(client,
	      "</div>\n"
	      "</body>\n"
	      "</html>\n");
  httpWrite2(client->http, "", 0);
}


/*
 * 'html_header()' - Show the web interface header and title.
 */

static void
html_header(server_client_t *client,	/* I - Client */
            const char    *title)	/* I - Title */
{
  html_printf(client,
	      "<!doctype html>\n"
	      "<html>\n"
	      "<head>\n"
	      "<title>%s</title>\n"
	      "<link rel=\"shortcut icon\" href=\"/icon.png\" type=\"image/png\">\n"
	      "<link rel=\"apple-touch-icon\" href=\"/icon.png\" type=\"image/png\">\n"
	      "<meta http-equiv=\"X-UA-Compatible\" content=\"IE=9\">\n"
	      "<meta name=\"viewport\" content=\"width=device-width\">\n"
	      "<style>\n"
	      "body { font-family: sans-serif; margin: 0; }\n"
	      "div.body { padding: 0px 10px 10px; }\n"
	      "blockquote { background: #dfd; border-radius: 5px; color: #006; padding: 10px; }\n"
	      "table.form { border-collapse: collapse; margin-top: 10px; width: 100%%; }\n"
	      "table.form td, table.form th { padding: 5px 2px; width: 50%%; }\n"
	      "table.form th { text-align: right; }\n"
	      "table.striped { border-bottom: solid thin black; border-collapse: collapse; width: 100%%; }\n"
	      "table.striped tr:nth-child(even) { background: #fcfcfc; }\n"
	      "table.striped tr:nth-child(odd) { background: #f0f0f0; }\n"
	      "table.striped th { background: white; border-bottom: solid thin black; text-align: left; vertical-align: bottom; }\n"
	      "table.striped td { margin: 0; padding: 5px; vertical-align: top; }\n"
	      "table.nav { border-collapse: collapse; width: 100%%; }\n"
	      "table.nav td { margin: 0; text-align: center; }\n"
	      "td.nav a, td.nav a:active, td.nav a:hover, td.nav a:hover:link, td.nav a:hover:link:visited, td.nav a:link, td.nav a:link:visited, td.nav a:visited { background: inherit; color: inherit; font-size: 80%%; text-decoration: none; }\n"
	      "td.nav { background: #333; color: #fff; padding: 4px 8px; width: 33%%; }\n"
	      "td.nav.sel { background: #fff; color: #000; font-weight: bold; }\n"
	      "td.nav:hover { background: #666; color: #fff; }\n"
	      "td.nav:active { background: #000; color: #ff0; }\n"
	      "</style>\n"
	      "</head>\n"
	      "<body>\n"
	      "<table class=\"nav\"><tr>"
	      "<td class=\"nav%s\"><a href=\"/\">Status</a></td>"
	      "<td class=\"nav%s\"><a href=\"/supplies\">Supplies</a></td>"
	      "<td class=\"nav%s\"><a href=\"/media\">Media</a></td>"
	      "</tr></table>\n"
	      "<div class=\"body\">\n", title, !strcmp(client->uri, "/") ? " sel" : "", !strcmp(client->uri, "/supplies") ? " sel" : "", !strcmp(client->uri, "/media") ? " sel" : "");
}


/*
 * 'html_printf()' - Send formatted text to the client, quoting as needed.
 */

static void
html_printf(server_client_t *client,	/* I - Client */
	    const char    *format,	/* I - Printf-style format string */
	    ...)			/* I - Additional arguments as needed */
{
  va_list	ap;			/* Pointer to arguments */
  const char	*start;			/* Start of string */
  char		size,			/* Size character (h, l, L) */
		type;			/* Format type character */
  int		width,			/* Width of field */
		prec;			/* Number of characters of precision */
  char		tformat[100],		/* Temporary format string for sprintf() */
		*tptr,			/* Pointer into temporary format */
		temp[1024];		/* Buffer for formatted numbers */
  char		*s;			/* Pointer to string */


 /*
  * Loop through the format string, formatting as needed...
  */

  va_start(ap, format);
  start = format;

  while (*format)
  {
    if (*format == '%')
    {
      if (format > start)
        httpWrite2(client->http, start, (size_t)(format - start));

      tptr    = tformat;
      *tptr++ = *format++;

      if (*format == '%')
      {
        httpWrite2(client->http, "%", 1);
        format ++;
	start = format;
	continue;
      }
      else if (strchr(" -+#\'", *format))
        *tptr++ = *format++;

      if (*format == '*')
      {
       /*
        * Get width from argument...
	*/

	format ++;
	width = va_arg(ap, int);

	snprintf(tptr, sizeof(tformat) - (size_t)(tptr - tformat), "%d", width);
	tptr += strlen(tptr);
      }
      else
      {
	width = 0;

	while (isdigit(*format & 255))
	{
	  if (tptr < (tformat + sizeof(tformat) - 1))
	    *tptr++ = *format;

	  width = width * 10 + *format++ - '0';
	}
      }

      if (*format == '.')
      {
	if (tptr < (tformat + sizeof(tformat) - 1))
	  *tptr++ = *format;

        format ++;

        if (*format == '*')
	{
         /*
	  * Get precision from argument...
	  */

	  format ++;
	  prec = va_arg(ap, int);

	  snprintf(tptr, sizeof(tformat) - (size_t)(tptr - tformat), "%d", prec);
	  tptr += strlen(tptr);
	}
	else
	{
	  prec = 0;

	  while (isdigit(*format & 255))
	  {
	    if (tptr < (tformat + sizeof(tformat) - 1))
	      *tptr++ = *format;

	    prec = prec * 10 + *format++ - '0';
	  }
	}
      }

      if (*format == 'l' && format[1] == 'l')
      {
        size = 'L';

	if (tptr < (tformat + sizeof(tformat) - 2))
	{
	  *tptr++ = 'l';
	  *tptr++ = 'l';
	}

	format += 2;
      }
      else if (*format == 'h' || *format == 'l' || *format == 'L')
      {
	if (tptr < (tformat + sizeof(tformat) - 1))
	  *tptr++ = *format;

        size = *format++;
      }
      else
        size = 0;


      if (!*format)
      {
        start = format;
        break;
      }

      if (tptr < (tformat + sizeof(tformat) - 1))
        *tptr++ = *format;

      type  = *format++;
      *tptr = '\0';
      start = format;

      switch (type)
      {
	case 'E' : /* Floating point formats */
	case 'G' :
	case 'e' :
	case 'f' :
	case 'g' :
	    if ((size_t)(width + 2) > sizeof(temp))
	      break;

	    sprintf(temp, tformat, va_arg(ap, double));

            httpWrite2(client->http, temp, strlen(temp));
	    break;

        case 'B' : /* Integer formats */
	case 'X' :
	case 'b' :
        case 'd' :
	case 'i' :
	case 'o' :
	case 'u' :
	case 'x' :
	    if ((size_t)(width + 2) > sizeof(temp))
	      break;

#  ifdef HAVE_LONG_LONG
            if (size == 'L')
	      sprintf(temp, tformat, va_arg(ap, long long));
	    else
#  endif /* HAVE_LONG_LONG */
            if (size == 'l')
	      sprintf(temp, tformat, va_arg(ap, long));
	    else
	      sprintf(temp, tformat, va_arg(ap, int));

            httpWrite2(client->http, temp, strlen(temp));
	    break;

	case 'p' : /* Pointer value */
	    if ((size_t)(width + 2) > sizeof(temp))
	      break;

	    sprintf(temp, tformat, va_arg(ap, void *));

            httpWrite2(client->http, temp, strlen(temp));
	    break;

        case 'c' : /* Character or character array */
            if (width <= 1)
            {
              temp[0] = (char)va_arg(ap, int);
              temp[1] = '\0';
              html_escape(client, temp, 1);
            }
            else
              html_escape(client, va_arg(ap, char *), (size_t)width);
	    break;

	case 's' : /* String */
	    if ((s = va_arg(ap, char *)) == NULL)
	      s = "(null)";

            html_escape(client, s, strlen(s));
	    break;
      }
    }
    else
      format ++;
  }

  if (format > start)
    httpWrite2(client->http, start, (size_t)(format - start));

  va_end(ap);
}


/*
 * 'parse_options()' - Parse URL options into CUPS options.
 *
 * The client->options string is destroyed by this function.
 */

static int				/* O - Number of options */
parse_options(server_client_t *client,	/* I - Client */
              cups_option_t **options)	/* O - Options */
{
  char	*name,				/* Name */
      	*value,				/* Value */
	*next;				/* Next name=value pair */
  int	num_options = 0;		/* Number of options */


  *options = NULL;

  for (name = client->options; name && *name; name = next)
  {
    if ((value = strchr(name, '=')) == NULL)
      break;

    *value++ = '\0';
    if ((next = strchr(value, '&')) != NULL)
      *next++ = '\0';

    num_options = cupsAddOption(name, value, num_options, options);
  }

  return (num_options);
}
