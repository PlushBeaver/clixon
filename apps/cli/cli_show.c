/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
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

 * 
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>

#include <unistd.h>
#include <dirent.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <pwd.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

/* Exported functions in this file are in clixon_cli_api.h */
#include "clixon_cli_api.h"
#include "cli_common.h" /* internal functions */

/*! Completion callback intended for automatically generated data model
 *
 * Returns an expand-type list of commands as used by cligen 'expand' 
 * functionality.
 *
 * Assume callback given in a cligen spec: a <x:int expand_dbvar("db" "<xmlkeyfmt>")
 * @param[in]   h        clicon handle 
 * @param[in]   name     Name of this function (eg "expand_dbvar")
 * @param[in]   cvv      The command so far. Eg: cvec [0]:"a 5 b"; [1]: x=5;
 * @param[in]   argv     Arguments given at the callback ("<db>" "<xmlkeyfmt>")
 * @param[out]  commands vector of function pointers to callback functions
 * @param[out]  helptxt  vector of pointers to helptexts
 * @see cli_expand_var_generate  This is where arg is generated
 */
int
expand_dbvar(void   *h, 
	     char   *name, 
	     cvec   *cvv, 
	     cvec   *argv, 
	     cvec   *commands,
	     cvec   *helptexts)
{
    int              retval = -1;
    char            *api_path_fmt;
    char            *api_path = NULL;
    char            *dbstr;    
    cxobj           *xt = NULL;
    char            *xpath = NULL;
    cxobj          **xvec = NULL;
    cxobj           *xe; /* direct ptr */
    cxobj           *xerr = NULL; /* free */
    size_t           xlen = 0;
    cxobj           *x;
    char            *bodystr;
    int              i;
    char            *bodystr0 = NULL; /* previous */
    cg_var          *cv;
    yang_stmt       *yspec;
    cxobj           *xtop = NULL; /* xpath root */
    cxobj           *xbot = NULL; /* xpath, NULL if datastore */
    yang_stmt       *y = NULL; /* yang spec of xpath */
    yang_stmt       *yp;
    yang_stmt       *ytype;
    yang_stmt       *ypath;
    cxobj           *xcur;
    char            *xpathcur;
    char            *reason = NULL;
    cvec            *nsc = NULL;
    int              ret;
    
    if (argv == NULL || cvec_len(argv) != 2){
	clicon_err(OE_PLUGIN, 0, "requires arguments: <db> <xmlkeyfmt>");
	goto done;
    }
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }
    if ((cv = cvec_i(argv, 0)) == NULL){
	clicon_err(OE_PLUGIN, 0, "Error when accessing argument <db>");
	goto done;
    }
    dbstr  = cv_string_get(cv);
    if (strcmp(dbstr, "running") != 0 &&
	strcmp(dbstr, "candidate") != 0 &&
	strcmp(dbstr, "startup") != 0){
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", dbstr);	
	goto done;
    }
    if ((cv = cvec_i(argv, 1)) == NULL){
	clicon_err(OE_PLUGIN, 0, "Error when accessing argument <api_path>");
	goto done;
    }
    api_path_fmt = cv_string_get(cv);
    /* api_path_fmt = /interface/%s/address/%s
     * api_path: -->  /interface/eth0/address/.*
     * xpath:    -->  /interface/[name="eth0"]/address
     */
    if (api_path_fmt2api_path(api_path_fmt, cvv, &api_path) < 0)
	goto done;
    if (api_path2xpath(api_path, yspec, &xpath, &nsc, NULL) < 0)
	goto done;

    /* Get configuration */
    if (clicon_rpc_get_config(h, NULL, dbstr, xpath, nsc, &xt) < 0) /* XXX */
    	goto done;
    if ((xe = xpath_first(xt, NULL, "/rpc-error")) != NULL){
	clixon_netconf_error(xe, "Get configuration", NULL);
	goto ok; 
    }
    xcur = xt; /* default top-of-tree */
    xpathcur = xpath;
    /* Create config top-of-tree */
    if ((xtop = xml_new("config", NULL, CX_ELMNT)) == NULL)
	goto done;
    xbot = xtop;
    /* This is primarily to get "y", 
     * xpath2xml would have worked!!
     * XXX: but y is just the first in this list, there could be other y:s?
     */
    if (api_path){
	if ((ret = api_path2xml(api_path, yspec, xtop, YC_DATANODE, 0, &xbot, &y, &xerr)) < 0)
	    goto done;
	if (ret == 0){
	    clixon_netconf_error(xerr, "Expand datastore symbol", NULL);
	    goto done;
	}
    }
    if (y==NULL)
	goto ok;


    /* Special case for leafref. Detect leafref via Yang-type, 
     * Get Yang path element, tentatively add the new syntax to the whole
     * tree and apply the path to that.
     * Last, the reference point for the xpath code below is changed to 
     * the point of the tentative new xml.
     * Here the whole syntax tree is loaded, and it would be better to offload
     * such operations to the datastore by a generic xpath function.
     */
    if ((ytype = yang_find(y, Y_TYPE, NULL)) != NULL)
	if (strcmp(yang_argument_get(ytype), "leafref")==0){
	    if ((ypath = yang_find(ytype, Y_PATH, NULL)) == NULL){
		clicon_err(OE_DB, 0, "Leafref %s requires path statement", yang_argument_get(ytype));
		goto done;
	    }
	    xpathcur = yang_argument_get(ypath);
	    if (xml_merge(xt, xtop, yspec, &reason) < 0) /* Merge xtop into xt */
		goto done;
	    if (reason){
		fprintf(stderr, "%s\n", reason);
		goto done;
	    }	    
	    if ((xcur = xpath_first(xt, nsc, "%s", xpath)) == NULL){
		clicon_err(OE_DB, 0, "xpath %s should return merged content", xpath);
		goto done;
	    }
	}
    if (xpath_vec(xcur, nsc, "%s", &xvec, &xlen, xpathcur) < 0) 
	goto done;
    /* Loop for inserting into commands cvec. 
     * Detect duplicates: for ordered-by system assume list is ordered, so you need
     * just remember previous
     * but for ordered-by system, check the whole list
     */
    bodystr0 = NULL;
    for (i = 0; i < xlen; i++) {
	x = xvec[i];
	if (xml_type(x) == CX_BODY)
	    bodystr = xml_value(x);
	else
	    bodystr = xml_body(x);
	if (bodystr == NULL)
	    continue; /* no body, cornercase */
	if ((y = xml_spec(x)) != NULL &&
	    (yp = yang_parent_get(y)) != NULL &&
	    yang_keyword_get(yp) == Y_LIST &&
	    yang_find(yp, Y_ORDERED_BY, "user") != NULL){
	    /* Detect duplicates linearly in existing values */
	    {
		cg_var *cv = NULL;
		while ((cv = cvec_each(commands, cv)) != NULL)
		    if (strcmp(cv_string_get(cv), bodystr) == 0)
			break;
		if (cv == NULL)
		    cvec_add_string(commands, NULL, bodystr);
	    }
	}
	else{
	    if (bodystr0 && strcmp(bodystr, bodystr0) == 0)
		continue; /* duplicate, assume sorted */
	    bodystr0 = bodystr;
	    /* RFC3986 decode */
	    cvec_add_string(commands, NULL, bodystr);
	}
    }
 ok:
    retval = 0;
  done:
    if (xerr)
	xml_free(xerr);
    if (nsc)
	xml_nsctx_free(nsc);
    if (reason)
	free(reason);
    if (api_path)
	free(api_path);
    if (xvec)
	free(xvec);
    if (xtop)
	xml_free(xtop);
    if (xt)
	xml_free(xt);
    if (xpath) 
	free(xpath);
    return retval;
}

