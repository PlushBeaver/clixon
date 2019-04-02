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
  use your version of this file under the terms of Apache License version 2, indicate
  your decision by deleting the provisions above and replace them with the 
  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 *
 * Access functions for clixon data. 
 * Free-typed values for runtime getting and setting.
 *            Accessed with clicon_data(h).
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
#include <syslog.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/socket.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clixon_err.h"
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_log.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_xml_sort.h"
#include "clixon_options.h"
#include "clixon_plugin.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_data.h"

/*! Get YANG specification for application
 * Must use hash functions directly since they are not strings.
 */
yang_spec *
clicon_dbspec_yang(clicon_handle h)
{
    clicon_hash_t  *cdat = clicon_data(h);
    size_t          len;
    void           *p;

    if ((p = hash_value(cdat, "dbspec_yang", &len)) != NULL)
	return *(yang_spec **)p;
    return NULL;
}

/*! Set yang specification for application
 * ys must be a malloced pointer
 */
int
clicon_dbspec_yang_set(clicon_handle     h, 
		       struct yang_spec *ys)
{
    clicon_hash_t  *cdat = clicon_data(h);

    /* It is the pointer to ys that should be copied by hash,
       so we send a ptr to the ptr to indicate what to copy.
     */
    if (hash_add(cdat, "dbspec_yang", &ys, sizeof(ys)) == NULL)
	return -1;
    return 0;
}

/*! Get NACM (rfc 8341) XML parse tree if external not in std xml config
 * @param[in]  h    Clicon handle
 * @retval     xn   XML NACM tree, or NULL
 * @note only used if config option CLICON_NACM_MODE is external
 * @see clicon_nacm_ext_set
 */
cxobj *
clicon_nacm_ext(clicon_handle h)
{
    clicon_hash_t *cdat = clicon_data(h);
    size_t         len;
    void          *p;

    if ((p = hash_value(cdat, "nacm_xml", &len)) != NULL)
	return *(cxobj **)p;
    return NULL;
}

/*! Set NACM (rfc 8341) external XML parse tree, free old if any
 * @param[in]  h   Clicon handle
 * @param[in]  xn  XML Nacm tree
 * @note only used if config option CLICON_NACM_MODE is external
 * @see clicon_nacm_ext
 */
int
clicon_nacm_ext_set(clicon_handle h,
		     cxobj        *xn)
{
    clicon_hash_t *cdat = clicon_data(h);
    cxobj         *xo;

    if ((xo = clicon_nacm_ext(h)) != NULL)
	xml_free(xo);
    /* It is the pointer to xn that should be copied by hash,
       so we send a ptr to the ptr to indicate what to copy.
     */
    if (hash_add(cdat, "nacm_xml", &xn, sizeof(xn)) == NULL)
	return -1;
    return 0;
}


#if 1 /* Temporary function until "Top-level Yang symbol cannot be called "config"" is fixed */
/*! Get YANG specification for clixon config
 * Must use hash functions directly since they are not strings.
 */
yang_spec *
clicon_config_yang(clicon_handle h)
{
    clicon_hash_t  *cdat = clicon_data(h);
    size_t          len;
    void           *p;

    if ((p = hash_value(cdat, "control_yang", &len)) != NULL)
	return *(yang_spec **)p;
    return NULL;
}

/*! Set yang specification for control
 * ys must be a malloced pointer
 */
int
clicon_config_yang_set(clicon_handle     h, 
		       struct yang_spec *ys)
{
    clicon_hash_t  *cdat = clicon_data(h);

    /* It is the pointer to ys that should be copied by hash,
       so we send a ptr to the ptr to indicate what to copy.
     */
    if (hash_add(cdat, "control_yang", &ys, sizeof(ys)) == NULL)
	return -1;
    return 0;
}
#endif

/*! Get YANG specification for Clixon system options and features
 * Must use hash functions directly since they are not strings.
 * Example: features are typically accessed directly in the config tree.
 */
