/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 * "api-path" is "URI-encoded path expression" definition in RFC8040 3.5.3
 * "Instance-identifier" is a subset of XML Xpaths and defined in Yang, used in NACM for example.
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <syslog.h>
#include <fcntl.h>
#include <sys/stat.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon/clixon.h"

/* Command line options to be passed to getopt(3) */
#define UTIL_PATH_OPTS "hD:f:ap:y:Y:n:"

static int
usage(char *argv0)
{
    fprintf(stderr, "usage:%s [options]\n"
	    "where options are\n"
            "\t-h \t\tHelp\n"
    	    "\t-D <level> \tDebug\n"
	    "\t-f <file>  \tXML file\n"
	    "\t-a \t\tUse API-PATH (default INSTANCE-ID)\n"
	    "\t-p <xpath> \tPATH string\n"
	    "\t-y <filename> \tYang filename or dir (load all files)\n"
    	    "\t-Y <dir> \tYang dirs (can be several)\n"
	    "\t-n <n>   \tRepeat the call n times(for profiling)\n"
	    "and the following extra rules:\n"
	    "\tif -f is not given, XML input is expected on stdin\n"
	    "\tif -p is not given, <path> is expected as the first line on stdin\n"
	    "This means that with no arguments, <api-path> and XML is expected on stdin.\n",
	    argv0
	    );
    exit(0);
}

