/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand and Benny Holmgren

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

See https://www.w3.org/TR/xpath/

*
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
#define XPATH_OPTS "hD:f:p:i:n:cy:Y:x"

static int
usage(char *argv0)
{
    fprintf(stderr, "usage:%s [options]\n"
	    "where options are\n"
            "\t-h \t\tHelp\n"
    	    "\t-D <level> \tDebug\n"
	    "\t-f <file>  \tXML file\n"
	    "\t-p <xpath> \tPrimary XPATH string\n"
	    "\t-i <xpath0>\t(optional) Initial XPATH string\n"
	    "\t-n <pfx:id>\tNamespace binding (pfx=NULL for default)\n"
	    "\t-c \t\tMap xpath to canonical form\n"
	    "\t-y <filename> \tYang filename or dir (load all files)\n"
    	    "\t-Y <dir> \tYang dirs (can be several)\n"
	    "\t-x \t\tXPath optimize\n"
	    "and the following extra rules:\n"
	    "\tif -f is not given, XML input is expected on stdin\n"
	    "\tif -p is not given, <xpath> is expected as the first line on stdin\n"
	    "This means that with no arguments, <xpath> and XML is expected on stadin.\n",
	    argv0
	    );
    exit(0);
}

static int
ctx_print2(cbuf   *cb,
	   xp_ctx *xc)
{
    int        i;

    cprintf(cb, "%s:", (char*)clicon_int2str(ctxmap, xc->xc_type));
    switch (xc->xc_type){
    case XT_NODESET:
	for (i=0; i<xc->xc_size; i++){
	    cprintf(cb, "%d:", i);
	    clicon_xml2cbuf(cb, xc->xc_nodeset[i], 0, 0, -1);
	}
	break;
    case XT_BOOL:
	cprintf(cb, "%s", xc->xc_bool?"true":"false");
	break;
    case XT_NUMBER:
	cprintf(cb, "%lf", xc->xc_number);
	break;
    case XT_STRING:
	cprintf(cb, "%s", xc->xc_string);
	break;
    }
    return 0;
}

