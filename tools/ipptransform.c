/*
 * ipptransform utility for converting PDF and JPEG files to raster data or HP PCL.
 *
 * Copyright 2016 by Apple Inc.
 *
 * These coded instructions, statements, and computer programs are the
 * property of Apple Inc. and are protected by Federal copyright
 * law.  Distribution and use rights are outlined in the file "LICENSE.txt"
 * which should have been included with this file.  If this file is
 * file is missing or damaged, see the license at "http://www.cups.org/".
 *
 * This file is subject to the Apple OS-Developed Software exception.
 */

#include <cups/cups.h>
#include <cups/raster.h>
#include <cups/array-private.h>
#include <cups/string-private.h>

#ifdef __APPLE__
#  include <ApplicationServices/ApplicationServices.h>
#endif /* __APPLE__ */

extern void CGContextSetCTM(CGContextRef c, CGAffineTransform m);

#include "threshold64.h"


/*
 * Constants...
 */

#define XFORM_MAX_RASTER	16777216

#define XFORM_RED_MASK		0x000000ff
#define XFORM_GREEN_MASK	0x0000ff00
#define XFORM_BLUE_MASK		0x00ff0000
#define XFORM_RGB_MASK		(XFORM_RED_MASK | XFORM_GREEN_MASK |  XFORM_BLUE_MASK)
#define XFORM_BG_MASK		(XFORM_BLUE_MASK | XFORM_GREEN_MASK)
#define XFORM_RG_MASK		(XFORM_RED_MASK | XFORM_GREEN_MASK)


/*
 * Local types...
 */

typedef ssize_t (*xform_write_cb_t)(void *, const unsigned char *, size_t);

typedef struct xform_raster_s xform_raster_t;

struct xform_raster_s
{
  int			num_options;	/* Number of job options */
  cups_option_t		*options;	/* Job options */
  unsigned		copies;		/* Number of copies */
  cups_page_header2_t	header;		/* Page header */
  cups_page_header2_t	back_header;	/* Page header for back side */
  unsigned char		*band_buffer;	/* Band buffer */
  unsigned		band_height;	/* Band height */
  unsigned		band_bpp;	/* Bytes per pixel in band */

  /* Set by start_job callback */
  cups_raster_t		*ras;		/* Raster stream */

  /* Set by start_page callback */
  unsigned		left, top, right, bottom;
					/* Image (print) box with origin at top left */
  unsigned		out_blanks;	/* Blank lines */
  size_t		out_length;	/* Output buffer size */
  unsigned char		*out_buffer;	/* Output (bit) buffer */
  unsigned char		*comp_buffer;	/* Compression buffer */

  /* Callbacks */
  void			(*end_job)(xform_raster_t *, xform_write_cb_t, void *);
  void			(*end_page)(xform_raster_t *, unsigned, xform_write_cb_t, void *);
  void			(*start_job)(xform_raster_t *, xform_write_cb_t, void *);
  void			(*start_page)(xform_raster_t *, unsigned, xform_write_cb_t, void *);
  void			(*write_line)(xform_raster_t *, unsigned, const unsigned char *, xform_write_cb_t, void *);
};


/*
 * Local globals...
 */

static int	Verbosity = 0;		/* Log level */


/*
 * Local functions...
 */

static int	load_env_options(cups_option_t **options);
static void	pack_pixels(unsigned char *row, size_t num_pixels);
static void	pcl_end_job(xform_raster_t *ras, xform_write_cb_t cb, void *ctx);
static void	pcl_end_page(xform_raster_t *ras, unsigned page, xform_write_cb_t cb, void *ctx);
static void	pcl_init(xform_raster_t *ras);
static void	pcl_printf(xform_write_cb_t cb, void *ctx, const char *format, ...) __attribute__ ((__format__ (__printf__, 3, 4)));
static void	pcl_start_job(xform_raster_t *ras, xform_write_cb_t cb, void *ctx);
static void	pcl_start_page(xform_raster_t *ras, unsigned page, xform_write_cb_t cb, void *ctx);
static void	pcl_write_line(xform_raster_t *ras, unsigned y, const unsigned char *line, xform_write_cb_t cb, void *ctx);
static void	raster_end_job(xform_raster_t *ras, xform_write_cb_t cb, void *ctx);
static void	raster_end_page(xform_raster_t *ras, unsigned page, xform_write_cb_t cb, void *ctx);
static void	raster_init(xform_raster_t *ras);
static void	raster_start_job(xform_raster_t *ras, xform_write_cb_t cb, void *ctx);
static void	raster_start_page(xform_raster_t *ras, unsigned page, xform_write_cb_t cb, void *ctx);
static void	raster_write_line(xform_raster_t *ras, unsigned y, const unsigned char *line, xform_write_cb_t cb, void *ctx);
static void	usage(int status) __attribute__((noreturn));
static ssize_t	write_fd(int *fd, const unsigned char *buffer, size_t bytes);
static int	xform_jpeg(const char *filename, const char *format, const char *resolutions, const char *types, int num_options, cups_option_t *options, xform_write_cb_t cb, void *ctx);
static int	xform_pdf(const char *filename, const char *format, const char *resolutions, const char *types, const char *sheet_back, int num_options, cups_option_t *options, xform_write_cb_t cb, void *ctx);
static int	xform_setup(xform_raster_t *ras, const char *format, const char *resolutions, const char *types, const char *sheet_back, int color, unsigned pages, int num_options, cups_option_t *options);


/*
 * 'main()' - Main entry for transform utility.
 */

