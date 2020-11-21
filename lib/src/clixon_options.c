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
  use your version of this file under the terms of Apache License version 2, indicate
  your decision by deleting the provisions above and replace them with the 
  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 *
 * This file contains access functions for two types of clixon vars:
 * - options, ie string based variables from Clixon configuration files.
 *            Accessed with clicon_options(h).
 * @see clixon_data.[ch] for free-type runtime get/set
 */
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>
#include <libgen.h> /* dirname */
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/socket.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_err.h"
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_file.h"
#include "clixon_log.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_options.h"
#include "clixon_data.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_yang_parse_lib.h"
#include "clixon_netconf_lib.h"
#include "clixon_xml_nsctx.h"
#include "clixon_xml_io.h"
#include "clixon_validate.h"
#include "clixon_xml_map.h"

/* Mapping between Cli generation from Yang string <--> constants, 
   see clixon-config.yang type cli_genmodel_type */
static const map_str2int cli_genmodel_map[] = {
    {"NONE",                 GT_NONE},
    {"VARS",                 GT_VARS},
    {"ALL",                  GT_ALL},
    {"HIDE",                 GT_HIDE},
    {NULL,                   -1}
};

/* Mapping between Clicon startup modes string <--> constants, 
   see clixon-config.yang type startup_mode */
static const map_str2int startup_mode_map[] = {
    {"none",     SM_NONE}, 
    {"running",  SM_RUNNING}, 
    {"startup",  SM_STARTUP}, 
    {"init",     SM_INIT}, 
    {NULL,       -1}
};

/* Mapping between Clicon privileges modes string <--> constants, 
 * see clixon-config.yang type priv_mode */
static const map_str2int priv_mode_map[] = {
    {"none",      PM_NONE}, 
    {"drop_perm", PM_DROP_PERM}, 
    {"drop_temp", PM_DROP_TEMP}, 
    {NULL,        -1}
};


/* Mapping between Clicon nacm user credential string <--> constants, 
 * see clixon-config.yang type nacm_cred_mode */
static const map_str2int nacm_credentials_map[] = {
    {"none",      NC_NONE}, 
    {"exact",     NC_EXACT}, 
    {"except",    NC_EXCEPT}, 
    {NULL,        -1}
};

/* Mapping between datastore cache string <--> constants, 
 * see clixon-config.yang type datastore_cache */
static const map_str2int datastore_cache_map[] = {
    {"nocache",               DATASTORE_NOCACHE},
    {"cache",                 DATASTORE_CACHE},
    {"cache-zerocopy",        DATASTORE_CACHE_ZEROCOPY},
    {NULL,                    -1}
};

/* Mapping between regular expression type string <--> constants, 
 * see clixon-config.yang type regexp_mode */
static const map_str2int yang_regexp_map[] = {
    {"posix",               REGEXP_POSIX},
    {"libxml2",             REGEXP_LIBXML2},
    {NULL,                 -1}
};

/*! Print registry on file. For debugging.
 * @param[in] h        Clicon handle
 * @param[in] dbglevel Debug level
 * @retval    0        OK
 * @retval   -1        Error
 * @note CLICON_FEATURE and CLICON_YANG_DIR are treated specially since they are lists
 */
int
clicon_option_dump(clicon_handle h, 
		   int           dbglevel)
{
    int            retval = -1;
    clicon_hash_t *hash = clicon_options(h);
    int            i;
    char         **keys = NULL;
    void          *val;
    size_t         klen;
    size_t         vlen;
    cxobj         *x = NULL;
    
    if (clicon_hash_keys(hash, &keys, &klen) < 0)
	goto done;
    for(i = 0; i < klen; i++) {
	val = clicon_hash_value(hash, keys[i], &vlen);
	if (vlen){
	    if (((char*)val)[vlen-1]=='\0') /* assume string */
		clicon_debug(dbglevel, "%s =\t \"%s\"", keys[i], (char*)val);
	    else
		clicon_debug(dbglevel, "%s =\t 0x%p , length %zu", keys[i], val, vlen);
	}
	else
	    clicon_debug(dbglevel, "%s = NULL", keys[i]);
    }
    /* Next print CLICON_FEATURE and CLICON_YANG_DIR from config tree
     * Since they are lists they are placed in the config tree.
     */
    x = NULL;
    while ((x = xml_child_each(clicon_conf_xml(h), x, CX_ELMNT)) != NULL) {
	if (strcmp(xml_name(x), "CLICON_YANG_DIR") != 0)
	    continue;
	clicon_debug(dbglevel, "%s =\t \"%s\"", xml_name(x), xml_body(x));
    }
    x = NULL;
    while ((x = xml_child_each(clicon_conf_xml(h), x, CX_ELMNT)) != NULL) {
	if (strcmp(xml_name(x), "CLICON_FEATURE") != 0)
	    continue;
	clicon_debug(dbglevel, "%s =\t \"%s\"", xml_name(x), xml_body(x));
    }
   retval = 0;
 done:
    if (keys)
	free(keys);
    return retval;
}