/*! List files in a directory
 */
int
expand_dir(char   *dir, 
	   int    *nr, 
	   char ***commands, 
	   mode_t  flags, 
	   int     detail)
{
    DIR	          *dirp;
    struct dirent *dp;
    struct stat    st;
    char          *str;
    char          *cmd;
    int            len;
    int            retval = -1;
    struct passwd *pw;
    char           filename[MAXPATHLEN];

    if ((dirp = opendir(dir)) == 0){
	fprintf(stderr, "expand_dir: opendir(%s) %s\n", 
		dir, strerror(errno));
	return -1;
    }
    *nr = 0;
    while ((dp = readdir(dirp)) != NULL) {
	if (
#if 0
	    strcmp(dp->d_name, ".") != 0 &&
	    strcmp(dp->d_name, "..") != 0
#else
	    dp->d_name[0] != '.'
#endif	    
	    ) {
	    snprintf(filename, MAXPATHLEN-1, "%s/%s", dir, dp->d_name);
	    if (lstat(filename, &st) == 0){
		if ((st.st_mode & flags) == 0)
		    continue;

#if EXPAND_RECURSIVE
		if (S_ISDIR(st.st_mode)) {
		    int nrsav = *nr;
		    if(expand_dir(filename, nr, commands, detail) < 0)
			goto quit;
		    while(nrsav < *nr) {
			len = strlen(dp->d_name) +  strlen((*commands)[nrsav]) + 2;
			if((str = malloc(len)) == NULL) {
			    fprintf(stderr, "expand_dir: malloc: %s\n",
				    strerror(errno));
			    goto quit;
			}
			snprintf(str, len-1, "%s/%s",
				 dp->d_name, (*commands)[nrsav]);
			free((*commands)[nrsav]);
			(*commands)[nrsav] = str;
			
			nrsav++;
		    }
		    continue;
		}
#endif
		if ((cmd = strdup(dp->d_name)) == NULL) {
		    fprintf(stderr, "expand_dir: strdup: %s\n",
			    strerror(errno));
		    goto quit;
		}
#ifndef __APPLE__
		if (0 &&detail){
		    if ((pw = getpwuid(st.st_uid)) == NULL){
			fprintf(stderr, "expand_dir: getpwuid(%d): %s\n",
				st.st_uid, strerror(errno));
			goto quit;
		    }
		    len = strlen(cmd) + 
			strlen(pw->pw_name) +
#ifdef __FreeBSD__
			strlen(ctime(&st.st_mtimespec.tv_sec)) +
#else
			strlen(ctime(&st.st_mtim.tv_sec)) +
#endif

			strlen("{ by }") + 1 /* \0 */;
		    if ((str=realloc(cmd, strlen(cmd)+len)) == NULL) {
			fprintf(stderr, "expand_dir: malloc: %s\n",
				strerror(errno));
			goto quit;
		    }
		    snprintf(str + strlen(dp->d_name), 
			     len - strlen(dp->d_name),
			     "{%s by %s}",
#ifdef __FreeBSD__
			     ctime(&st.st_mtimespec.tv_sec),
#else
			     ctime(&st.st_mtim.tv_sec),
#endif

			     pw->pw_name
			);
		    cmd = str;
		}
#endif /* __APPLE__ */
		if (((*commands) =
		     realloc(*commands, ((*nr)+1)*sizeof(char**))) == NULL){
		    perror("expand_dir: realloc");
		    goto quit;
		}
		(*commands)[(*nr)] = cmd;
		(*nr)++;
		if (*nr >= 128) /* Limit number of options */
		    break;
	    }
	}
    }
    retval = 0;
  quit:
    closedir(dirp);
    return retval;
}