int
main(int    argc,
     char **argv)
{
    int         retval = -1;
    char       *argv0 = argv[0];
    int         i;
    cxobj     **xv = NULL;
    cxobj      *x0 = NULL;
    cxobj      *x;
    int         c;
    int         len;
    char       *buf = NULL;
    int         ret;
    int         fd = 0; /* unless overriden by argv[1] */
    char       *yang_file_dir = NULL;
    yang_stmt  *yspec = NULL;
    char       *xpath = NULL;
    char       *xpath0 = NULL;
    char       *filename;
    xp_ctx     *xc = NULL;
    cbuf       *cb = NULL;
    clicon_handle h;
    struct stat st;
    cvec       *nsc = NULL;
    int         canonical = 0;

    clicon_log_init("xpath", LOG_DEBUG, CLICON_LOG_STDERR); 

    if ((h = clicon_handle_init()) == NULL)
	goto done;

    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, XPATH_OPTS)) != -1)
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
		clicon_err(OE_UNIX, errno, "open(%s)", argv[1]);
		goto done;
	    }
	    break;
	case 'p': /* Primary XPATH string */
	    xpath = optarg;
	    break;
	case 'i': /* Optional initial XPATH string */
	    xpath0 = optarg;
	    break;
	case 'n':{ /* Namespace binding */
	    char *prefix;
	    char *id;
	    if (nsc == NULL &&
		(nsc = xml_nsctx_init(NULL, NULL)) == NULL)
		goto done;
	    if (nodeid_split(optarg, &prefix, &id) < 0)
		goto done;
	    if (prefix && strcmp(prefix, "null")==0){
		free(prefix);
		prefix = NULL;
	    }
	    if (xml_nsctx_add(nsc, prefix, id) < 0)
		goto done;
	    if (prefix)
		free(prefix);
	    if (id)
		free(id);
	    break;
	}
	case 'c': /* Map namespace to canonical form */
	    canonical = 1;
	    break;
	case 'y':
	    yang_file_dir = optarg;
	    break;
	case 'Y':
	    if (clicon_option_add(h, "CLICON_YANG_DIR", optarg) < 0)
		goto done;
	    break;
	case 'x': /* xpath optimize. Only if XPATH_LIST_OPTIMIZE is set */ 

	    xpath_list_optimize_set(1);
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

    if (xpath==NULL){
	/* First read xpath */
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
	xpath = buf;
    }

    /* If canonical, translate nsc and xpath to canonical form */
    if (canonical){
	char *xpath1 = NULL;
	cvec *nsc1 = NULL;
	if (xpath2canonical(xpath, nsc, yspec, &xpath1, &nsc1) < 0)
	    goto done;
	xpath = xpath1;
	if (xpath)
	    fprintf(stdout, "%s\n", xpath);
	if (nsc)
	    xml_nsctx_free(nsc);
	nsc = nsc1;
	if (nsc)
	    cvec_print(stdout, nsc);
	goto ok; /* need a switch to continue, now just print and quit */
    }

    /* 
     * If fd=0, then continue reading from stdin (after CR)
     * If fd>0, reading from file opened as argv[1]
     */
    if (xml_parse_file(fd, "</clicon>", NULL, &x0) < 0){
	fprintf(stderr, "Error: parsing: %s\n", clicon_err_reason);
	return -1;
    }

    /* Validate XML as well */
    if (yang_file_dir){
	cbuf  *cbret = NULL;
	cxobj *x1;
	cxobj *xerr = NULL; /* malloced must be freed */

	x1 = xml_child_i(x0, 0);
	/* Populate */
	if (xml_apply0(x1, CX_ELMNT, xml_spec_populate, yspec) < 0)
	    goto done;
	/* Sort */
	if (xml_apply0(x1, CX_ELMNT, xml_sort, h) < 0)
	    goto done;
	/* Add default values */
	if (xml_apply(x1, CX_ELMNT, xml_default, h) < 0)
	    goto done;
	if (xml_apply0(x1, -1, xml_sort_verify, h) < 0)
	    clicon_log(LOG_NOTICE, "%s: sort verify failed", __FUNCTION__);
	if ((ret = xml_yang_validate_all_top(h, x1, &xerr)) < 0) 
	    goto done;
	if (ret > 0 && (ret = xml_yang_validate_add(h, x1, &xerr)) < 0)
	    goto done;
	if (ret == 0){
	    if ((cbret = cbuf_new()) ==NULL){
		clicon_err(OE_XML, errno, "cbuf_new");
		goto done;
	    }
	    if (netconf_err2cb(xerr, cbret) < 0)
		goto done;
	    fprintf(stderr, "xml validation error: %s\n", cbuf_get(cbret));
	    goto done;
	}
    }
    
    /* If xpath0 given, position current x (ie somewhere else than root) */
    if (xpath0){
	if ((x = xpath_first(x0, NULL, "%s", xpath0)) == NULL){
	    fprintf(stderr, "Error: xpath0 returned NULL\n");
	    return -1;
	}
    }
    else
	x = x0;
#ifdef XPATH_LIST_OPTIMIZE /* Experimental */
    {
	int hits = 0;
	int j;

	xpath_list_optimize_stats(&hits);
	for (j=0;j<1;j++){
	    if (xpath_vec_ctx(x, nsc, xpath, 0, &xc) < 0)
		return -1;
	}
	xpath_list_optimize_stats(&hits);
	fprintf(stderr, "hits after:%d\n", hits);
    }
#else
    if (xpath_vec_ctx(x, nsc, xpath, 0, &xc) < 0)
	return -1;
#endif
    /* Print results */
    cb = cbuf_new();
    ctx_print2(cb, xc);
    fprintf(stdout, "%s\n", cbuf_get(cb));
 ok:
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    if (nsc)
	xml_nsctx_free(nsc);
    if (xc)
	ctx_free(xc);
    if (xv)
	free(xv);
    if (buf)
	free(buf);
    if (x0)
	xml_free(x0);
    if (fd > 0)
	close(fd);
    return retval;
}