cxobj *
clicon_conf_xml(clicon_handle h)
{
    clicon_hash_t *cdat = clicon_data(h);
    size_t         len;
    void          *p;

    if ((p = hash_value(cdat, "clixon_conf", &len)) != NULL)
	return *(cxobj **)p;
    return NULL;
}

/*! Set YANG specification for Clixon system options and features
 * ys must be a malloced pointer
 */
int
clicon_conf_xml_set(clicon_handle h, 
		    cxobj        *x)
{
    clicon_hash_t  *cdat = clicon_data(h);

    /* It is the pointer to x that should be copied by hash,
     * so we send a ptr to the ptr to indicate what to copy.
     */
    if (hash_add(cdat, "clixon_conf", &x, sizeof(x)) == NULL)
	return -1;
    return 0;
}

#ifdef XXX
/*! Get xmldb datastore plugin handle, as used by dlopen/dlsym/dlclose */
plghndl_t
clicon_xmldb_plugin_get(clicon_handle h)
{
    clicon_hash_t  *cdat = clicon_data(h);
    size_t          len;
    void           *p;

    if ((p = hash_value(cdat, "xmldb_plugin", &len)) != NULL)
	return *(plghndl_t*)p;
    return NULL;
}

/*! Set xmldb datastore plugin handle, as used by dlopen/dlsym/dlclose */
int
clicon_xmldb_plugin_set(clicon_handle h, 
			plghndl_t     handle)
{
    clicon_hash_t  *cdat = clicon_data(h);

    if (hash_add(cdat, "xmldb_plugin", &handle, sizeof(void*)) == NULL)
	return -1;
    return 0;
}


/*! Get XMLDB API struct pointer
 * @param[in]  h   Clicon handle
 * @retval     xa  XMLDB API struct
 * @note xa is really of type struct xmldb_api*
 */
void *
clicon_xmldb_api_get(clicon_handle h)
{
    clicon_hash_t  *cdat = clicon_data(h);
    size_t          len;
    void           *xa;

    if ((xa = hash_value(cdat, "xmldb_api", &len)) != NULL)
	return *(void**)xa;
    return NULL;
}

/*! Set or reset XMLDB API struct pointer
 * @param[in]  h   Clicon handle
 * @param[in]  xa  XMLDB API struct
 * @note xa is really of type struct xmldb_api*
 */
int
clicon_xmldb_api_set(clicon_handle     h, 
		     void             *xa)
{
    clicon_hash_t  *cdat = clicon_data(h);

    /* It is the pointer to xa_api that should be copied by hash,
       so we send a ptr to the ptr to indicate what to copy.
     */
    if (hash_add(cdat, "xmldb_api", &xa, sizeof(void*)) == NULL)
	return -1;
    return 0;
}


/*! Get XMLDB storage handle
 * @param[in]  h   Clicon handle
 * @retval     xh  XMLDB storage handle. If not connected return NULL
 */
void *
clicon_xmldb_handle_get(clicon_handle h)
{
    clicon_hash_t  *cdat = clicon_data(h);
    size_t          len;
    void           *xh;

    if ((xh = hash_value(cdat, "xmldb_handle", &len)) != NULL)
	return *(void**)xh;
    return NULL;
}

/*! Set or reset XMLDB storage handle
 * @param[in]  h   Clicon handle
 * @param[in]  xh  XMLDB storage handle. If NULL reset it
 * @note Just keep note of it, dont allocate it or so.
 */
int
clicon_xmldb_handle_set(clicon_handle h, 
			void         *xh)
{
    clicon_hash_t  *cdat = clicon_data(h);

    if (hash_add(cdat, "xmldb_handle", &xh, sizeof(void*)) == NULL)
	return -1;
    return 0;
}
#endif /* XXX */


/*! Get authorized user name
 * @param[in]  h   Clicon handle
 * @retval     xh  XMLDB storage handle. If not connected return NULL
 */
char *
clicon_username_get(clicon_handle h)
{
    clicon_hash_t  *cdat = clicon_data(h);

    return (char*)hash_value(cdat, "username", NULL);
}