int					/* O - Exit status */
main(int  argc,				/* I - Number of command-line args */
     char *argv[])			/* I - Command-line arguments */
{
  int		i;			/* Looping var */
  const char	*filename = NULL,	/* File to transform */
		*content_type,		/* Source content type */
		*device_uri,		/* Destination URI */
		*output_type,		/* Destination content type */
		*resolutions,		/* pwg-raster-document-resolution-supported */
		*sheet_back,		/* pwg-raster-document-sheet-back */
		*types,			/* pwg-raster-document-type-supported */
		*opt;			/* Option character */
  int		num_options;		/* Number of options */
  cups_option_t	*options;		/* Options */
  int		fd = 1;			/* Output file/socket */
  int		status = 0;		/* Exit status */


 /*
  * Process the command-line...
  */

  num_options  = load_env_options(&options);
  content_type = getenv("CONTENT_TYPE");
  device_uri   = getenv("DEVICE_URI");
  output_type  = getenv("OUTPUT_TYPE");
  resolutions  = getenv("PWG_RASTER_DOCUMENT_RESOLUTION_SUPPORTED");
  sheet_back   = getenv("PWG_RASTER_DOCUMENT_SHEET_BACK");
  types        = getenv("PWG_RASTER_DOCUMENT_TYPE_SUPPORTED");

  if ((opt = getenv("SERVER_LOGLEVEL")) != NULL)
  {
    if (!strcmp(opt, "debug"))
      Verbosity = 2;
    else if (!strcmp(opt, "info"))
      Verbosity = 1;
  }

  for (i = 1; i < argc; i ++)
  {
    if (argv[i][0] == '-' && argv[i][1] != '-')
    {
      for (opt = argv[i] + 1; *opt; opt ++)
      {
        switch (*opt)
	{
	  case 'd' :
	      i ++;
	      if (i >= argc)
	        usage(1);

	      device_uri = argv[i];
	      break;

	  case 'i' :
	      i ++;
	      if (i >= argc)
	        usage(1);

	      content_type = argv[i];
	      break;

	  case 'm' :
	      i ++;
	      if (i >= argc)
	        usage(1);

	      output_type = argv[i];
	      break;

	  case 'o' :
	      i ++;
	      if (i >= argc)
	        usage(1);

	      num_options = cupsParseOptions(argv[i], num_options, &options);
	      break;

	  case 'r' : /* pwg-raster-document-resolution-supported values */
	      i ++;
	      if (i >= argc)
	        usage(1);

	      resolutions = argv[i];
	      break;

	  case 's' : /* pwg-raster-document-sheet-back value */
	      i ++;
	      if (i >= argc)
	        usage(1);

	      sheet_back = argv[i];
	      break;

	  case 't' : /* pwg-raster-document-type-supported values */
	      i ++;
	      if (i >= argc)
	        usage(1);

	      types = argv[i];
	      break;

	  case 'v' : /* Be verbose... */
	      Verbosity ++;
	      break;

	  default :
	      fprintf(stderr, "ERROR: Unknown option '-%c'.\n", *opt);
	      usage(1);
	      break;
	}
      }
    }
    else if (!strcmp(argv[i], "--help"))
      usage(0);
    else if (!strncmp(argv[i], "--", 2))
    {
      fprintf(stderr, "ERROR: Unknown option '%s'.\n", argv[i]);
      usage(1);
    }
    else if (!filename)
      filename = argv[i];
    else
      usage(1);
  }

 /*
  * Check that we have everything we need...
  */

  if (!filename)
    usage(1);

  if (!content_type)
  {
    if ((opt = strrchr(filename, '.')) != NULL)
    {
      if (!strcmp(opt, ".pdf"))
        content_type = "application/pdf";
      else if (!strcmp(opt, ".jpg") || !strcmp(opt, ".jpeg"))
        content_type = "image/jpeg";
    }
  }

  if (!content_type)
  {
    fprintf(stderr, "ERROR: Unknown format for \"%s\", please specify with '-i' option.\n", filename);
    usage(1);
  }
  else if (strcmp(content_type, "application/pdf") && strcmp(content_type, "image/jpeg"))
  {
    fprintf(stderr, "ERROR: Unsupported format \"%s\" for \"%s\".\n", content_type, filename);
    usage(1);
  }

  if (!output_type)
  {
    fputs("ERROR: Unknown output format, please specify with '-m' option.\n", stderr);
    usage(1);
  }
  else if (strcmp(output_type, "application/vnd.hp-pcl") && strcmp(output_type, "image/pwg-raster"))
  {
    fprintf(stderr, "ERROR: Unsupported output format \"%s\".\n", output_type);
    usage(1);
  }

  if (!resolutions)
    resolutions = "300dpi";
  if (!sheet_back)
    sheet_back = "normal";
  if (!types)
    types = "sgray_8";

 /*
  * If the device URI is specified, open the connection...
  */

  if (device_uri)
  {
    char	scheme[32],		/* URI scheme */
		userpass[256],		/* URI user:pass */
		host[256],		/* URI host */
		resource[1024],		/* URI resource path */
		service[32];		/* Service port */
    int		port;			/* URI port number */
    http_addrlist_t *list;		/* Address list for socket */

    if (httpSeparateURI(HTTP_URI_CODING_ALL, device_uri, scheme, sizeof(scheme), userpass, sizeof(userpass), host, sizeof(host), &port, resource, sizeof(resource)) < HTTP_URI_STATUS_OK)
    {
      fprintf(stderr, "ERROR: Invalid device URI \"%s\".\n", device_uri);
      usage(1);
    }

    if (strcmp(scheme, "socket"))
    {
      fprintf(stderr, "ERROR: Unsupported device URI scheme \"%s\".\n", scheme);
      usage(1);
    }

    snprintf(service, sizeof(service), "%d", port);
    if ((list = httpAddrGetList(host, AF_UNSPEC, service)) == NULL)
    {
      fprintf(stderr, "ERROR: Unable to lookup device URI host \"%s\": %s\n", host, cupsLastErrorString());
      usage(1);
    }

    if (!httpAddrConnect2(list, &fd, 30000, NULL))
    {
      fprintf(stderr, "ERROR: Unable to connect to \"%s\" on port %d: %s\n", host, port, cupsLastErrorString());
      usage(1);
    }
  }

 /*
  * Do transform...
  */

  if (!strcmp(content_type, "application/pdf"))
    status = xform_pdf(filename, output_type, resolutions, types, sheet_back, num_options, options, (xform_write_cb_t)write_fd, &fd);
  else
    status = xform_jpeg(filename, output_type, resolutions, types, num_options, options, (xform_write_cb_t)write_fd, &fd);

  if (fd != 1)
    close(fd);

  return (status);
}


/*
 * 'load_env_options()' - Load options from the environment.
 */

extern char **environ;