/*! Open and parse single config file
 * @param[in]  filename
 * @param[in]  yspec
 * @param[out] xconfig   Pointer to xml config tree. Should be freed by caller
 */
static int
parse_configfile_one(const char *filename,
		     yang_stmt  *yspec,
		     cxobj     **xconfig)
{
    int    retval = -1;
    FILE  *fp = NULL;
    cxobj *xt = NULL;
    cxobj *xerr = NULL;
    cxobj *xa;
    cbuf  *cbret = NULL;
    int    ret;

    if ((fp = fopen(filename, "r")) < 0){
	clicon_err(OE_UNIX, errno, "open configure file: %s", filename);
	return -1;
    }
    clicon_debug(2, "%s: Reading config file %s", __FUNCTION__, filename);
    if ((ret = clixon_xml_parse_file(fp, yspec?YB_MODULE:YB_NONE, yspec, NULL, &xt, &xerr)) < 0)
	goto done;
    if (ret == 0){
	if ((cbret = cbuf_new()) ==NULL){
	    clicon_err(OE_XML, errno, "cbuf_new");
	    goto done;
	}
	if (netconf_err2cb(xerr, cbret) < 0)
	    goto done;
	/* Here one could make it more relaxing to not quit on unrecognized option? */
	clixon_netconf_error(xerr, NULL, NULL);
	goto done;
    }
    /* Ensure a single root */
    if (xt == NULL || xml_child_nr(xt) != 1){
	clicon_err(OE_CFG, 0, "Config file %s: Lacks single top element", filename);
	goto done;
    }
    if (xml_rootchild(xt, 0, &xt) < 0)
	goto done;
    /* Check well-formedness */
    if (strcmp(xml_name(xt), "clixon-config") != 0 ||
	(xa = xml_find_type(xt, NULL, "xmlns", CX_ATTR)) == NULL ||
	strcmp(xml_value(xa), CLIXON_CONF_NS) != 0){
	clicon_err(OE_CFG, 0, "Config file %s: Lacks top-level \"clixon-config\" element\nClixon config files should begin with: <clixon-config xmlns=\"%s\">", filename, CLIXON_CONF_NS);
	goto done;
    }
    *xconfig = xt;
    xt = NULL;
    retval = 0;
 done:
    if (xt)
	xml_free(xt);
    if (fp)
	fclose(fp);
    if (cbret)
	cbuf_free(cbret);
    if (xerr)
	xml_free(xerr);
    return retval;
}

/*! Read filename and set values to global options registry. XML variant.
 *
 * @param[in]  h            Clixon handle
 * @param[in]  filename     Main configuration file
 * @param[in]  extraconfig0 Override (if set use that, othewrwise get from main file)
 * @param[in]  yspec        Yang spec
 * @param[out] xconfig      Pointer to xml config tree. Should be freed by caller
 * @retval     0            OK
 * @retval    -1            Error
 */