/*! Set authorized user name
 * @param[in]  h   Clicon handle
 * @param[in]  xh  XMLDB storage handle. If NULL reset it
 * @note Just keep note of it, dont allocate it or so.
 */
int
clicon_username_set(clicon_handle h, 
		    void         *username)
{
    clicon_hash_t  *cdat = clicon_data(h);

    if (username == NULL)
	return hash_del(cdat, "username");
    return hash_add(cdat, "username", username, strlen(username)+1)==NULL?-1:0;
}

/*! Get backend daemon startup status
 * @param[in]  h      Clicon handle
 * @retval     status Startup status
 */
enum startup_status
clicon_startup_status_get(clicon_handle h)
{
    clicon_hash_t *cdat = clicon_data(h);
    void           *p;

    if ((p = hash_value(cdat, "startup_status", NULL)) != NULL)
        return *(enum startup_status *)p;
    return STARTUP_ERR;
}

/*! Set backend daemon startup status
 * @param[in]  h      Clicon handle
 * @param[in]  status Startup status
 * @retval     0      OK
 * @retval    -1      Error (when setting value)
 */
int
clicon_startup_status_set(clicon_handle       h,
			  enum startup_status status)
{
    clicon_hash_t  *cdat = clicon_data(h);
    if (hash_add(cdat, "startup_status", &status, sizeof(status))==NULL)
        return -1;
    return 0;
}

/*! Get socket fd (ie backend server socket / restconf fcgx socket)
 * @param[in]  h   Clicon handle
 * @retval    -1   No open socket
 * @retval     s   Socket
 */
int
clicon_socket_get(clicon_handle h)
{
    clicon_hash_t *cdat = clicon_data(h);
    void           *p;

    if ((p = hash_value(cdat, "socket", NULL)) == NULL)
	return -1;
    return *(int*)p;
}

/*! Set socket fd (ie backend server socket / restconf fcgx socket)
 * @param[in]  h   Clicon handle
 * @param[in]  s   Open socket (or -1 to close)
 * @retval    0       OK
 * @retval   -1       Error
 */
int
clicon_socket_set(clicon_handle h, 
		  int           s)
{
    clicon_hash_t  *cdat = clicon_data(h);

    if (s == -1)
	return hash_del(cdat, "socket");
    return hash_add(cdat, "socket", &s, sizeof(int))==NULL?-1:0;
}

/*! Get module state cache
 * @param[in]  h     Clicon handle
 * @param[in]  brief 0: Full module state tree, 1: Brief tree (datastore)
 * @retval     xms   Module state cache XML tree
 * xms is on the form: <modules-state>...
 */
cxobj *
clicon_modst_cache_get(clicon_handle h,
		       int           brief)
{
    clicon_hash_t *cdat = clicon_data(h);
    void           *p;

    if ((p = hash_value(cdat, brief?"modst_brief":"modst_full", NULL)) != NULL)
	return *(cxobj **)p;
    return NULL;
}

/*! Set module state cache
 * @param[in]  h     Clicon handle
 * @param[in]  brief 0: Full module state tree, 1: Brief tree (datastore)
 * @param[in]  xms   Module state cache XML tree
 * @retval    0       OK
 * @retval   -1       Error
 */
int
clicon_modst_cache_set(clicon_handle h,
		       int           brief,
			cxobj        *xms)
{
    clicon_hash_t  *cdat = clicon_data(h);
    cxobj          *x;

    if ((x = clicon_modst_cache_get(h, brief)) != NULL)
	xml_free(x);
    if (xms == NULL)
	goto ok;
    assert(strcmp(xml_name(xms),"modules-state")==0);
    if ((x = xml_dup(xms)) == NULL)
	return -1;
    if (hash_add(cdat, brief?"modst_brief":"modst_full", &x, sizeof(x))==NULL)
	return -1;
 ok:
    return 0;
}

/*! Get yang module changelog
 * @param[in]  h    Clicon handle
 * @retval     xch  Module revision changelog XML tree
 * @see draft-wang-netmod-module-revision-management-01
 */