static int				/* O - Number of options */
load_env_options(
    cups_option_t **options)		/* I - Options */
{
  int	i;				/* Looping var */
  char	name[256],			/* Option name */
	*nameptr,			/* Pointer into name */
	*envptr;			/* Pointer into environment variable */
  int	num_options = 0;		/* Number of options */


  *options = NULL;

 /*
  * Load all of the IPP_xxx environment variables as options...
  */

  for (i = 0; environ[i]; i ++)
  {
    envptr = environ[i];

    if (strncmp(envptr, "IPP_", 4))
      continue;

    for (nameptr = name, envptr += 4; *envptr && *envptr != '='; envptr ++)
    {
      if (nameptr > (name + sizeof(name) - 1))
        continue;

      if (*envptr == '_')
        *nameptr++ = '-';
      else
        *nameptr++ = (char)_cups_tolower(*envptr);
    }

    *nameptr = '\0';
    if (*envptr == '=')
      envptr ++;

    num_options = cupsAddOption(name, envptr, num_options, options);
  }

  return (num_options);
}


/*
 * 'pack_pixels()' - Pack RGBX scanlines into RGB scanlines.
 *
 * This routine is suitable only for 8 bit RGBX data packed into RGB bytes.
 */

static void
pack_pixels(unsigned char *row,		/* I - Row of pixels to pack */
	    size_t        num_pixels)	/* I - Number of pixels in row */
{
  size_t	num_quads = num_pixels / 4;
					/* Number of 4 byte samples to pack */
  size_t	leftover_pixels = num_pixels & 3;
					/* Number of pixels remaining */
  UInt32	*quad_row = (UInt32 *)row;
					/* 32-bit pixel pointer */
  UInt32	*dest = quad_row;	/* Destination pointer */
  unsigned char *src_byte;		/* Remaining source bytes */
  unsigned char *dest_byte;		/* Remaining destination bytes */


 /*
  * Copy all of the groups of 4 pixels we can...
  */

  while (num_quads > 0)
  {
    *dest++ = (quad_row[0] & XFORM_RGB_MASK) | (quad_row[1] << 24);
    *dest++ = ((quad_row[1] & XFORM_BG_MASK) >> 8) |
              ((quad_row[2] & XFORM_RG_MASK) << 16);
    *dest++ = ((quad_row[2] & XFORM_BLUE_MASK) >> 16) | (quad_row[3] << 8);
    quad_row += 4;
    num_quads --;
  }

 /*
  * Then handle the leftover pixels...
  */

  src_byte  = (unsigned char *)quad_row;
  dest_byte = (unsigned char *)dest;

  while (leftover_pixels > 0)
  {
    *dest_byte++ = *src_byte++;
    *dest_byte++ = *src_byte++;
    *dest_byte++ = *src_byte++;
    src_byte ++;
    leftover_pixels --;
  }
}


/*
 * 'pcl_end_job()' - End a PCL "job".
 */

static void
pcl_end_job(xform_raster_t   *ras,	/* I - Raster information */
            xform_write_cb_t cb,	/* I - Write callback */
            void             *ctx)	/* I - Write context */
{
  (void)ras;

 /*
  * Send a PCL reset sequence.
  */

  (*cb)(ctx, (const unsigned char *)"\033E", 2);
}


/*
 * 'pcl_end_page()' - End of PCL page.
 */

static void
pcl_end_page(xform_raster_t   *ras,	/* I - Raster information */
	     unsigned         page,	/* I - Current page */
             xform_write_cb_t cb,	/* I - Write callback */
             void             *ctx)	/* I - Write context */
{
 /*
  * End graphics...
  */

  (*cb)(ctx, (const unsigned char *)"\033*r0B", 5);

 /*
  * Formfeed as needed...
  */

  if (!(ras->header.Duplex && (page & 1)))
    (*cb)(ctx, (const unsigned char *)"\014", 1);

 /*
  * Free the output buffer...
  */

  free(ras->out_buffer);
  ras->out_buffer = NULL;
}


/*
 * 'pcl_init()' - Initialize callbacks for PCL output.
 */

static void
pcl_init(xform_raster_t *ras)		/* I - Raster information */
{
  ras->end_job    = pcl_end_job;
  ras->end_page   = pcl_end_page;
  ras->start_job  = pcl_start_job;
  ras->start_page = pcl_start_page;
  ras->write_line = pcl_write_line;
}


/*
 * 'pcl_printf()' - Write a formatted string.
 */

static void
pcl_printf(xform_write_cb_t cb,		/* I - Write callback */
           void             *ctx,	/* I - Write context */
	   const char       *format,	/* I - Printf-style format string */
	   ...)				/* I - Additional arguments as needed */
{
  va_list	ap;			/* Argument pointer */
  char		buffer[8192];		/* Buffer */


  va_start(ap, format);
  vsnprintf(buffer, sizeof(buffer), format, ap);
  va_end(ap);

  (*cb)(ctx, (const unsigned char *)buffer, strlen(buffer));
}


/*
 * 'pcl_start_job()' - Start a PCL "job".
 */

static void
pcl_start_job(xform_raster_t   *ras,	/* I - Raster information */
              xform_write_cb_t cb,	/* I - Write callback */
              void             *ctx)	/* I - Write context */
{
  (void)ras;

 /*
  * Send a PCL reset sequence.
  */

  (*cb)(ctx, (const unsigned char *)"\033E", 2);
}


/*
 * 'pcl_start_page()' - Start a PCL page.
 */