/*! CLI callback show yang spec. If arg given matches yang argument string */
int
show_yang(clicon_handle h, 
	  cvec         *cvv, 
	  cvec         *argv)
{
  yang_stmt *yn;
  char      *str = NULL;
  yang_stmt *yspec;

  yspec = clicon_dbspec_yang(h);	
  if (cvec_len(argv) > 0){
      str = cv_string_get(cvec_i(argv, 0));
      yn = yang_find(yspec, 0, str);
  }
  else
    yn = yspec;
  yang_print(stdout, yn);
  return 0;
}

/*! Show configuration and state internal function
 *
 * @param[in]  h     CLICON handle
 * @param[in]  state If set, show both config and state, otherwise only config
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  argv  String vector: <dbname> <format> <xpath> [<varname>]
 * Format of argv:
 *   <dbname>  "running"|"candidate"|"startup" # note only running state=1
 *   <format>  "text"|"xml"|"json"|"cli"|"netconf" (see format_enum)
 *   <xpath>   xpath expression, that may contain one %, eg "/sender[name='foo']"
 *   <namespace> If xpath set, the namespace the symbols in xpath belong to (optional)
 * @code
 *   show config id <n:string>, cli_show_config("running","xml","iface[name='foo']","urn:example:example");
 * @endcode
 * @note if state parameter is set, then db must be running
 */