static int
parse_configfile(clicon_handle  h,
		 const char    *filename,
		 char          *extraconfdir0, 
		 yang_stmt     *yspec,
		 cxobj        **xconfig)
{
    int            retval = -1;
    struct stat    st;
    cxobj         *xt = NULL;
    cxobj         *xc = NULL;
    cxobj         *x = NULL;
    char          *name;
    char          *body;
    clicon_hash_t *copt = clicon_options(h);
    cbuf          *cbret = NULL;
    cxobj         *xerr = NULL;
    int            ret;
    cvec          *nsc = NULL;
    int            i;
    int            ndp;
    struct dirent *dp = NULL;
    char           filename1[MAXPATHLEN];
    char          *extraconfdir = NULL;
    cxobj         *xe = NULL;
    cxobj         *xec;
    DIR           *dirp;

    if (filename == NULL || !strlen(filename)){
	clicon_err(OE_UNIX, 0, "Not specified");
	goto done;
    }
    if (stat(filename, &st) < 0){
	clicon_err(OE_UNIX, errno, "%s", filename);
	goto done;
    }
    if (!S_ISREG(st.st_mode)){
	clicon_err(OE_UNIX, 0, "%s is not a regular file", filename);
	goto done;
    }
    /* Parse main config file */
    if (parse_configfile_one(filename, yspec, &xt) < 0)
	goto done;
    /* xt is a single-rooted:  <clixon-config>...</clixon-config>
     * If no override (eg from command-line)
     * Bootstrap: Shortcut to read extra confdir inline */
    if ((extraconfdir = extraconfdir0) == NULL)
	if ((xc = xpath_first(xt, 0, "CLICON_CONFIGDIR")) != NULL)
	    extraconfdir = xml_body(xc);
    if (extraconfdir){ /* If extra dir, parse extra config files */
	/* A check it exists (also done in clicon_file_dirent) */
	if ((dirp = opendir(extraconfdir)) == NULL) {
	    clicon_err(OE_UNIX, errno, "CLICON_CONFIGDIR: %s opendir", extraconfdir);
	    goto done;
	}
	closedir(dirp);
	if((ndp = clicon_file_dirent(extraconfdir, &dp, NULL, S_IFREG)) < 0)  /* Read dir */
	    goto done;
	/* Loop through files */
	for (i = 0; i < ndp; i++){	
	    snprintf(filename1, sizeof(filename1), "%s/%s", extraconfdir, dp[i].d_name);
	    if (parse_configfile_one(filename1, yspec, &xe) < 0)
		goto done;
	    /* Drain objects from extrafile and replace/append to main */
	    while ((xec = xml_child_i_type(xe, 0, CX_ELMNT)) != NULL) {
		name = xml_name(xec);
		body = xml_body(xec);
		/* Ignored from file due to bootstrapping */
		if (strcmp(name,"CLICON_CONFIGFILE")==0)
		    continue;
		/* List options for configure options that are leaf-lists: append to main */
		if (strcmp(name,"CLICON_FEATURE")==0 ||
		    strcmp(name,"CLICON_YANG_DIR")==0){
		    if (xml_addsub(xt, xec) < 0)
			goto done;
		    continue;
		}
		/* Remove existing in master if any */
		if ((x = xml_find_type(xt, NULL, name, CX_ELMNT)) != NULL)
		    xml_purge(x);
		/* Append to master (removed from xe) */
		if (xml_addsub(xt, xec) < 0)
		    goto done;
	    }
	    if (xe)
		xml_free(xe);
	    xe = NULL;
	}
    }
    if (xml_default_recurse(xt, 0) < 0)
	goto done;	
    if ((ret = xml_yang_validate_add(h, xt, &xerr)) < 0)
	goto done;
    if (ret == 0){
	if ((cbret = cbuf_new()) ==NULL){
	    clicon_err(OE_XML, errno, "cbuf_new");
	    goto done;
	}
	if (netconf_err2cb(xerr, cbret) < 0)
	    goto done;
	clicon_err(OE_CFG, 0, "Config file validation: %s", cbuf_get(cbret));
	goto done;
    }
    x = NULL;
    while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL) {
	name = xml_name(x);
	body = xml_body(x);
	/* Ignored non-leafs */
	if (name == NULL || body == NULL)
	    continue;
	/* Ignored from file due to bootstrapping */
	if (strcmp(name,"CLICON_CONFIGFILE")==0)
	    continue;
	/* List options for configure options that are leaf-lists (not leaf)
	 * They must be accessed directly by looping over clicon_conf_xml(h)
	 */
	if (strcmp(name,"CLICON_FEATURE")==0)
	    continue;
	if (strcmp(name,"CLICON_YANG_DIR")==0)
	    continue;

	if (clicon_hash_add(copt, 
			    name,
			    body,
			    strlen(body)+1) == NULL)
	    goto done;
    }
    retval = 0;
    *xconfig = xt;
    xt = NULL;
 done:
    if (dp)
	free(dp);
    if (nsc)
	xml_nsctx_free(nsc);
    if (cbret)
	cbuf_free(cbret);
    if (xerr)
	xml_free(xerr);
    if (xt)
	xml_free(xt);
    return retval;
}