static void
pcl_start_page(xform_raster_t   *ras,	/* I - Raster information */
               unsigned         page,	/* I - Current page */
               xform_write_cb_t cb,	/* I - Write callback */
               void             *ctx)	/* I - Write context */
{
 /*
  * Setup margins to be 1/6" top and bottom and 1/4" or .135" on the
  * left and right.
  */

  ras->top    = ras->header.HWResolution[1] / 6;
  ras->bottom = ras->header.cupsHeight - ras->header.HWResolution[1] / 6 - 1;

  if (ras->header.PageSize[1] == 842)
  {
   /* A4 gets special side margins to expose an 8" print area */
    ras->left  = (ras->header.cupsWidth - 8 * ras->header.HWResolution[0]) / 2;
    ras->right = ras->left + 8 * ras->header.HWResolution[0] - 1;
  }
  else
  {
   /* All other sizes get 1/4" margins */
    ras->left  = ras->header.HWResolution[0] / 4;
    ras->right = ras->header.cupsWidth - ras->header.HWResolution[0] / 4 - 1;
  }

  if (!ras->header.Duplex || (page & 1))
  {
   /*
    * Set the media size...
    */

    pcl_printf(cb, ctx, "\033&l12D\033&k12H");
					/* Set 12 LPI, 10 CPI */
    pcl_printf(cb, ctx, "\033&l0O");	/* Set portrait orientation */

    switch (ras->header.PageSize[1])
    {
      case 540 : /* Monarch Envelope */
          pcl_printf(cb, ctx, "\033&l80A");
	  break;

      case 595 : /* A5 */
          pcl_printf(cb, ctx, "\033&l25A");
	  break;

      case 624 : /* DL Envelope */
          pcl_printf(cb, ctx, "\033&l90A");
	  break;

      case 649 : /* C5 Envelope */
          pcl_printf(cb, ctx, "\033&l91A");
	  break;

      case 684 : /* COM-10 Envelope */
          pcl_printf(cb, ctx, "\033&l81A");
	  break;

      case 709 : /* B5 Envelope */
          pcl_printf(cb, ctx, "\033&l100A");
	  break;

      case 756 : /* Executive */
          pcl_printf(cb, ctx, "\033&l1A");
	  break;

      case 792 : /* Letter */
          pcl_printf(cb, ctx, "\033&l2A");
	  break;

      case 842 : /* A4 */
          pcl_printf(cb, ctx, "\033&l26A");
	  break;

      case 1008 : /* Legal */
          pcl_printf(cb, ctx, "\033&l3A");
	  break;

      case 1191 : /* A3 */
          pcl_printf(cb, ctx, "\033&l27A");
	  break;

      case 1224 : /* Tabloid */
          pcl_printf(cb, ctx, "\033&l6A");
	  break;
    }

   /*
    * Set top margin and turn off perforation skip...
    */

    pcl_printf(cb, ctx, "\033&l%uE\033&l0L", 12 * ras->top / ras->header.HWResolution[1]);

    if (ras->header.Duplex)
    {
      int mode = ras->header.Duplex ? 1 + ras->header.Tumble != 0 : 0;

      pcl_printf(cb, ctx, "\033&l%dS", mode);
					/* Set duplex mode */
    }
  }
  else if (ras->header.Duplex)
    pcl_printf(cb, ctx, "\033&a2G");	/* Print on back side */

 /*
  * Set graphics mode...
  */

  pcl_printf(cb, ctx, "\033*t%uR", ras->header.HWResolution[0]);
					/* Set resolution */
  pcl_printf(cb, ctx, "\033*r%uS", ras->right - ras->left + 1);
					/* Set width */
  pcl_printf(cb, ctx, "\033*r%uT", ras->bottom - ras->top + 1);
					/* Set height */
  pcl_printf(cb, ctx, "\033&a0H\033&a%uV", 720 * ras->top / ras->header.HWResolution[1]);
					/* Set position */

  pcl_printf(cb, ctx, "\033*b2M");	/* Use PackBits compression */
  pcl_printf(cb, ctx, "\033*r1A");	/* Start graphics */

 /*
  * Allocate the output buffer...
  */

  ras->out_blanks  = 0;
  ras->out_length  = (ras->right - ras->left + 8) / 8;
  ras->out_buffer  = malloc(ras->out_length);
  ras->comp_buffer = malloc(2 * ras->out_length + 2);
}


/*
 * 'pcl_write_line()' - Write a line of raster data.
 */

static void
pcl_write_line(
    xform_raster_t      *ras,		/* I - Raster information */
    unsigned            y,		/* I - Line number */
    const unsigned char *line,		/* I - Pixels on line */
    xform_write_cb_t    cb,		/* I - Write callback */
    void                *ctx)		/* I - Write context */
{
  unsigned	x;			/* Column number */
  unsigned char	bit,			/* Current bit */
		byte,			/* Current byte */
		*outptr,		/* Pointer into output buffer */
		*outend,		/* End of output buffer */
		*start,			/* Start of sequence */
		*compptr;		/* Pointer into compression buffer */
  unsigned	count;			/* Count of bytes for output */


  if (line[0] == 255 && !memcmp(line, line + 1, ras->right - ras->left))
  {
   /*
    * Skip blank line...
    */

    ras->out_blanks ++;
    return;
  }

 /*
  * Dither the line into the output buffer...
  */

  y &= 63;

  for (x = ras->left, bit = 128, byte = 0, outptr = ras->out_buffer; x <= ras->right; x ++, line ++)
  {
    if (*line <= threshold[x & 63][y])
      byte |= bit;

    if (bit == 1)
    {
      *outptr++ = byte;
      byte      = 0;
      bit       = 128;
    }
    else
      bit >>= 1;
  }

  if (bit != 128)
    *outptr++ = byte;

 /*
  * Apply compression...
  */

  compptr = ras->comp_buffer;
  outend  = outptr;
  outptr  = ras->out_buffer;

  while (outptr < outend)
  {
    if ((outptr + 1) >= outend)
    {
     /*
      * Single byte on the end...
      */

      *compptr++ = 0x00;
      *compptr++ = *outptr++;
    }
    else if (outptr[0] == outptr[1])
    {
     /*
      * Repeated sequence...
      */

      outptr ++;
      count = 2;

      while (outptr < (outend - 1) &&
	     outptr[0] == outptr[1] &&
	     count < 127)
      {
	outptr ++;
	count ++;
      }

      *compptr++ = (unsigned char)(257 - count);
      *compptr++ = *outptr++;
    }
    else
    {
     /*
      * Non-repeated sequence...
      */

      start = outptr;
      outptr ++;
      count = 1;

      while (outptr < (outend - 1) &&
	     outptr[0] != outptr[1] &&
	     count < 127)
      {
	outptr ++;
	count ++;
      }

      *compptr++ = (unsigned char)(count - 1);

      memcpy(compptr, start, count);
      compptr += count;
    }
  }

 /*
  * Output the line...
  */

  if (ras->out_blanks > 0)
  {
   /*
    * Skip blank lines first...
    */

    pcl_printf(cb, ctx, "\033*b%dY", ras->out_blanks);
    ras->out_blanks = 0;
  }

  pcl_printf(cb, ctx, "\033*b%dW", (int)(compptr - ras->comp_buffer));
  (*cb)(ctx, ras->comp_buffer, (size_t)(compptr - ras->comp_buffer));
}