static int
cli_show_config1(clicon_handle h, 
		 int           state,
		 cvec         *cvv, 
		 cvec         *argv)
{
    int              retval = -1;
    char            *db;
    char            *formatstr;
    char            *xpath;
    enum format_enum format;
    cbuf            *cbxpath = NULL;
    char            *val = NULL;
    cxobj           *xt = NULL;
    cxobj           *xc;
    cxobj           *xerr;
    enum genmodel_type gt;
    yang_stmt       *yspec;
    char            *namespace = NULL;
    cvec            *nsc = NULL;
    
    if (cvec_len(argv) != 3 && cvec_len(argv) != 4){
	clicon_err(OE_PLUGIN, 0, "Got %d arguments. Expected: <dbname>,<format>,<xpath>[,<attr>]", cvec_len(argv));

	goto done;
    }
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }
    /* First argv argument: Database */
    db = cv_string_get(cvec_i(argv, 0));
    /* Second argv argument: Format */
    formatstr = cv_string_get(cvec_i(argv, 1));
    if ((int)(format = format_str2int(formatstr)) < 0){
	clicon_err(OE_PLUGIN, 0, "Not valid format: %s", formatstr);
	goto done;
    }
    /* Third argv argument: xpath */
    xpath = cv_string_get(cvec_i(argv, 2));

    /* Create XPATH variable string */
    if ((cbxpath = cbuf_new()) == NULL){
	clicon_err(OE_PLUGIN, errno, "cbuf_new");	
	goto done;
    }
    cprintf(cbxpath, "%s", xpath);	
    /* Fourth argument is namespace */
    if (cvec_len(argv) == 4){
	namespace = cv_string_get(cvec_i(argv, 3));
	if ((nsc = xml_nsctx_init(NULL, namespace)) == NULL)
	    goto done;
    }
    if (state == 0){     /* Get configuration-only from database */
	if (clicon_rpc_get_config(h, NULL, db, cbuf_get(cbxpath), nsc, &xt) < 0)
	    goto done;
    }
    else {               /* Get configuration and state from database */
	if (strcmp(db, "running") != 0){
	    clicon_err(OE_FATAL, 0, "Show state only for running database, not %s", db);
	    goto done;
	}
	if (clicon_rpc_get(h, cbuf_get(cbxpath), nsc, CONTENT_ALL, -1, &xt) < 0)
	    goto done;
    }
    if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Get configuration", NULL);
	goto done;
    }
    /* Print configuration according to format */
    switch (format){
    case FORMAT_XML:
	xc = NULL; /* Dont print xt itself */
	while ((xc = xml_child_each(xt, xc, -1)) != NULL)
	    clicon_xml2file(stdout, xc, 0, 1);
	break;
    case FORMAT_JSON:
	xml2json(stdout, xt, 1);
	break;
    case FORMAT_TEXT:
	xc = NULL; /* Dont print xt itself */
	while ((xc = xml_child_each(xt, xc, -1)) != NULL)
	    xml2txt(stdout, xc, 0); /* tree-formed text */
	break;
    case FORMAT_CLI:
	/* get CLI generatade mode: VARS|ALL */
	if ((gt = clicon_cli_genmodel_type(h)) == GT_ERR)
	    goto done;
	xc = NULL; /* Dont print xt itself */
	while ((xc = xml_child_each(xt, xc, CX_ELMNT)) != NULL)
	    xml2cli(stdout, xc, NULL, gt); /* cli syntax */
	break;
    case FORMAT_NETCONF:
	fprintf(stdout, "<rpc><edit-config><target><candidate/></target><config>\n");
	xc = NULL; /* Dont print xt itself */
	while ((xc = xml_child_each(xt, xc, -1)) != NULL)
	    clicon_xml2file(stdout, xc, 2, 1);
	fprintf(stdout, "</config></edit-config></rpc>]]>]]>\n");
	break;
    }
    retval = 0;
done:
    if (nsc)
	xml_nsctx_free(nsc);
    if (xt)
	xml_free(xt);
    if (val)
	free(val);
    if (cbxpath)
	cbuf_free(cbxpath);
    return retval;
}

/*! Show configuration and state CLIGEN callback function
 *
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  argv  String vector: <dbname> <format> <xpath> [<varname>]
 * Format of argv:
 *   <dbname>  "running"|"candidate"|"startup"
 *   <format>  "text"|"xml"|"json"|"cli"|"netconf" (see format_enum)
 *   <xpath>   xpath expression, that may contain one %, eg "/sender[name="%s"]"
 *   <namespace> If xpath set, the namespace the symbols in xpath belong to (optional)
 * @code
 *   show config id <n:string>, cli_show_config("running","xml","iface[name='foo']","urn:example:example");
 * @endcode
 * @see cli_show_config_state  For config and state data (not only config)
 */