cxobj *
clicon_xml_changelog_get(clicon_handle h)
{
    clicon_hash_t *cdat = clicon_data(h);
    void          *p;

    if ((p = hash_value(cdat, "xml-changelog", NULL)) != NULL)
	return *(cxobj **)p;
    return NULL;
}

/*! Set xml module changelog
 * @param[in] h   Clicon handle
 * @param[in] s   Module revision changelog XML tree
 * @retval    0   OK
 * @retval   -1   Error
 * @see draft-wang-netmod-module-revision-management-01
 */
int
clicon_xml_changelog_set(clicon_handle h, 
			 cxobj        *xchlog)
{
    clicon_hash_t  *cdat = clicon_data(h);

    if (hash_add(cdat, "xml-changelog", &xchlog, sizeof(xchlog))==NULL)
	return -1;
    return 0;
}

/*! Get user clicon command-line options argv, argc (after --)
 * @param[in]  h    Clicon handle
 * @param[out] argc
 * @param[out] argv
 * @retval     0    OK 
 * @retval    -1    Error
 */
int
clicon_argv_get(clicon_handle h,
		int          *argc,
		char       ***argv)
		
{
    clicon_hash_t *cdat = clicon_data(h);
    void          *p;

    if (argc){
	if ((p = hash_value(cdat, "argc", NULL)) == NULL)
	    return -1;
	*argc = *(int*)p;
    }
    if (argv){
	if ((p = hash_value(cdat, "argv", NULL)) == NULL)
	    return -1;
	*argv = (char**)p;
    }
    return 0;
}

/*! Set clicon user command-line options argv, argc (after --)
 * @param[in] h     Clicon handle
 * @param[in] prog  argv[0] - the program name
 * @param[in] argc  Length of argv
 * @param[in] argv  Array of command-line options or NULL
 * @retval    0     OK
 * @retval   -1     Error
 * @note If argv=NULL deallocate allocated argv vector if exists.
 */
int
clicon_argv_set(clicon_handle h, 
		char         *prgm,
		int           argc,
		char        **argv)
{
    int             retval = -1;
    clicon_hash_t  *cdat = clicon_data(h);
    char          **argvv = NULL;
    size_t          len;
    
    /* add space for null-termination and argv[0] program name */
    len = argc+2;
    if ((argvv = calloc(len, sizeof(char*))) == NULL){
	clicon_err(OE_UNIX, errno, "calloc");
	goto done;
    }
    memcpy(argvv+1, argv, argc*sizeof(char*));
    argvv[0] = prgm;
    /* Note the value is the argv vector (which is copied) */
    if (hash_add(cdat, "argv", argvv, len*sizeof(char*))==NULL) 
	goto done;
    argc += 1;
    if (hash_add(cdat, "argc", &argc, sizeof(argc))==NULL)
	goto done;
    retval = 0;
 done:
    if (argvv)
	free(argvv);
    return retval;
}

/*! Get xml database element including pid and xml cache
 * @param[in]  h    Clicon handle
 * @param[in]  db   Name of database
 * @retval     de   Database element
 * @retval     NULL None found
 * @note these use db_elmnt hash, not data
 */
db_elmnt *
clicon_db_elmnt_get(clicon_handle h,
		    const char   *db)
{
    clicon_hash_t *cdat = clicon_db_elmnt(h);
    void          *p;

    if ((p = hash_value(cdat, db, NULL)) != NULL)
	return (db_elmnt *)p;
    return NULL;
}

/*! Set xml database element including pid and xml cache
 * @param[in] h   Clicon handle
 * @param[in] db  Name of database
 * @param[in] de  Database element
 * @retval    0   OK
 * @retval   -1   Error
 * XXX add prefix to db to ensure uniqueness?
 * @note these use db_elmnt hash, not data
*/
int
clicon_db_elmnt_set(clicon_handle h, 
		    const char   *db,
		    db_elmnt     *de)
{
    clicon_hash_t  *cdat = clicon_db_elmnt(h);

    if (hash_add(cdat, db, de, sizeof(*de))==NULL)
	return -1;
    return 0;
}