int
main(int    argc,
     char **argv)
{
    int         retval = -1;
    char       *argv0 = argv[0];
    int         i;
    cxobj      *x = NULL;
    cxobj      *xc;
    cxobj     **xvec = NULL;
    int         xlen = 0;
    int         c;
    int         len;
    char       *buf = NULL;
    int         ret;
    int         fd = 0; /* unless overriden by argv[1] */
    char       *yang_file_dir = NULL;
    yang_stmt  *yspec = NULL;
    char       *path = NULL;
    char       *filename;
    cbuf       *cb = NULL;
    int         api_path_p = 0; /* api-path or instance-id */
    clicon_handle h;
    struct stat   st;
    cxobj        *xcfg = NULL;
    cxobj        *xerr = NULL; /* malloced must be freed */
    int           nr = 1;
	
    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init("api-path", LOG_DEBUG, CLICON_LOG_STDERR); 
    /* Initialize clixon handle */
    if ((h = clicon_handle_init()) == NULL)
	goto done;
    /* Initialize config tree (needed for -Y below) */
    if ((xcfg = xml_new("clixon-config", NULL, CX_ELMNT)) == NULL)
	goto done;
    if (clicon_conf_xml_set(h, xcfg) < 0)
	goto done;
    
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, UTIL_PATH_OPTS)) != -1)
	switch (c) {
	case 'h':
	    usage(argv0);
	    break;
    	case 'D':
	    if (sscanf(optarg, "%d", &debug) != 1)
		usage(argv0);
	    break;
	case 'f': /* XML file */
	    filename = optarg;
	    if ((fd = open(filename, O_RDONLY)) < 0){
		clicon_err(OE_UNIX, errno, "open(%s)", optarg);
		goto done;
	    }
	    break;
	case 'a': /* API-PATH instead of INSTANCE-ID */
	    api_path_p++;
	    break;
	case 'p': /* API-PATH string */
	    path = optarg;
	    break;
	case 'y':
	    yang_file_dir = optarg;
	    break;
	case 'Y':
	    if (clicon_option_add(h, "CLICON_YANG_DIR", optarg) < 0)
		goto done;
	    break;
	case 'n':
	    nr = atoi(optarg);
	    break;
	default:
	    usage(argv[0]);
	    break;
	}
    /* Parse yang */
    if (yang_file_dir){
	if ((yspec = yspec_new()) == NULL)
	    goto done;
	if (stat(yang_file_dir, &st) < 0){
	    clicon_err(OE_YANG, errno, "%s not found", yang_file_dir);
	    goto done;
	}
	if (S_ISDIR(st.st_mode)){
	    if (yang_spec_load_dir(h, yang_file_dir, yspec) < 0)
		goto done;
	}
	else{
	    if (yang_spec_parse_file(h, yang_file_dir, yspec) < 0)
		goto done;
	}
    }

    if (path==NULL){
	/* First read api-path from file */
	len = 1024; /* any number is fine */
	if ((buf = malloc(len)) == NULL){
	    perror("pt_file malloc");
	    return -1;
	}
	memset(buf, 0, len);
	i = 0;
	while (1){ 
	    if ((ret = read(0, &c, 1)) < 0){
		perror("read");
		goto done;
	    }
	    if (ret == 0)
		break;
	    if (c == '\n')
		break;
	    if (len==i){
		if ((buf = realloc(buf, 2*len)) == NULL){
		    fprintf(stderr, "%s: realloc: %s\n", __FUNCTION__, strerror(errno));
		    return -1;
		}	    
		memset(buf+len, 0, len);
		len *= 2;
	    }
	    buf[i++] = (char)(c&0xff);
	}
	path = buf;
    }

    /* 
     * If fd=0, then continue reading from stdin (after CR)
     * If fd>0, reading from file opened as argv[1]
     */
    if (clixon_xml_parse_file(fd, YB_NONE, NULL, NULL, &x, NULL) < 0){
	fprintf(stderr, "Error: parsing: %s\n", clicon_err_reason);
	return -1;
    }

    /* Validate XML as well */
    if (yang_file_dir){
	/* Populate */
	if (xml_bind_yang(x, YB_MODULE, yspec, NULL) < 0)
	    goto done;
	/* sort */
	if (xml_apply0(x, CX_ELMNT, xml_sort, h) < 0)
	    goto done;
	if (xml_apply0(x, -1, xml_sort_verify, h) < 0)
	    clicon_log(LOG_NOTICE, "%s: sort verify failed", __FUNCTION__);
	/* Add default values */
	if (xml_apply(x, CX_ELMNT, xml_default, h) < 0)
	    goto done;
	if ((ret = xml_yang_validate_all_top(h, x, &xerr)) < 0) 
	    goto done;
	if (ret > 0 && (ret = xml_yang_validate_add(h, x, &xerr)) < 0)
	    goto done;
	if (ret == 0){
	    if ((cb = cbuf_new()) ==NULL){
		clicon_err(OE_XML, errno, "cbuf_new");
		goto done;
	    }
	    if (netconf_err2cb(xerr, cb) < 0)
		goto done;
	    fprintf(stderr, "xml validation error: %s\n", cbuf_get(cb));
	    goto done;
	}

    }
    /* Repeat for profiling (default is nr = 1) */
    xvec = NULL;
    for (i=0; i<nr; i++){
	if (api_path_p){
	    if ((ret = clixon_xml_find_api_path(x, yspec, &xvec, &xlen, "%s", path)) < 0)
		goto done;
	}
	else{
	    if ((ret = clixon_xml_find_instance_id(x, yspec, &xvec, &xlen, "%s", path)) < 0)
		goto done;
	}
	if (ret == 0){
	    fprintf(stderr, "Fail\n");
	    goto done;
	}
    }
    /* Print results */
    for (i = 0; i < xlen; i++){
	xc = xvec[i];
	fprintf(stdout, "%d: ", i);
	clicon_xml2file(stdout, xc, 0, 0);
	fprintf(stdout, "\n");
	fflush(stdout);
    }
    retval = 0;
 done:
    if (yspec != NULL)
	yspec_free(yspec);
    if (cb)
	cbuf_free(cb);
    if (xvec)
	free(xvec);
    if (buf)
	free(buf);
    if (x)
	xml_free(x);
    if (xcfg)
	xml_free(xcfg);
    if (fd > 0)
	close(fd);
    if (h)
	clicon_handle_exit(h);
    return retval;
}