int
cli_show_config(clicon_handle h, 
		cvec         *cvv, 
		cvec         *argv)
{
    return cli_show_config1(h, 0, cvv, argv);
}

/*! Show configuration and state CLIgen callback function
 *
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  argv  String vector: <dbname> <format> <xpath> [<varname>]
 * Format of argv:
 *   <dbname>  "running"
 *   <format>  "text"|"xml"|"json"|"cli"|"netconf" (see format_enum)
 *   <xpath>   xpath expression, that may contain one %, eg "/sender[name="%s"]"
 *   <varname> optional name of variable in cvv. If set, xpath must have a '%s'
 * @code
 *   show state id <n:string>, cli_show_config_state("running","xml","iface[name='foo']","urn:example:example");
 * @endcode
 * @see cli_show_config  For config-only, no state
 */
int
cli_show_config_state(clicon_handle h, 
		      cvec         *cvv, 
		      cvec         *argv)
{
    return cli_show_config1(h, 1, cvv, argv);
}

/*! Show configuration as text given an xpath
 * Utility function used by cligen spec file
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line must contain xpath and ns variables
 * @param[in]  argv  A string: <dbname>
 * @note  Hardcoded that variable xpath and ns cvv must exist. (kludge)
 */
int
show_conf_xpath(clicon_handle h, 
		cvec         *cvv, 
		cvec         *argv)
{
    int              retval = -1;
    char            *str;
    char            *xpath;
    cg_var          *cv;
    cxobj           *xt = NULL;
    cxobj           *xerr;
    cxobj          **xv = NULL;
    size_t           xlen;
    int              i;
    char            *namespace = NULL;
    cvec            *nsc = NULL;

    if (cvec_len(argv) != 1){
	clicon_err(OE_PLUGIN, 0, "Requires one element to be <dbname>");
	goto done;
    }
    str = cv_string_get(cvec_i(argv, 0));
    /* Dont get attr here, take it from arg instead */
    if (strcmp(str, "running") != 0 && 
	strcmp(str, "candidate") != 0 && 
	strcmp(str, "startup") != 0){
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", str);	
	goto done;
    }
    /* Look for xpath in command (kludge: cv must be called "xpath") */
    cv = cvec_find(cvv, "xpath");
    xpath = cv_string_get(cv);

    /* Look for namespace in command (kludge: cv must be called "ns") */
    cv = cvec_find(cvv, "ns");
    namespace = cv_string_get(cv);
    if ((nsc = xml_nsctx_init(NULL, namespace)) == NULL)
	goto done;
    if (clicon_rpc_get_config(h, NULL, str, xpath, nsc, &xt) < 0)
    	goto done;
    if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Get configuration", NULL);
	goto done;
    }

    if (xpath_vec(xt, nsc, "%s", &xv, &xlen, xpath) < 0) 
	goto done;
    for (i=0; i<xlen; i++)
	xml_print(stdout, xv[i]);

    retval = 0;
done:
    if (nsc)
	xml_nsctx_free(nsc);
    if (xv)
	free(xv);
    if (xt)
	xml_free(xt);
    return retval;
}

int cli_show_version(clicon_handle h,
		     cvec         *vars,
		     cvec         *argv)
{
    fprintf(stdout, "%s\n", CLIXON_VERSION_STRING);
    return 0;
}

/*! Generic show configuration CLIgen callback using generated CLI syntax
 * @param[in]  h     CLICON handle
 * @param[in]  state If set, show both config and state, otherwise only config
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  argv  String vector: <dbname> <format> <xpath> [<varname>]
 * Format of argv:
 *   <api_path_fmt> Generated API PATH
 *   <dbname>  "running"|"candidate"|"startup"
 *   <format>  "text"|"xml"|"json"|"cli"|"netconf" (see format_enum)
 * @note if state parameter is set, then db must be running
 * @note that first argument is generated by code.
 */