/*! Add configuration option overriding file setting
 * Add to clicon_options hash, and to clicon_conf_xml tree
 * Assumes clicon_conf_xml_set has been called
 * @param[in]  h      Clicon handle
 * @param[in]  name   Name of configuration option (see clixon-config.yang)
 * @param[in]  value  String value
 * @retval     0      OK
 * @retval    -1      Error
 * @see clicon_options_main  For loading options from file
 */
int
clicon_option_add(clicon_handle h,
		  const char   *name,
		  char         *value)
{
    int            retval = -1;
    clicon_hash_t *copt = clicon_options(h);
    cxobj         *x;

    if (strcmp(name, "CLICON_FEATURE")==0 ||
	strcmp(name, "CLICON_YANG_DIR")==0){
	if ((x = clicon_conf_xml(h)) == NULL){
	    clicon_err(OE_UNIX, ENOENT, "option %s not found (clicon_conf_xml_set has not been called?)", name);
	    goto done;
	}
	if (clixon_xml_parse_va(YB_NONE, NULL, &x, NULL, "<%s>%s</%s>",
				name, value, name) < 0)
	    goto done;
    }
    if (clicon_hash_add(copt, 
		 name,
		 value,
		 strlen(value)+1) == NULL)
	goto done;
    retval = 0;
 done:
    return retval;
}

/*! Parse clixon yang file. Parse XML config file. Initialize option values
 *
 * Set default options, Read config-file, Check that all values are set.
 * Parse clixon yang file and save in yspec.
 * Read clixon system config files
 * @param[in]  h     clicon handle
 * @param[in]  yspec Yang spec of clixon config file
 * @note Due to Bug: Top-level Yang symbol cannot be called "config" in any 
 *       imported yang file, the config module needs to be isolated from all 
 *       other yang modules.
 */
int
clicon_options_main(clicon_handle h)
{
    int            retval = -1;
    char          *configfile;
    clicon_hash_t *copt = clicon_options(h);
    char          *suffix;
    char           xml = 0; /* Configfile is xml, otherwise legacy */
    cxobj         *xconfig = NULL;
    yang_stmt     *yspec = NULL;
    char          *extraconfdir = NULL;

    /* Create configure yang-spec */
    if ((yspec = yspec_new()) == NULL)
	goto done;
    /*
     * Set configure file if not set by command-line above
     */
    if (!clicon_hash_lookup(copt, "CLICON_CONFIGFILE")){ 
	clicon_option_str_set(h, "CLICON_CONFIGFILE", CLIXON_DEFAULT_CONFIG);
    }
    configfile = clicon_hash_value(copt, "CLICON_CONFIGFILE", NULL);
    clicon_debug(1, "CLICON_CONFIGFILE=%s", configfile);
    /* File must end with .xml */
    if ((suffix = rindex(configfile, '.')) != NULL){
	suffix++;
	xml = strcmp(suffix, "xml") == 0;
    }
    if (xml == 0){
	clicon_err(OE_CFG, 0, "%s: suffix %s not recognized", configfile, suffix);
	goto done;
    }
    
    /* Override extraconfdir */
    if (clicon_option_str(h, "CLICON_CONFIGDIR") &&
	(extraconfdir = strdup(clicon_option_str(h, "CLICON_CONFIGDIR"))) == NULL){
	clicon_err(OE_UNIX, errno, "strdup");
	goto done;
    }

    /* Read configfile first without yangspec, and without extra config dir for bootstrapping, 
     * see second time below with proper yangspec and extra config dir
     * (You need to read the config-file to get the YANG_DIR to find the clixon yang-spec)
     * Difference from parsing with yangspec is:
     * - no default values
     * - no sanity checks
     * - no extra config dir
     */
    if (parse_configfile(h, configfile, extraconfdir, NULL, &xconfig) < 0)
	goto done;
    
    clicon_conf_xml_set(h, xconfig);

    /* Parse clixon yang spec */
    if (yang_spec_parse_module(h, "clixon-config", NULL, yspec) < 0)
	goto done;    
    clicon_conf_xml_set(h, NULL);
    if (xconfig){
	xml_free(xconfig);
	xconfig = NULL;
    }

    /* Read configfile second time now with check yang spec */
    if (parse_configfile(h, configfile, extraconfdir, yspec, &xconfig) < 0)
	goto done;
    if (xml_spec(xconfig) == NULL){
	clicon_err(OE_CFG, 0, "Config file %s: did not find corresponding Yang specification\nHint: File does not begin with: <clixon-config xmlns=\"%s\"> or clixon-config.yang not found?", configfile, CLIXON_CONF_NS);
	goto done;
    }
    /* Set yang config spec (must store to free at exit, since conf_xml below uses it) */
    if (clicon_config_yang_set(h, yspec) < 0)
       goto done;
    yspec = NULL;
    /* Set clixon_conf pointer to handle */
    if (clicon_conf_xml_set(h, xconfig) < 0)
	goto done;

    retval = 0;
 done:
    if (yspec)
	ys_free(yspec);
    if (extraconfdir)
	free(extraconfdir);
    return retval;
}