/*
 * 'raster_end_job()' - End a raster "job".
 */

static void
raster_end_job(xform_raster_t   *ras,	/* I - Raster information */
	       xform_write_cb_t cb,	/* I - Write callback */
	       void             *ctx)	/* I - Write context */
{
  (void)cb;
  (void)ctx;

  cupsRasterClose(ras->ras);
}


/*
 * 'raster_end_page()' - End of raster page.
 */

static void
raster_end_page(xform_raster_t   *ras,	/* I - Raster information */
	        unsigned         page,	/* I - Current page */
		xform_write_cb_t cb,	/* I - Write callback */
		void             *ctx)	/* I - Write context */
{
  (void)ras;
  (void)page;
  (void)cb;
  (void)ctx;
}


/*
 * 'raster_init()' - Initialize callbacks for raster output.
 */

static void
raster_init(xform_raster_t *ras)	/* I - Raster information */
{
  ras->end_job    = raster_end_job;
  ras->end_page   = raster_end_page;
  ras->start_job  = raster_start_job;
  ras->start_page = raster_start_page;
  ras->write_line = raster_write_line;
}


/*
 * 'raster_start_job()' - Start a raster "job".
 */

static void
raster_start_job(xform_raster_t   *ras,	/* I - Raster information */
		 xform_write_cb_t cb,	/* I - Write callback */
		 void             *ctx)	/* I - Write context */
{
  ras->ras = cupsRasterOpenIO((cups_raster_iocb_t)cb, ctx, CUPS_RASTER_WRITE_PWG);
}


/*
 * 'raster_start_page()' - Start a raster page.
 */

static void
raster_start_page(xform_raster_t   *ras,/* I - Raster information */
		  unsigned         page,/* I - Current page */
		  xform_write_cb_t cb,	/* I - Write callback */
		  void             *ctx)/* I - Write context */
{
  (void)cb;
  (void)ctx;

  ras->left   = 0;
  ras->top    = 0;
  ras->right  = ras->header.cupsWidth - 1;
  ras->bottom = ras->header.cupsHeight - 1;

  if (ras->header.Duplex && !(page & 1))
    cupsRasterWriteHeader2(ras->ras, &ras->back_header);
  else
    cupsRasterWriteHeader2(ras->ras, &ras->header);
}


/*
 * 'raster_write_line()' - Write a line of raster data.
 */

static void
raster_write_line(
    xform_raster_t      *ras,		/* I - Raster information */
    unsigned            y,		/* I - Line number */
    const unsigned char *line,		/* I - Pixels on line */
    xform_write_cb_t    cb,		/* I - Write callback */
    void                *ctx)		/* I - Write context */
{
  (void)y;
  (void)cb;
  (void)ctx;

  cupsRasterWritePixels(ras->ras, (unsigned char *)line, ras->header.cupsBytesPerLine);
}


/*
 * 'usage()' - Show program usage.
 */

static void
usage(int status)			/* I - Exit status */
{
  puts("Usage: ipptransform [options] filename");
  puts("Options:");
  puts("  --help");
  puts("  -d device-uri");
  puts("  -i input/format");
  puts("  -m output/format");
  puts("  -o \"name=value [... name=value]\"");
  puts("  -r resolution[,...,resolution]");
  puts("  -s {flipped|manual-tumble|normal|rotated}");
  puts("  -t sgray_8[,srgb_8]");
  puts("  -v");

  exit(status);
}


/*
 * 'write_fd()' - Write to a file/socket.
 */

static ssize_t				/* O - Number of bytes written or -1 on error */
write_fd(int                 *fd,	/* I - File descriptor */
         const unsigned char *buffer,	/* I - Buffer */
         size_t              bytes)	/* I - Number of bytes to write */
{
  ssize_t	temp,			/* Temporary byte count */
		total = 0;		/* Total bytes written */


  while (bytes > 0)
  {
    if ((temp = write(*fd, buffer, bytes)) < 0)
    {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      else
        return (-1);
    }

    total  += temp;
    bytes  -= (size_t)temp;
    buffer += temp;
  }

  return (total);
}


/*
 * 'xform_jpeg()' - Transform a JPEG image for printing.
 */

static int				/* O - 0 on success, 1 on error */
xform_jpeg(const char       *filename,	/* I - File to transform */
           const char       *format,	/* I - Output format (MIME media type) */
           const char       *resolutions,/* I - Supported resolutions */
	   const char       *types,	/* I - Supported types */
           int              num_options,/* I - Number of options */
           cups_option_t    *options,	/* I - Options */
           xform_write_cb_t cb,		/* I - Write callback */
           void             *ctx)	/* I - Write context */
{
  // TODO: Implement me
  (void)filename;
  (void)format;
  (void)resolutions;
  (void)types;
  (void)num_options;
  (void)options;
  (void)cb;
  (void)ctx;

  return (1);
}


/*
 * 'xform_pdf()' - Transform a PDF file for printing.
 */