static int 
cli_show_auto1(clicon_handle h,
	       int           state,
	       cvec         *cvv,
	       cvec         *argv)
{
    int              retval = 1;
    yang_stmt       *yspec;
    char            *api_path_fmt;  /* xml key format */
    //    char            *api_path = NULL; /* xml key */
    char            *db;
    char            *xpath = NULL;
    cvec            *nsc = NULL;
    char            *formatstr;
    enum format_enum format = FORMAT_XML;
    cxobj           *xt = NULL;
    cxobj           *xp;
    cxobj           *xerr;
    enum genmodel_type gt;
    char            *api_path = NULL;

    if (cvec_len(argv) != 3){
	clicon_err(OE_PLUGIN, 0, "Usage: <api-path-fmt>* <database> <format>. (*) generated.");
	goto done;
    }
    /* First argv argument: API_path format */
    api_path_fmt = cv_string_get(cvec_i(argv, 0));
    /* Second argv argument: Database */
    db = cv_string_get(cvec_i(argv, 1));
    /* Third format: output format */
    formatstr = cv_string_get(cvec_i(argv, 2));
    if ((int)(format = format_str2int(formatstr)) < 0){
	clicon_err(OE_PLUGIN, 0, "Not valid format: %s", formatstr);
	goto done;
    }
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }
    if (api_path_fmt2api_path(api_path_fmt, cvv, &api_path) < 0)
	goto done;
    if (api_path2xpath(api_path, yspec, &xpath, &nsc, NULL) < 0)
	goto done;
    /* XXX Kludge to overcome a trailing / in show, that I cannot add to
     * yang2api_path_fmt_1 where it should belong.
     */
    if (xpath[strlen(xpath)-1] == '/')
	xpath[strlen(xpath)-1] = '\0';

    if (state == 0){   /* Get configuration-only from database */
	if (clicon_rpc_get_config(h, NULL, db, xpath, nsc, &xt) < 0)
	    goto done;
    }
    else{              /* Get configuration and state from database */
	if (strcmp(db, "running") != 0){
	    clicon_err(OE_FATAL, 0, "Show state only for running database, not %s", db);
	    goto done;
	}
	if (clicon_rpc_get(h, xpath, nsc, CONTENT_ALL, -1, &xt) < 0)
	    goto done;
    }
    if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Get configuration", NULL);
	goto done;
    }
    if ((xp = xpath_first(xt, nsc, "%s", xpath)) != NULL)
	/* Print configuration according to format */
	switch (format){
	case FORMAT_XML:
	    clicon_xml2file(stdout, xp, 0, 1);
	    break;
	case FORMAT_JSON:
	    xml2json(stdout, xp, 1);
	    break;
	case FORMAT_TEXT:
	    xml2txt(stdout, xp, 0); /* tree-formed text */
	    break;
	case FORMAT_CLI:
	    if ((gt = clicon_cli_genmodel_type(h)) == GT_ERR)
		goto done;
	    xml2cli(stdout, xp, NULL, gt); /* cli syntax */
	    break;
	case FORMAT_NETCONF:
	    fprintf(stdout, "<rpc><edit-config><target><candidate/></target><config>\n");
	    clicon_xml2file(stdout, xp, 2, 1);
	    fprintf(stdout, "</config></edit-config></rpc>]]>]]>\n");
	    break;
	default: /* see cli_show_config() */
	    break;
	}
    retval = 0;
 done:
    if (nsc)
	xml_nsctx_free(nsc);
    if (api_path)
	free(api_path);
    if (xpath)
	free(xpath);
    if (xt)
	xml_free(xt);
    return retval;
}

/*! Generic show configuration CLIgen callback using generated CLI syntax
 * Format of argv:
 *   <api_path_fmt> Generated API PATH
 *   <dbname>  "running"|"candidate"|"startup"
 *   <format>  "text"|"xml"|"json"|"cli"|"netconf" (see format_enum)
 * @see cli_show_auto_state  For config and state
 */
int 
cli_show_auto(clicon_handle h,
	      cvec         *cvv,
	      cvec         *argv)
{
    return cli_show_auto1(h, 0, cvv, argv);
}

/*! Generic show config and state CLIgen callback using generated CLI syntax
 * Format of argv:
 *   <api_path_fmt> Generated API PATH
 *   <dbname>  "running"
 *   <format>  "text"|"xml"|"json"|"cli"|"netconf" (see format_enum)
 * @see cli_show_auto    For config only
 */
int 
cli_show_auto_state(clicon_handle h,
		    cvec         *cvv,
		    cvec         *argv)
{
    return cli_show_auto1(h, 1, cvv, argv);
}