/*! Check if a clicon option has a value
 * @param[in] h     clicon_handle
 * @param[in] name  option name
 * @retval  !=0     option exists
 * @retval    0     option does not exist
 */
int
clicon_option_exists(clicon_handle h,
		     const char   *name)
{
    clicon_hash_t *copt = clicon_options(h);

    return (clicon_hash_lookup(copt, (char*)name) != NULL);
}

/*! Get a single string option string via handle
 *
 * @param[in] h       clicon_handle
 * @param[in] name    option name
 * @retval    NULL    If option not found, or value of option is NULL
 * @retval    string  value of option if found
 * clicon options should be strings.
 * @note To differentiate the two reasons why NULL may be returned, use function 
 * clicon_option_exists() before the call
 */
char *
clicon_option_str(clicon_handle h, 
		  const char   *name)
{
    clicon_hash_t *copt = clicon_options(h);

    if (clicon_hash_lookup(copt, (char*)name) == NULL)
	return NULL;
    return clicon_hash_value(copt, (char*)name, NULL);
}

/*! Set a single string option via handle 
 * @param[in] h       clicon_handle
 * @param[in] name    option name
 * @param[in] val     option value, must be null-terminated string
 * @retval    0       OK
 * @retval   -1       Error
 */
int
clicon_option_str_set(clicon_handle h, 
		      const char   *name, 
		      char         *val)
{
    clicon_hash_t *copt = clicon_options(h);

    return clicon_hash_add(copt, (char*)name, val, strlen(val)+1)==NULL?-1:0;
}

/*! Get options as integer but stored as string
 *
 * @param[in] h    clicon handle
 * @param[in] name name of option
 * @retval    int  An integer as a result of atoi
 * @retval    -1   If option does not exist
 * @code
 *  if (clicon_option_exists(h, "X"))
 *	return clicon_option_int(h, "X");
 *  else
 *      return 0;
 * @endcode
 * Note that -1 can be both error and value.
 * This means that it should be used together with clicon_option_exists() and
 * supply a default value as shown in the example.
 */
int
clicon_option_int(clicon_handle h,
		  const char   *name)
{
    char *s;

    if ((s = clicon_option_str(h, name)) == NULL)
	return -1;
    return atoi(s);
}

/*! Set option given as int.
 * @param[in] h     Clicon handle
 * @param[in] name  Name of option to set
 * @param[in] val   Integer value
 */
int
clicon_option_int_set(clicon_handle h,
		      const char   *name,
		      int           val)
{
    char s[64];
    
    if (snprintf(s, sizeof(s)-1, "%u", val) < 0)
	return -1;
    return clicon_option_str_set(h, name, s);
}

/*! Get options as bool but stored as string
 *
 * @param[in] h    Clicon handle
 * @param[in] name name of option
 * @retval    0    false, or does not exist, or does not have a boolean value
 * @retval    1    true
 * @code
 *  if (clicon_option_exists(h, "X")
 *	return clicon_option_bool(h, "X");
 *  else
 *      return 0; # default false? 
 * @endcode
 * Note that 0 can be both error and false.
 * This means that it should be used together with clicon_option_exists() and
 * supply a default value as shown in the example.
 */