static int				/* O - 0 on success, 1 on error */
xform_pdf(const char       *filename,	/* I - File to transform */
          const char       *format,	/* I - Output format (MIME media type) */
          const char       *resolutions,/* I - Supported resolutions */
	  const char       *types,	/* I - Supported types */
	  const char       *sheet_back,	/* I - Back side transform */
          int              num_options,	/* I - Number of options */
          cups_option_t    *options,	/* I - Options */
          xform_write_cb_t cb,		/* I - Write callback */
          void             *ctx)	/* I - Write context */
{
  CFURLRef		url;		/* CFURL object for PDF filename */
  CGPDFDocumentRef	document= NULL;	/* Input document */
  CGPDFPageRef		pdf_page;	/* Page in PDF file */
  xform_raster_t	ras;		/* Raster info */
  CGColorSpaceRef	cs;		/* Quartz color space */
  CGContextRef		context;	/* Quartz bitmap context */
  CGBitmapInfo		info;		/* Bitmap flags */
  size_t		band_size;	/* Size of band line */
  double		xscale, yscale;	/* Scaling factor */
  CGAffineTransform 	transform;	/* Transform for page */
  CGAffineTransform	back_transform;	/* Transform for back side */
  CGRect		dest;		/* Destination rectangle */

  unsigned		pages = 1;	/* Number of pages */
  int			color = 1;	/* Does the PDF have color? */
//  const char		*page_ranges;	/* "page-ranges" option */
  unsigned		copy;		/* Current copy */
  unsigned		page;		/* Current page */
  unsigned		media_sheets = 0,
			impressions = 0;/* Page/sheet counters */


 /*
  * Open the PDF file...
  */

  if ((url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)filename, (CFIndex)strlen(filename), false)) == NULL)
  {
    fputs("ERROR: Unable to create CFURL for file.\n", stderr);
    return (1);
  }

  document = CGPDFDocumentCreateWithURL(url);
  CFRelease(url);

  if (!document)
  {
    fputs("ERROR: Unable to create CFPDFDocument for file.\n", stderr);
    return (1);
  }

  if (CGPDFDocumentIsEncrypted(document))
  {
   /*
    * Only support encrypted PDFs with a blank password...
    */

    if (!CGPDFDocumentUnlockWithPassword(document, ""))
    {
      fputs("ERROR: Document is encrypted and cannot be unlocked.\n", stderr);
      CGPDFDocumentRelease(document);
      return (1);
    }
  }

  if (!CGPDFDocumentAllowsPrinting(document))
  {
    fputs("ERROR: Document does not allow printing.\n", stderr);
    CGPDFDocumentRelease(document);
    return (1);
  }

  pages = (unsigned)CGPDFDocumentGetNumberOfPages(document);
  /* TODO: Support page-ranges */

 /*
  * Setup the raster context...
  */

  if (xform_setup(&ras, format, resolutions, types, sheet_back, color, pages, num_options, options))
  {
    CGPDFDocumentRelease(document);
    return (1);
  }

  if (ras.header.cupsBitsPerPixel == 8)
  {
   /*
    * Grayscale output...
    */

    ras.band_bpp = 1;
    info         = kCGImageAlphaNone;
    cs           = CGColorSpaceCreateWithName(kCGColorSpaceGenericGrayGamma2_2);
  }
  else
  {
   /*
    * Color (sRGB) output...
    */

    ras.band_bpp = 4;
    info         = kCGImageAlphaNoneSkipLast;
    cs           = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  }


  band_size = ras.header.cupsWidth * ras.band_bpp;
  if ((ras.band_height = XFORM_MAX_RASTER / band_size) < 1)
    ras.band_height = 1;
  else if (ras.band_height > ras.header.cupsHeight)
    ras.band_height = ras.header.cupsHeight;

  ras.band_buffer = malloc(ras.band_height * band_size);
  context         = CGBitmapContextCreate(ras.band_buffer, ras.header.cupsWidth, ras.band_height, 8, band_size, cs, info);

  CGColorSpaceRelease(cs);

  /* Don't anti-alias or interpolate when creating raster data */
  CGContextSetAllowsAntialiasing(context, 0);
  CGContextSetInterpolationQuality(context, kCGInterpolationNone);

  xscale = ras.header.HWResolution[0] / 72.0;
  yscale = ras.header.HWResolution[1] / 72.0;

  if (Verbosity > 1)
    fprintf(stderr, "DEBUG: xscale=%g, yscale=%g\n", xscale, yscale);
  CGContextScaleCTM(context, xscale, yscale);

  if (Verbosity > 1)
    fprintf(stderr, "DEBUG: Band height=%u, page height=%u, page translate 0.0,%g\n", ras.band_height, ras.header.cupsHeight, -1.0 * (ras.header.cupsHeight - ras.band_height) / yscale);
  CGContextTranslateCTM(context, 0.0, -1.0 * (ras.header.cupsHeight - ras.band_height) / yscale);

  dest.origin.x    = dest.origin.y = 0.0;
  dest.size.width  = ras.header.cupsWidth * 72.0 / ras.header.HWResolution[0];
  dest.size.height = ras.header.cupsHeight * 72.0 / ras.header.HWResolution[1];

 /*
  * Setup the back page transform, if any...
  */

  if (sheet_back)
  {
    if (!strcmp(sheet_back, "flipped"))
    {
      if (ras.header.Tumble)
        back_transform = CGAffineTransformMake(-1, 0, 0, 1, ras.header.cupsPageSize[0], 0);
      else
        back_transform = CGAffineTransformMake(1, 0, 0, -1, 0, ras.header.cupsPageSize[1]);
    }
    else if (!strcmp(sheet_back, "manual-tumble") && ras.header.Tumble)
      back_transform = CGAffineTransformMake(-1, 0, 0, -1, ras.header.cupsPageSize[0], ras.header.cupsPageSize[1]);
    else if (!strcmp(sheet_back, "rotated") && !ras.header.Tumble)
      back_transform = CGAffineTransformMake(-1, 0, 0, -1, ras.header.cupsPageSize[0], ras.header.cupsPageSize[1]);
    else
      back_transform = CGAffineTransformMake(1, 0, 0, 1, 0, 0);
  }
  else
    back_transform = CGAffineTransformMake(1, 0, 0, 1, 0, 0);

  if (Verbosity > 1)
    fprintf(stderr, "DEBUG: cupsPageSize=[%g %g]\n", ras.header.cupsPageSize[0], ras.header.cupsPageSize[1]);
  if (Verbosity > 1)
    fprintf(stderr, "DEBUG: back_transform=[%g %g %g %g %g %g]\n", back_transform.a, back_transform.b, back_transform.c, back_transform.d, back_transform.tx, back_transform.ty);

 /*
  * Draw all of the pages...
  */

  (*(ras.start_job))(&ras, cb, ctx);

  for (copy = 0; copy < ras.copies; copy ++)
  {
    for (page = 1; page <= pages; page ++)
    {
      unsigned		y,		/* Current line */
			band_starty = 0,/* Start line of band */
			band_endy = 0;	/* End line of band */
      unsigned char	*lineptr;	/* Pointer to line */

      pdf_page  = CGPDFDocumentGetPage(document, page);
      transform = CGPDFPageGetDrawingTransform(pdf_page, kCGPDFCropBox,dest, 0, true);

      if (Verbosity > 1)
        fprintf(stderr, "DEBUG: Printing copy %d/%d, page %d/%d, transform=[%g %g %g %g %g %g]\n", copy + 1, ras.copies, page, pages, transform.a, transform.b, transform.c, transform.d, transform.tx, transform.ty);

      (*(ras.start_page))(&ras, page, cb, ctx);

      for (y = ras.top; y <= ras.bottom; y ++)
      {
	if (y >= band_endy)
	{
	 /*
	  * Draw the next band of raster data...
	  */

	  band_starty = y;
	  band_endy   = y + ras.band_height;
	  if (band_endy > ras.bottom)
	    band_endy = ras.bottom;

	  if (Verbosity > 1)
	    fprintf(stderr, "DEBUG: Drawing band from %u to %u.\n", band_starty, band_endy);

	  CGContextSaveGState(context);
	    if (ras.header.cupsNumColors == 1)
	      CGContextSetGrayFillColor(context, 1., 1.);
	    else
	      CGContextSetRGBFillColor(context, 1., 1., 1., 1.);

	    CGContextSetCTM(context, CGAffineTransformIdentity);
	    CGContextFillRect(context, CGRectMake(0., 0., ras.header.cupsWidth, ras.band_height));
	  CGContextRestoreGState(context);

	  CGContextSaveGState(context);
	    if (Verbosity > 1)
	      fprintf(stderr, "DEBUG: Band translate 0.0,%g\n", y / yscale);
	    CGContextTranslateCTM(context, 0.0, y / yscale);
	    if (!(page & 1) && ras.header.Duplex)
	      CGContextConcatCTM(context, back_transform);
	    CGContextConcatCTM(context, transform);

	    CGContextClipToRect(context, CGPDFPageGetBoxRect(pdf_page, kCGPDFCropBox));
	    CGContextDrawPDFPage(context, pdf_page);
	  CGContextRestoreGState(context);
	}

       /*
	* Prepare and write a line...
	*/

	lineptr = ras.band_buffer + (y - band_starty) * band_size + ras.left * ras.band_bpp;
	if (ras.band_bpp == 4)
	  pack_pixels(lineptr, ras.right - ras.left + 1);

	(*(ras.write_line))(&ras, y, lineptr, cb, ctx);
      }

      (*(ras.end_page))(&ras, page, cb, ctx);

      impressions ++;
      fprintf(stderr, "ATTR: job-impressions-completed=%u\n", impressions);
      if (!ras.header.Duplex || !(page & 1))
      {
        media_sheets ++;
	fprintf(stderr, "ATTR: job-media-sheets-completed=%u\n", media_sheets);
      }
    }

    if (ras.copies > 1 && (pages & 1) && ras.header.Duplex)
    {
     /*
      * Duplex printing, add a blank back side image...
      */

      unsigned		y;		/* Current line */

      if (Verbosity > 1)
        fprintf(stderr, "DEBUG: Printing blank page %u for duplex.\n", pages + 1);

      memset(ras.band_buffer, 255, ras.header.cupsBytesPerLine);

      (*(ras.start_page))(&ras, page, cb, ctx);

      for (y = ras.top; y < ras.bottom; y ++)
	(*(ras.write_line))(&ras, y, ras.band_buffer, cb, ctx);

      (*(ras.end_page))(&ras, page, cb, ctx);

      impressions ++;
      fprintf(stderr, "ATTR: job-impressions-completed=%u\n", impressions);
      if (!ras.header.Duplex || !(page & 1))
      {
        media_sheets ++;
	fprintf(stderr, "ATTR: job-media-sheets-completed=%u\n", media_sheets);
      }
    }
  }

  (*(ras.end_job))(&ras, cb, ctx);

  CGPDFDocumentRelease(document);
  CGContextRelease(context);

  return (0);
}