int
clicon_option_bool(clicon_handle h,
		   const char   *name)
{
    char *s;

    if ((s = clicon_option_str(h, name)) == NULL)
	return 0;
    if (strcmp(s,"true")==0)
	return 1;
    if (strcmp(s,"1")==0)
	return 1;
    return 0; /* Hopefully false, but anything else than "true" or "one" */
}

/*! Set option given as bool
 * @param[in] h     Clicon handle
 * @param[in] name  Name of option to set
 * @param[in] val   Boolean value, 0 or 1
 */
int
clicon_option_bool_set(clicon_handle h,
		      const char   *name,
		      int           val)
{
    char s[64];
    
    if (val != 0 && val != 1){
	clicon_err(OE_CFG, EINVAL, "val is %d, 0 or 1 expected", val);
	return -1;
    }
    if (snprintf(s, sizeof(s)-1, "%s", val?"true":"false") < 0){
	clicon_err(OE_CFG, errno, "snprintf");
	return -1;
    }
    return clicon_option_str_set(h, name, s);
}

/*! Delete option 
 * @param[in] h     Clicon handle
 * @param[in] name  Name of option to delete
 */
int
clicon_option_del(clicon_handle h,
		  const char   *name)
{
    clicon_hash_t *copt = clicon_options(h);

    return clicon_hash_del(copt, (char*)name);
}

/*-----------------------------------------------------------------
 * Specific option access functions for YANG configuration variables.
 * Sometimes overridden by command-line options, 
 * such as -f for CLICON_CONFIGFILE
 * @see yang/clixon-config@<date>.yang
 * You can always use the basic access functions, such as
 * clicon_option_str[_set]
 * But sometimes there are type conversions, etc which makes it more
 * convenient to make wrapper functions. Or not?
 *-----------------------------------------------------------------*/
/*! Whether to generate CLIgen syntax from datamodel or not (0, 1 or 2)
 * Must be used with a previous clicon_option_exists().
 * @param[in] h     Clicon handle
 * @retval    flag  If set, generate CLI code from yang model, otherwise not
 * @see clixon-config@<date>.yang CLICON_CLI_GENMODEL
 */
int
clicon_cli_genmodel(clicon_handle h)
{
    char const *opt = "CLICON_CLI_GENMODEL";

    if (clicon_option_exists(h, opt))
	return clicon_option_int(h, opt);
    else
	return 0;
}

/*! Generate code for CLI completion of existing db symbols
 * @param[in] h     Clicon handle
 * @retval    flag  If set, generate auto-complete CLI specs
 * @see clixon-config@<date>.yang CLICON_CLI_GENMODEL_COMPLETION
 */
int
clicon_cli_genmodel_completion(clicon_handle h)
{
    char const *opt = "CLICON_CLI_GENMODEL_COMPLETION";

    if (clicon_option_exists(h, opt))
	return clicon_option_int(h, opt);
    else
	return 0;
}

/*! How to generate and show CLI syntax: VARS|ALL 
 * @param[in] h     Clicon handle
 * @retval    mode
 * @see clixon-config@<date>.yang CLICON_CLI_GENMODEL_TYPE
 */
enum genmodel_type
clicon_cli_genmodel_type(clicon_handle h)
{
    char *str;

    if ((str = clicon_option_str(h, "CLICON_CLI_GENMODEL_TYPE")) == NULL)
	return GT_VARS;
    else
	return clicon_str2int(cli_genmodel_map, str);
}

/*! Get "do not include keys in cvec" in cli vars callbacks
 * @param[in] h     Clicon handle
 * @retval    flag  If set, get only vars
 * @see clixon-config@<date>.yang CLICON_CLI_VARONLY
 */
int
clicon_cli_varonly(clicon_handle h)
{
    char const *opt = "CLICON_CLI_VARONLY";

    if (clicon_option_exists(h, opt))
	return clicon_option_int(h, opt);
    else
	return 0;
}

/*! Get family of backend socket: AF_UNIX, AF_INET or AF_INET6 
 * @see clixon-config@<date>.yang CLICON_SOCK_FAMILY
 * @param[in] h     Clicon handle
 * @retval    fam   Socket family
 */
int
clicon_sock_family(clicon_handle h)
{
    char *s;

    if ((s = clicon_option_str(h, "CLICON_SOCK_FAMILY")) == NULL)
	return AF_UNIX;
    else  if (strcmp(s, "IPv4")==0)
	return AF_INET;
    else  if (strcmp(s, "IPv6")==0)
	return AF_INET6;
    else
	return AF_UNIX; /* default */
}

/*! Get port for backend socket in case of AF_INET or AF_INET6 
 * @param[in] h     Clicon handle
 * @retval    port  Socket port
 * @see clixon-config@<date>.yang CLICON_SOCK_PORT
 */
int
clicon_sock_port(clicon_handle h)
{
    char *s;

    if ((s = clicon_option_str(h, "CLICON_SOCK_PORT")) == NULL)
	return -1;
    return atoi(s);
}

/*! Set if all configuration changes are committed automatically 
 * @param[in] h     Clicon handle
 * @retval    flag  Autocommit (or not)
 */
int
clicon_autocommit(clicon_handle h)
{
    char const *opt = "CLICON_AUTOCOMMIT";

    if (clicon_option_exists(h, opt))
	return clicon_option_int(h, opt);
    else
	return 0;
}

/*! Which method to boot/start clicon backend
 * @param[in] h     Clicon handle
 * @retval    mode  Startup mode
 */
int
clicon_startup_mode(clicon_handle h)
{
    char *mode;

    if ((mode = clicon_option_str(h, "CLICON_STARTUP_MODE")) == NULL)
	return -1;
    return clicon_str2int(startup_mode_map, mode);
}

/*! Which privileges drop method to use
 * @param[in] h     Clicon handle
 * @retval    mode  Privileges mode
 */
enum priv_mode_t
clicon_backend_privileges_mode(clicon_handle h)
{
    char *mode;

    if ((mode = clicon_option_str(h, "CLICON_BACKEND_PRIVILEGES")) == NULL)
	return -1;
    return clicon_str2int(priv_mode_map, mode);
}

/*! Which privileges drop method to use
 * @param[in] h     Clicon handle
 * @retval    mode  Privileges mode
 */
enum nacm_credentials_t
clicon_nacm_credentials(clicon_handle h)
{
    char *mode;

    if ((mode = clicon_option_str(h, "CLICON_NACM_CREDENTIALS")) == NULL)
	return -1;
    return clicon_str2int(nacm_credentials_map, mode);
}

/*! Which datastore cache method to use
 * @param[in] h      Clicon handle
 * @retval    method Datastore cache method
 * @see clixon-config@<date>.yang CLICON_DATASTORE_CACHE
 */
enum datastore_cache
clicon_datastore_cache(clicon_handle h)
{
    char *str;

    if ((str = clicon_option_str(h, "CLICON_DATASTORE_CACHE")) == NULL)
	return DATASTORE_CACHE;
    else
	return clicon_str2int(datastore_cache_map, str);
}

/*! Which Yang regexp/pattern engine to use
 * @param[in] h     Clicon handle
 * @retval    mode  Regexp engine to use
 * @see clixon-config@<date>.yang CLICON_YANG_REGEXP
 */
enum regexp_mode
clicon_yang_regexp(clicon_handle h)
{
    char *str;

    if ((str = clicon_option_str(h, "CLICON_YANG_REGEXP")) == NULL)
	return REGEXP_POSIX;
    else
	return clicon_str2int(yang_regexp_map, str);
}

/*---------------------------------------------------------------------
 * Specific option access functions for non-yang options
 * Typically dynamic values and more complex datatypes,
 * Such as handles to plugins, API:s and parsed structures
 *--------------------------------------------------------------------*/

/*! Get quiet mode eg -q option, do not print notifications on stdout 
 * @param[in] h      Clicon handle
 * @retval    flag   quiet mode on or off
 */
int
clicon_quiet_mode(clicon_handle h)
{
    char *s;
    if ((s = clicon_option_str(h, "CLICON_QUIET")) == NULL)
	return 0; /* default */
    return atoi(s);
}

/*! Set quiet mode
 * @param[in] h      Clicon handle
 * @param[in] val    Flag value
 */
int
clicon_quiet_mode_set(clicon_handle h,
		      int           val)
{
    return clicon_option_int_set(h, "CLICON_QUIET", val);
}