/*
 * 'xform_setup()' - Setup a raster context for printing.
 */

static int				/* O - 0 on success, -1 on failure */
xform_setup(xform_raster_t *ras,	/* I - Raster information */
            const char     *format,	/* I - Output format (MIME media type) */
	    const char     *resolutions,/* I - Supported resolutions */
	    const char     *types,	/* I - Supported types */
	    const char     *sheet_back,	/* I - Back side transform */
	    int            color,	/* I - Document contains color? */
            unsigned       pages,	/* I - Number of pages */
            int            num_options,	/* I - Number of options */
            cups_option_t  *options)	/* I - Options */
{
  const char	*copies,		/* "copies" option */
		*media,			/* "media" option */
		*media_col;		/* "media-col" option */
  pwg_media_t	*pwg_media = NULL;	/* PWG media value */
  const char	*print_quality,		/* "print-quality" option */
		*printer_resolution,	/* "printer-resolution" option */
		*sides,			/* "sides" option */
		*type;			/* Raster type to use */
  int		xdpi, ydpi;		/* Resolution to use */
  cups_array_t	*res_array,		/* Resolutions in array */
		*type_array;		/* Types in array */


 /*
  * Initialize raster information...
  */

  memset(ras, 0, sizeof(xform_raster_t));

  ras->num_options = num_options;
  ras->options     = options;

  if (!strcmp(format, "application/vnd.hp-pcl"))
    pcl_init(ras);
  else
    raster_init(ras);

 /*
  * Get the number of copies...
  */

  if ((copies = cupsGetOption("copies", num_options, options)) != NULL)
  {
    int temp = atoi(copies);		/* Copies value */

    if (temp < 1 || temp > 9999)
    {
      fprintf(stderr, "ERROR: Invalid \"copies\" value '%s'.\n", copies);
      return (-1);
    }

    ras->copies = (unsigned)temp;
  }
  else
    ras->copies = 1;

 /*
  * Figure out the media size...
  */

  if ((media = cupsGetOption("media", num_options, options)) != NULL)
  {
    if ((pwg_media = pwgMediaForPWG(media)) == NULL)
      pwg_media = pwgMediaForLegacy(media);

    if (!pwg_media)
    {
      fprintf(stderr, "ERROR: Unknown \"media\" value '%s'.\n", media);
      return (-1);
    }
  }
  else if ((media_col = cupsGetOption("media-col", num_options, options)) != NULL)
  {
    int			num_cols;	/* Number of collection values */
    cups_option_t	*cols;		/* Collection values */
    const char		*media_size_name,
			*media_size;	/* Collection attributes */

    num_cols = cupsParseOptions(media_col, 0, &cols);
    if ((media_size_name = cupsGetOption("media-size-name", num_cols, cols)) != NULL)
    {
      if ((pwg_media = pwgMediaForPWG(media_size_name)) == NULL)
      {
	fprintf(stderr, "ERROR: Unknown \"media-size-name\" value '%s'.\n", media_size_name);
	cupsFreeOptions(num_cols, cols);
	return (-1);
      }
    }
    else if ((media_size = cupsGetOption("media-size", num_cols, cols)) != NULL)
    {
      int		num_sizes;	/* Number of collection values */
      cups_option_t	*sizes;		/* Collection values */
      const char	*x_dim,		/* Collection attributes */
			*y_dim;

      num_sizes = cupsParseOptions(media_size, 0, &sizes);
      if ((x_dim = cupsGetOption("x-dimension", num_sizes, sizes)) != NULL && (y_dim = cupsGetOption("y-dimension", num_sizes, sizes)) != NULL)
      {
        pwg_media = pwgMediaForSize(atoi(x_dim), atoi(y_dim));
      }
      else
      {
        fprintf(stderr, "ERROR: Bad \"media-size\" value '%s'.\n", media_size);
	cupsFreeOptions(num_sizes, sizes);
	cupsFreeOptions(num_cols, cols);
	return (-1);
      }

      cupsFreeOptions(num_sizes, sizes);
    }

    cupsFreeOptions(num_cols, cols);
  }

  if (!pwg_media)
  {
   /*
    * Use default size...
    */

    const char	*media_default = getenv("PRINTER_MEDIA_DEFAULT");
				/* "media-default" value */

    if (!media_default)
      media_default = "na_letter_8.5x11in";

    if ((pwg_media = pwgMediaForPWG(media_default)) == NULL)
    {
      fprintf(stderr, "ERROR: Unknown \"media-default\" value '%s'.\n", media_default);
      return (-1);
    }
  }

 /*
  * Figure out the proper resolution, etc.
  */

  res_array = _cupsArrayNewStrings(resolutions, ',');

  if ((printer_resolution = cupsGetOption("printer-resolution", num_options, options)) != NULL && !cupsArrayFind(res_array, (void *)printer_resolution))
  {
    if (Verbosity)
      fprintf(stderr, "INFO: Unsupported \"printer-resolution\" value '%s'.\n", printer_resolution);
    printer_resolution = NULL;
  }

  if (!printer_resolution)
  {
    if ((print_quality = cupsGetOption("print-quality", num_options, options)) != NULL)
    {
      switch (atoi(print_quality))
      {
        case IPP_QUALITY_DRAFT :
	    printer_resolution = cupsArrayIndex(res_array, 0);
	    break;

        case IPP_QUALITY_NORMAL :
	    printer_resolution = cupsArrayIndex(res_array, cupsArrayCount(res_array) / 2);
	    break;

        case IPP_QUALITY_HIGH :
	    printer_resolution = cupsArrayIndex(res_array, cupsArrayCount(res_array) - 1);
	    break;

	default :
	    if (Verbosity)
	      fprintf(stderr, "INFO: Unsupported \"print-quality\" value '%s'.\n", print_quality);
	    break;
      }
    }
  }

  if (!printer_resolution)
    printer_resolution = cupsArrayIndex(res_array, cupsArrayCount(res_array) / 2);

  if (!printer_resolution)
  {
    fputs("ERROR: No \"printer-resolution\" or \"pwg-raster-document-resolution-supported\" value.\n", stderr);
    return (-1);
  }

 /*
  * Parse the "printer-resolution" value...
  */

  if (sscanf(printer_resolution, "%ux%udpi", &xdpi, &ydpi) != 2)
  {
    if (sscanf(printer_resolution, "%udpi", &xdpi) == 1)
    {
      ydpi = xdpi;
    }
    else
    {
      fprintf(stderr, "ERROR: Bad resolution value '%s'.\n", printer_resolution);
      return (-1);
    }
  }

  cupsArrayDelete(res_array);

 /*
  * Now figure out the color space to use...
  */

  type_array = _cupsArrayNewStrings(types, ',');

  if (color && cupsArrayFind(type_array, "srgb_8"))
    type = "srgb_8";
  else
    type = "sgray_8";

 /*
  * Initialize the raster header...
  */

  if (pages == 1)
    sides = "one-sided";
  else if ((sides = cupsGetOption("sides", num_options, options)) == NULL)
  {
    if ((sides = getenv("PRINTER_SIDES_DEFAULT")) == NULL)
      sides = "one-sided";
  }

  if (ras->copies > 1 && (pages & 1) && strcmp(sides, "one-sided"))
    pages ++;

  if (!cupsRasterInitPWGHeader(&(ras->header), pwg_media, type, xdpi, ydpi, sides, NULL))
  {
    fprintf(stderr, "ERROR: Unable to initialize raster context: %s\n", cupsRasterErrorString());
    return (-1);
  }

  if (!cupsRasterInitPWGHeader(&(ras->back_header), pwg_media, type, xdpi, ydpi, sides, sheet_back))
  {
    fprintf(stderr, "ERROR: Unable to initialize back side raster context: %s\n", cupsRasterErrorString());
    return (-1);
  }

  ras->header.cupsInteger[CUPS_RASTER_PWG_TotalPageCount]      = ras->copies * pages;
  ras->back_header.cupsInteger[CUPS_RASTER_PWG_TotalPageCount] = ras->copies * pages;

  return (0);
}
