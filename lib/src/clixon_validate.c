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
 * XML code
 *
 * "api-path" is "URI-encoded path expression" definition in RFC8040 3.5.3
 */
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <syslog.h>
#include <fcntl.h>
#include <assert.h>
#include <arpa/inet.h>
#include <sys/param.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */

#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_string.h"
#include "clixon_err.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_netconf_lib.h"
#include "clixon_options.h"
#include "clixon_xml_nsctx.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_yang_module.h"
#include "clixon_yang_type.h"
#include "clixon_xml_map.h"
#include "clixon_validate.h"

/*! Validate xml node of type leafref, ensure the value is one of that path's reference
 * @param[in]  xt    XML leaf node of type leafref
 * @param[in]  ys    Yang spec of leaf
 * @param[in]  ytype Yang type statement belonging to the XML node
 * @param[out] xret  Error XML tree. Free with xml_free after use
 * @retval     1     Validation OK
 * @retval     0     Validation failed
 * @retval    -1     Error
 * From rfc7950 Sec 9.9.2
 *  The "path" XPath expression is  evaluated in the following context,
 *  in addition to the definition in Section 6.4.1:
 *   o  If the "path" statement is defined within a typedef, the context
 *      node is the leaf or leaf-list node in the data tree that
 *      references the typedef. (ie ys)
 *   o  Otherwise, the context node is the node in the data tree for which
 *      the "path" statement is defined. (ie yc)
 */
static int
validate_leafref(cxobj     *xt,
		 yang_stmt *ys,
		 yang_stmt *ytype,
		 cxobj    **xret)
{
    int          retval = -1;
    yang_stmt   *ypath;
    yang_stmt   *yp;
    cxobj      **xvec = NULL;
    cxobj       *x;
    int          i;
    size_t       xlen = 0;
    char        *leafrefbody;
    char        *leafbody;
    cvec        *nsc = NULL;
    cbuf        *cberr = NULL;
    char        *path;
    
    if ((leafrefbody = xml_body(xt)) == NULL)
	goto ok;
    if ((ypath = yang_find(ytype, Y_PATH, NULL)) == NULL){
	if (netconf_missing_element_xml(xret, "application", yang_argument_get(ytype), "Leafref requires path statement") < 0)
	    goto done;
	goto fail;
    }
    /* See comment^: If path is defined in typedef or not */
    if ((yp = yang_parent_get(ytype)) != NULL &&
	yang_keyword_get(yp) == Y_TYPEDEF){
	if (xml_nsctx_yang(ys, &nsc) < 0)
	    goto done;
    }
    else
	if (xml_nsctx_yang(ytype, &nsc) < 0)
	    goto done;
    path = yang_argument_get(ypath);
    if (xpath_vec(xt, nsc, "%s", &xvec, &xlen, path) < 0) 
	goto done;
    for (i = 0; i < xlen; i++) {
	x = xvec[i];
	if ((leafbody = xml_body(x)) == NULL)
	    continue;
	if (strcmp(leafbody, leafrefbody) == 0)
	    break;
    }
    if (i==xlen){
	if ((cberr = cbuf_new()) == NULL){
	    clicon_err(OE_UNIX, errno, "cbuf_new");
	    goto done;
	}
	cprintf(cberr, "Leafref validation failed: No leaf %s matching path %s", leafrefbody, path);
	if (netconf_bad_element_xml(xret, "application", leafrefbody, cbuf_get(cberr)) < 0)
	    goto done;
	goto fail;
    }
 ok:
    retval = 1;
 done:
    if (cberr)
	cbuf_free(cberr);
    if (nsc)
	xml_nsctx_free(nsc);
    if (xvec)
	free(xvec);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Validate xml node of type identityref, ensure value is a defined identity
 * Check if a given node has value derived from base identity. This is
 * a run-time check necessary when validating eg netconf.
 * Valid values for an identityref are any identities derived from all
 * the identityref's base identities.
 * Example:
 * b0 --> b1 --> b2  (b1 & b2 are derived)
 * identityref b2
 *   base b0;
 * This function does: derived_from(b2, b0);
 * @param[in]  xt    XML leaf node of type identityref
 * @param[in]  ys    Yang spec of leaf
 * @param[in]  ytype Yang type field of type identityref
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @retval     1     Validation OK
 * @retval     0     Validation failed
 * @retval    -1     Error
 * @see ys_populate_identity where the derived types are set
 * @see yang_augment_node
 * @see RFC7950 Sec 9.10.2:
 */
static int
validate_identityref(cxobj     *xt,
		     yang_stmt *ys,
		     yang_stmt *ytype,
		     cxobj    **xret)

{
    int         retval = -1;
    char       *node = NULL;
    char       *idref = NULL;
    yang_stmt  *ybaseref; /* This is the type's base reference */
    yang_stmt  *ybaseid;
    char       *prefix = NULL;
    char       *id = NULL;
    cbuf       *cberr = NULL;
    cbuf       *cb = NULL;
    cvec       *idrefvec; /* Derived identityref list: (module:id)**/
    
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new"); 
	goto done;
    }
    if ((cberr = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new"); 
	goto done;
    }
    /* Get idref value. Then see if this value is derived from ytype.
     */
    if ((node = xml_body(xt)) == NULL){ /* It may not be empty */
	if (netconf_bad_element_xml(xret, "application", xml_name(xt), "Identityref should not be empty") < 0)
	    goto done;
	goto fail;
    }
    if (nodeid_split(node, &prefix, &id) < 0)
	goto done;
    /* This is the type's base reference */
    if ((ybaseref = yang_find(ytype, Y_BASE, NULL)) == NULL){
	if (netconf_missing_element_xml(xret, "application", yang_argument_get(ytype), "Identityref validation failed, no base") < 0)
	    goto done;
	goto fail;
    }
    /* This is the actual base identity */
    if ((ybaseid = yang_find_identity(ybaseref, yang_argument_get(ybaseref))) == NULL){
	if (netconf_missing_element_xml(xret, "application", yang_argument_get(ybaseref), "Identityref validation failed, no base identity") < 0)
	    goto done;
	goto fail;
    }

    /* Assume proper namespace, otherwise we assume module prefixes,
     * see IDENTITYREF_KLUDGE 
     */
    if (0){
	char       *namespace;
	yang_stmt  *ymod;
	yang_stmt  *yspec;    

	/* Create an idref as <bbmodule>:<id> which is the format of the derived
	 * identityref list associated with the base identities.
	 */
	/* Get namespace (of idref) from xml */
	if (xml2ns(xt, prefix, &namespace) < 0)
	    goto done;
	yspec = ys_spec(ys);
	/* Get module of that namespace */
	if ((ymod = yang_find_module_by_namespace(yspec,  namespace)) == NULL){
	    clicon_err(OE_YANG, ENOENT, "No module found"); 
	    goto done;
	}
	cprintf(cb, "%s:%s", yang_argument_get(ymod), id);
    }
#if 1
    {
	yang_stmt  *ymod;
	/* idref from prefix:id to module:id */
	if (prefix == NULL)
	    ymod = ys_module(ys);
	else{ /* from prefix to name */
#if 1 /* IDENTITYREF_KLUDGE  */
	    ymod = yang_find_module_by_prefix_yspec(ys_spec(ys), prefix);
#endif
	}
	if (ymod == NULL){
	    cprintf(cberr, "Identityref validation failed, %s not derived from %s", 
		    node, yang_argument_get(ybaseid));
	    if (netconf_operation_failed_xml(xret, "application", cbuf_get(cberr)) < 0)
		goto done;
	    goto fail;
	}
	cprintf(cb, "%s:%s", yang_argument_get(ymod), id);
    }
#endif
    idref = cbuf_get(cb);	
    /* Here check if node is in the derived node list of the base identity 
     * The derived node list is a cvec computed XXX
     */
    idrefvec = yang_cvec_get(ybaseid);
    if (cvec_find(idrefvec, idref) == NULL){
	cprintf(cberr, "Identityref validation failed, %s not derived from %s", 
		node, yang_argument_get(ybaseid));
	if (netconf_operation_failed_xml(xret, "application", cbuf_get(cberr)) < 0)
	    goto done;
	goto fail;
    }
    retval = 1;
 done:
    if (cberr)
	cbuf_free(cberr);
    if (cb)
	cbuf_free(cb);
    if (id)
	free(id);
    if (prefix)
	free(prefix);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Validate an RPC node
 * @param[in]  h     Clicon handle
 * @param[in]  xrpc  XML node to be validated
 * @param[out] xret  Error XML tree. Free with xml_free after use
 * @retval     1     Validation OK
 * @retval     0     Validation failed
 * @retval    -1     Error
 * rfc7950
 * 7.14.2
 * If a leaf in the input tree has a "mandatory" statement with the
 * value "true", the leaf MUST be present in an RPC invocation.
 *
 * If a leaf in the input tree has a default value, the server MUST use
 * this value in the same cases as those described in Section 7.6.1.  In
 * these cases, the server MUST operationally behave as if the leaf was
 * present in the RPC invocation with the default value as its value.
 *
 * If a leaf-list in the input tree has one or more default values, the
 * server MUST use these values in the same cases as those described in
 * Section 7.7.2.  In these cases, the server MUST operationally behave
 * as if the leaf-list was present in the RPC invocation with the
 * default values as its values.
 *
 * Since the input tree is not part of any datastore, all "config"
 * statements for nodes in the input tree are ignored.
 *
 * If any node has a "when" statement that would evaluate to "false",
 * then this node MUST NOT be present in the input tree.
 *
 * 7.14.4
 * Input parameters are encoded as child XML elements to the rpc node's
 * XML element, in the same order as they are defined within the "input"
 * statement.
 *
 * If the RPC operation invocation succeeded and no output parameters
 * are returned, the <rpc-reply> contains a single <ok/> element defined
 * in [RFC6241].  If output parameters are returned, they are encoded as
 * child elements to the <rpc-reply> element defined in [RFC6241], in
 * the same order as they are defined within the "output" statement.
 * @see xml_yang_validate_all
 * @note Should need a variant accepting cxobj **xret
 */
int
xml_yang_validate_rpc(clicon_handle h,
		      cxobj        *xrpc,
		      cxobj       **xret)
{
    int        retval = -1;
    yang_stmt *yn=NULL;  /* rpc name */
    cxobj     *xn;       /* rpc name */
    
    if (strcmp(xml_name(xrpc), "rpc")){
	clicon_err(OE_XML, EINVAL, "Expected RPC");
	goto done;
    }
    xn = NULL;
    /* xn is name of rpc, ie <rcp><xn/></rpc> */
    while ((xn = xml_child_each(xrpc, xn, CX_ELMNT)) != NULL) {
	if ((yn = xml_spec(xn)) == NULL){
	    if (netconf_unknown_element_xml(xret, "application", xml_name(xn), NULL) < 0)
		goto done;
	    goto fail;
	}
	if ((retval = xml_yang_validate_all(h, xn, xret)) < 1) 
	    goto done; /* error or validation fail */
	if ((retval = xml_yang_validate_add(h, xn, xret)) < 1)
	    goto done; /* error or validation fail */
	if (xml_apply0(xn, CX_ELMNT, xml_default, h) < 0)
	    goto done;
    }
    // ok: /* pass validation */
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Check if an xml node is a part of a choice and have >1 siblings 
 * @param[in]  xt    XML node to be validated
 * @param[in]  yt    xt:s yang statement
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @retval     1     Validation OK
 * @retval     0     Validation failed (cbret set)
 * @retval    -1     Error
 * Check if xt is part of valid choice
 */
static int
check_choice(cxobj     *xt, 
	     yang_stmt *yt,
	     cxobj    **xret)
{
    int        retval = -1;
    yang_stmt *y;
    yang_stmt *ytp;      /* yt:s parent */
    yang_stmt *ytcase = NULL;   /* yt:s parent case if any */
    yang_stmt *ytchoice = NULL; 
    yang_stmt *yp;
    cxobj     *x;
    cxobj     *xp;
    
    if ((ytp = yang_parent_get(yt)) == NULL)
	goto ok;
    /* Return OK if xt is not choice */
    switch (yang_keyword_get(ytp)){
    case Y_CASE:
	ytcase = ytp;
	ytchoice = yang_parent_get(ytp);
	break;
    case Y_CHOICE:
	ytchoice = ytp;
	break;
    default:
	goto ok;  /* Not choice */
	break;
    }
    if ((xp = xml_parent(xt)) == NULL)
	goto ok;
    x = NULL; /* Find a child with same yang spec */
    while ((x = xml_child_each(xp, x, CX_ELMNT)) != NULL) {
	if (x == xt)
	    continue;
	y = xml_spec(x);
	if (y == yt) /* eg same list */
	    continue;
	yp = yang_parent_get(y);
	switch (yang_keyword_get(yp)){
	case Y_CASE:
	    if (yang_parent_get(yp) != ytchoice) /* Not same choice (not releveant)  */
		continue;
	    if (yp == ytcase) /* same choice but different case */
		continue;
	    break;
	case Y_CHOICE:
	    if (yp != ytcase) /* Not same choice (not relevant) */
		continue;
	    break;
	default:
	    continue; /* not choice */
	    break;
	}
	if (netconf_bad_element_xml(xret, "application", xml_name(x), "Element in choice statement already exists") < 0)
	    goto done;
	goto fail;
    } /* while */

 ok:
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Check if an xml node lacks mandatory children
 * @param[in]  xt    XML node to be validated
 * @param[in]  yt    xt:s yang statement
 * @param[out] xret  Error XML tree. Free with xml_free after use
 * @retval     1     Validation OK
 * @retval     0     Validation failed (cbret set)
 * @retval    -1     Error
 */
static int
check_mandatory(cxobj     *xt, 
		yang_stmt *yt,
		cxobj    **xret)

{
    int        retval = -1;
    cxobj     *x;
    yang_stmt *y;
    yang_stmt *yc;
    yang_stmt *yp;
    cvec      *cvk = NULL; /* vector of index keys */
    cg_var    *cvi;
    char      *keyname;
    
    yc = NULL;
    while ((yc = yn_each(yt, yc)) != NULL) {
	/* Check if a list does not have mandatory key leafs */
	if (yang_keyword_get(yt) == Y_LIST &&
	    yang_keyword_get(yc) == Y_KEY &&
	    yang_config(yt)){
	    cvk = yang_cvec_get(yt); /* Use Y_LIST cache, see ys_populate_list() */
	    cvi = NULL;
	    while ((cvi = cvec_each(cvk, cvi)) != NULL) {
		keyname = cv_string_get(cvi);	    
		if (xml_find_type(xt, NULL, keyname, CX_ELMNT) == NULL){
		    if (netconf_missing_element_xml(xret, "application", keyname, "Mandatory key") < 0)
			goto done;
		    goto fail;
		}
	    }
	}
	if (!yang_mandatory(yc))
	    continue;
	switch (yang_keyword_get(yc)){
	case Y_CONTAINER:
	case Y_ANYDATA:
	case Y_ANYXML:
	case Y_LEAF: 
	    if (yang_config(yc)==0) 
		 break;
	    /* Find a child with the mandatory yang */
	    x = NULL;
	    while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL) {
		if ((y = xml_spec(x)) != NULL
		    && y==yc)
		    break; /* got it */
	    }
	    if (x == NULL){
		if (netconf_missing_element_xml(xret, "application", yang_argument_get(yc), "Mandatory variable") < 0)
		    goto done;
		goto fail;
	    }
	    break;
	case Y_CHOICE: /* More complex because of choice/case structure */
	    x = NULL;
	    while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL) {
		if ((y = xml_spec(x)) != NULL &&
		    (yp = yang_choice(y)) != NULL &&
		    yp == yc){
		    break; /* leave loop with x set */
		}
	    }
	    if (x == NULL){
		/* @see RFC7950: 15.6 Error Message for Data That Violates 
		 * a Mandatory "choice" Statement */
		if (netconf_data_missing_xml(xret, yang_argument_get(yc), NULL) < 0)
		    goto done;
		goto fail;
	    }
	    break;
	default:
	    break;
	} /* switch */
    }
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*!
 * @param[out] xret    Error XML tree. Free with xml_free after use
 */
static int
check_list_key(cxobj     *xt, 
	       yang_stmt *yt,
	       cxobj    **xret)

{
    int        retval = -1;
    yang_stmt *yc;
    cvec      *cvk = NULL; /* vector of index keys */
    cg_var    *cvi;
    char      *keyname;
    
    yc = NULL;
    while ((yc = yn_each(yt, yc)) != NULL) {
	/* Check if a list does not have mandatory key leafs */
	if (yang_keyword_get(yt) == Y_LIST &&
	    yang_keyword_get(yc) == Y_KEY &&
	    yang_config(yt)){
	    cvk = yang_cvec_get(yt); /* Use Y_LIST cache, see ys_populate_list() */
	    cvi = NULL;
	    while ((cvi = cvec_each(cvk, cvi)) != NULL) {
		keyname = cv_string_get(cvi);	    
		if (xml_find_type(xt, NULL, keyname, CX_ELMNT) == NULL){
		    if (netconf_missing_element_xml(xret, "application", keyname, "Mandatory key") < 0)
			goto done;
		    goto fail;
		}
	    }
	}
    }
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! New element last in list, check if already exists if sp return -1
 * @param[in]  vec   Vector of existing entries (new is last)
 * @param[in]  i1    The new entry is placed at vec[i1]
 * @param[in]  vlen  Lenght of entry
 * @retval     0     OK, entry is unique
 * @retval    -1     Duplicate detected
 * @note This is currently linear complexity. It could be improved by inserting new element sorted and binary search.
 */
static int
check_insert_duplicate(char **vec,
		       int    i1,
		       int    vlen)
{
    int i;
    int v;
    char *b;
    
    for (i=0; i<i1; i++){
	for (v=0; v<vlen; v++){
	    b = vec[i*vlen+v];
	    if (b == NULL || strcmp(b, vec[i1*vlen+v]))
		break;
	}
	if (v==vlen) /* duplicate */
	    break;
    }
    return i==i1?0:-1;
}

/*! Given a list with unique constraint, detect duplicates
 * @param[in]  x     The first element in the list (on return the last)
 * @param[in]  xt    The parent of x
 * @param[in]  y     Its yang spec (Y_LIST)
 * @param[in]  yu    A yang unique spec (Y_UNIQUE)
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @retval     1     Validation OK
 * @retval     0     Validation failed (cbret set)
 * @retval    -1     Error
 * @note It would be possible to cache the vector built below
 */
static int
check_unique_list(cxobj     *x, 
		  cxobj     *xt, 
		  yang_stmt *y,
		  yang_stmt *yu,
		  cxobj    **xret)

{
    int       retval = -1;
    cvec      *cvk; /* unique vector */
    cg_var    *cvi; /* unique node name */
    cxobj     *xi;
    char     **vec = NULL; /* 2xmatrix */
    int        vlen;
    int        i;
    int        v;
    char      *bi;
    
    cvk = yang_cvec_get(yu);
    vlen = cvec_len(cvk); /* nr of unique elements to check */
    if ((vec = calloc(vlen*xml_child_nr(xt), sizeof(char*))) == NULL){
	clicon_err(OE_UNIX, errno, "calloc");
	goto done;
    }
    i = 0; /* x element index */
    do {
	cvi = NULL;
	v = 0; /* index in each tuple */
	while ((cvi = cvec_each(cvk, cvi)) != NULL){
	    /* RFC7950: Sec 7.8.3.1: entries that do not have value for all
	     * referenced leafs are not taken into account */
	    if ((xi = xml_find(x, cv_string_get(cvi))) ==NULL)
		break;
	    if ((bi = xml_body(xi)) == NULL)
		break;
	    vec[i*vlen + v++] = bi;
	}
	if (cvi==NULL){
	    /* Last element (i) is newly inserted, see if it is already there */
	    if (check_insert_duplicate(vec, i, vlen) < 0){
		if (netconf_data_not_unique_xml(xret, x, cvk) < 0)
		    goto done;
		goto fail;
	    }
	}
	x = xml_child_each(xt, x, CX_ELMNT);
	i++;
    } while (x && y == xml_spec(x));  /* stop if list ends, others may follow */
    /* It would be possible to cache vec here as an optimization */
    retval = 1;
 done:
    if (vec)
	free(vec);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Given a list, check if any min/max-elemants constraints apply
 * @param[in]  x  One x (the last) of a specific lis
 * @param[in]  y  Yang spec of x
 * @param[in]  nr Number of elements (like x) in thlist
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @retval     1     Validation OK
 * @retval     0     Validation failed (cbret set)
 * @retval    -1     Error
 * @see RFC7950 7.7.5
 */
static int
check_min_max(cxobj     *x,
	      yang_stmt *y,
	      int        nr,
	      cxobj     **xret)
{
    int         retval = -1;
    yang_stmt  *ymin; /* yang min */
    yang_stmt  *ymax; /* yang max */
    cg_var     *cv;
    
    if ((ymin = yang_find(y, Y_MIN_ELEMENTS, NULL)) != NULL){
	cv = yang_cv_get(ymin);
	if (nr < cv_uint32_get(cv)){
	    if (netconf_minmax_elements_xml(xret, x, 0) < 0)
		goto done;
	    goto fail;
	}
    }
    if ((ymax = yang_find(y, Y_MAX_ELEMENTS, NULL)) != NULL){
	cv = yang_cv_get(ymax);
	if (cv_uint32_get(cv) > 0 && /* 0 means unbounded */
	    nr > cv_uint32_get(cv)){
	    if (netconf_minmax_elements_xml(xret, x, 1) < 0)
		goto done;
	    goto fail;
	}
    }
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Detect unique constraint for duplicates from parent node and minmax
 * @param[in]  xt    XML parent (may have lists w unique constraints as child)
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @retval     1     Validation OK
 * @retval     0     Validation failed (xret set)
 * @retval    -1     Error
 * Assume xt:s children are sorted and yang populated.
 * The function does two different things of the children of an XML node:
 * (1) Check min/max element constraints
 * (2) Check unique constraints
 *
 * The routine uses a node traversing mechanism as the following example, where
 * two lists [x1,..] and [x2,..] are embedded:
 *   xt:  {a, b, [x1, x1, x1], d, e, f, [x2, x2, x2], g}
 * The function does this using a single iteration and uses the fact that the
 * xml symbols share yang symbols: ie [x1..] has yang y1 and d has yd.
 *
 * Unique constraints:
 * Lists are identified, then check_unique_list is called on each list.
 * Example, x has an associated yang list node with list of unique constraints
 *         y-list->y-unique - "a"
 *      xt->x ->  ab
 *          x ->  bc
 *          x ->  ab
 *
 * Min-max constraints: 
 * Find upper and lower bound of existing lists and report violations
 * Somewhat tricky to find violation of min-elements of empty
 * lists, but this is done by a "gap-detection" mechanism, which detects
 * gaps in the xml nodes given the ancestor Yang structure. 
 * But no gap analysis is done if the yang spec of the top-level xml is unknown
 * Example: 
 *   Yang structure:y1, y2, y3,
 *   XML structure: [x1, x1], [x3, x3] where [x2] list is missing
 * @note min-element constraints on empty lists are not detected on top-level.
 * Or more specifically, if no yang spec if associated with the top-level
 * XML node. This may not be a large problem since it would mean empty configs
 * are not allowed.
 */
static int
check_list_unique_minmax(cxobj  *xt,
			 cxobj **xret)
{
    int         retval = -1;
    cxobj      *x = NULL;
    yang_stmt  *y;
    yang_stmt  *yt;
    yang_stmt  *yp = NULL; /* previous in list */
    yang_stmt  *ye = NULL; /* yang each list to catch emtpy */
    yang_stmt  *ych; /* y:s parent node (if choice that ye can compare to) */
    cxobj      *xp = NULL; /* previous in list */
    yang_stmt  *yu;   /* yang unique */
    int         ret;
    int         nr=0;   /* Nr of list elements for min/max check */
    enum rfc_6020 keyw;
	    
    /* RFC 7950 7.7.5: regarding min-max elements check
     * The behavior of the constraint depends on the type of the 
     * leaf-list's or list's closest ancestor node in the schema tree 
     * that is not a non-presence container (see Section 7.5.1):
     * o If no such ancestor exists in the schema tree, the constraint
     * is enforced.
     * o Otherwise, if this ancestor is a case node, the constraint is
     * enforced if any other node from the case exists.
     * o  Otherwise, it is enforced if the ancestor node exists.
     */
    yt = xml_spec(xt); /* If yt == NULL, then no gap-analysis is done */
    /* Traverse all elemenents */
    while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL) {
	if ((y = xml_spec(x)) == NULL)
	    continue;
	if ((ych=yang_choice(y)) == NULL)
	    ych = y;
	keyw = yang_keyword_get(y);
	if (keyw != Y_LIST && keyw != Y_LEAF_LIST)
	    continue;
	if (yp != NULL){ /* There exists a previous (leaf)list */
	    if (y == yp){ /* If same yang as previous x, then skip (eg same list) */
		nr++;
		continue;
	    }
	    else {
		/* Check if the list length violates min/max */
		if ((ret = check_min_max(xp, yp, nr, xret)) < 0)
		    goto done;
		if (ret == 0)
		    goto fail;
	    }
	}
	yp = y; /* Restart min/max count */
	xp = x; /* Need a reference to the XML as well */
	nr = 1;
	/* Gap analysis: Check if there is any empty list between y and yp 
	 * Note, does not detect empty choice list (too complicated)
	 */
	if (yt != NULL && ych != ye){
	    /* Skip analysis if Yang spec is unknown OR
	     * if we are still iterating the same Y_CASE w multiple lists
	     */
	    ye = yn_each(yt, ye);
	    if (ye && ych != ye)
		do {
		    if (yang_keyword_get(ye) == Y_LIST || yang_keyword_get(ye) == Y_LEAF_LIST){
			/* Check if the list length violates min/max */
			if ((ret = check_min_max(xt, ye, 0, xret)) < 0)
			    goto done;
			if (ret == 0)
			    goto fail;
		    }
		    ye = yn_each(yt, ye);
		} while(ye != NULL && /* to avoid livelock (shouldnt happen) */
			ye != ych); 
	}
	if (keyw != Y_LIST)
	    continue;
	/* Here only lists. test unique constraints */
	yu = NULL;
	while ((yu = yn_each(y, yu)) != NULL) {
	    if (yang_keyword_get(yu) != Y_UNIQUE)
		continue;
	    /* Here is a list w unique constraints identified by:
	     * its first element x, its yang spec y, its parent xt, and 
	     * a unique yang spec yu,
	     */
	    if ((ret = check_unique_list(x, xt, y, yu, xret)) < 0)
		goto done;
	    if (ret == 0)
		goto fail;
	}
    }
    /* yp if set, is a list that has been traversed 
     * This check is made in the loop as well - this is for the last list
     */
    if (yp){  
	/* Check if the list length violates min/max */
	if ((ret = check_min_max(xp, yp, nr, xret)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
    }
    /* Check if there is any empty list between after last non-empty list 
     * Note, does not detect empty lists within choice/case (too complicated)
     */
    if ((ye = yn_each(yt, ye)) != NULL)
	do {
	    if (yang_keyword_get(ye) == Y_LIST || yang_keyword_get(ye) == Y_LEAF_LIST){
		/* Check if the list length violates min/max */
		if ((ret = check_min_max(xt, ye, 0, xret)) < 0)
		    goto done;
		if (ret == 0)
		    goto fail;
	    }
	} while((ye = yn_each(yt, ye)) != NULL);
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Validate a single XML node with yang specification for added entry
 * 1. Check if mandatory leafs present as subs.
 * 2. Check leaf values, eg int ranges and string regexps.
 * @param[in]  xt    XML node to be validated
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @retval     1     Validation OK
 * @retval     0     Validation failed (cbret set)
 * @retval    -1     Error
 * @code
 *   cxobj *x;
 *   cbuf *xret = NULL;
 *   if ((ret = xml_yang_validate_add(h, x, &xret)) < 0)
 *      err;
 *   if (ret == 0)
 *      fail;
 * @endcode
 * @see xml_yang_validate_all
 * @see xml_yang_validate_rpc
 * @note Should need a variant accepting cxobj **xret
 */
int
xml_yang_validate_add(clicon_handle h,
		      cxobj        *xt, 
		      cxobj       **xret)
{
    int        retval = -1;
    cg_var    *cv = NULL;
    char      *reason = NULL;
    yang_stmt *yt;   /* yang spec of xt going in */
    char      *body;
    int        ret;
    cxobj     *x;
    enum cv_type cvtype;
    
    /* if not given by argument (overide) use default link 
       and !Node has a config sub-statement and it is false */
    if ((yt = xml_spec(xt)) != NULL && yang_config(yt) != 0){
	if ((ret = check_choice(xt, yt, xret)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
	if ((ret = check_mandatory(xt, yt, xret)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
	/* Check leaf values */
	switch (yang_keyword_get(yt)){
	case Y_LEAF:
	    /* fall thru */
	case Y_LEAF_LIST:
	    /* validate value against ranges, etc */
	    if ((cv = cv_dup(yang_cv_get(yt))) == NULL){
		clicon_err(OE_UNIX, errno, "cv_dup");
		goto done;
	    }
	    /* In the union case, value is parsed as generic REST type,
	     * needs to be reparsed when concrete type is selected
	     */
	    if ((body = xml_body(xt)) == NULL){
		/* We do not allow ints to be empty. Otherwise NULL strings
		 * are considered as "" */
		cvtype = cv_type_get(cv);
		if (cv_isint(cvtype) || cvtype == CGV_BOOL || cvtype == CGV_DEC64){
		    if (netconf_bad_element_xml(xret, "application",  yang_argument_get(yt), "Invalid NULL value") < 0)
			goto done;
		    goto fail;
		}
	    }
	    else{
		if (cv_parse1(body, cv, &reason) != 1){
		    if (netconf_bad_element_xml(xret, "application",  yang_argument_get(yt), reason) < 0)
			goto done;
		    goto fail;
		}
	    }
	    if ((ys_cv_validate(h, cv, yt, &reason)) != 1){
		if (netconf_bad_element_xml(xret, "application",  yang_argument_get(yt), reason) < 0)
		    goto done;
		goto fail;
	    }
	    break;
	default:
	    break;
	}
    }
    x = NULL;
    while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL) {
	if ((ret = xml_yang_validate_add(h, x, xret)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
    }
    retval = 1;
 done:
    if (cv)
	cv_free(cv);
    if (reason)
	free(reason);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Some checks done only at edit_config, eg keys in lists
 * @param[out] xret    Error XML tree. Free with xml_free after use
 */
int
xml_yang_validate_list_key_only(clicon_handle h,
				cxobj        *xt, 
				cxobj       **xret)
{
    int        retval = -1;
    yang_stmt *yt;   /* yang spec of xt going in */
    int        ret;
    cxobj     *x;
    
    /* if not given by argument (overide) use default link 
       and !Node has a config sub-statement and it is false */
    if ((yt = xml_spec(xt)) != NULL && yang_config(yt) != 0){
	if ((ret = check_list_key(xt, yt, xret)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
    }
    x = NULL;
    while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL) {
	if ((ret = xml_yang_validate_list_key_only(h, x, xret)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
    }
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Validate a single XML node with yang specification for all (not only added) entries
 * 1. Check leafrefs. Eg you delete a leaf and a leafref references it.
 * @param[in]  xt  XML node to be validated
 * @param[out] xret  Error XML tree (if retval=0). Free with xml_free after use
 * @retval     1     Validation OK
 * @retval     0     Validation failed (cbret set)
 * @retval    -1     Error
 * @code
 *   cxobj *x;
 *   cbuf *xret = NULL;
 *   if ((ret = xml_yang_validate_all(h, x, &xret)) < 0)
 *      err;
 *   if (ret == 0)
 *      fail;
 *   xml_free(xret);
 * @endcode
 * @see xml_yang_validate_add
 * @see xml_yang_validate_rpc
 * @note Should need a variant accepting cxobj **xret
 */
int
xml_yang_validate_all(clicon_handle h,
		      cxobj        *xt, 
		      cxobj       **xret)
{
    int        retval = -1;
    yang_stmt *ys;  /* yang node */
    yang_stmt *yc;  /* yang child */
    yang_stmt *ye;  /* yang must error-message */
    char      *xpath;
    int        nr;
    int        ret;
    cxobj     *x;
    cxobj     *xp;
    char      *namespace = NULL;
    cbuf      *cb = NULL;
    cvec      *nsc = NULL;

    /* if not given by argument (overide) use default link 
       and !Node has a config sub-statement and it is false */
    ys=xml_spec(xt);
    if (ys==NULL){
	if ((cb = cbuf_new()) == NULL){
	    clicon_err(OE_UNIX, errno, "cbuf_new");
	    goto done;
	}
	if (xml2ns(xt, xml_prefix(xt), &namespace) < 0)
	    goto done;
	cprintf(cb, "Failed to find YANG spec of XML node: %s", xml_name(xt));
	if ((xp = xml_parent(xt)) != NULL)
	    cprintf(cb, " with parent: %s", xml_name(xp));
	if (namespace)
	    cprintf(cb, " in namespace: %s", namespace);
	if (netconf_unknown_element_xml(xret, "application", xml_name(xt), cbuf_get(cb)) < 0)
	    goto done;
	goto fail;
    }
    if (yang_config(ys) != 0){
	/* Node-specific validation */
	switch (yang_keyword_get(ys)){
	case Y_ANYXML:
	case Y_ANYDATA:
	    goto ok;
	    break;
	case Y_LEAF:
	    /* fall thru */
	case Y_LEAF_LIST:
	    /* Special case if leaf is leafref, then first check against
	       current xml tree
 	    */
	    /* Get base type yc */
	    if (yang_type_get(ys, NULL, &yc, NULL, NULL, NULL, NULL, NULL) < 0)
		goto done;
	    if (strcmp(yang_argument_get(yc), "leafref") == 0){
		if ((ret = validate_leafref(xt, ys, yc, xret)) < 0)
		    goto done;
		if (ret == 0)
		    goto fail;
		}
	    else if (strcmp(yang_argument_get(yc), "identityref") == 0){
		if ((ret = validate_identityref(xt, ys, yc, xret)) < 0)
		    goto done;
		if (ret == 0)
		    goto fail;
	    }
	    break;
	default:
	    break;
	}
	/* must sub-node RFC 7950 Sec 7.5.3. Can be several. 
	* XXX. use yang path instead? */
	yc = NULL;
	while ((yc = yn_each(ys, yc)) != NULL) {
	    if (yang_keyword_get(yc) != Y_MUST)
		continue;
	    xpath = yang_argument_get(yc); /* "must" has xpath argument */
	    if (xml_nsctx_yang(yc, &nsc) < 0)
		goto done;
	    if ((nr = xpath_vec_bool(xt, nsc, "%s", xpath)) < 0)
		goto done;
	    if (!nr){
		ye = yang_find(yc, Y_ERROR_MESSAGE, NULL);
		if (netconf_operation_failed_xml(xret, "application", 
						 ye?yang_argument_get(ye):"must xpath validation failed") < 0)
		    goto done;
		goto fail;
	    }
	    if (nsc){
		xml_nsctx_free(nsc);
		nsc = NULL;
	    }
	}
	/* "when" sub-node RFC 7950 Sec 7.21.5. Can only be one. */
	if ((yc = yang_find(ys, Y_WHEN, NULL)) != NULL){
	    xpath = yang_argument_get(yc); /* "when" has xpath argument */
	    if ((nr = xpath_vec_bool(xt, NULL, "%s", xpath)) < 0)
		goto done;
	    if (!nr){
		if (netconf_operation_failed_xml(xret, "application", 
					     "when xpath validation failed") < 0)
		    goto done;
		goto fail;
	    }
	}
    }
    x = NULL;
    while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL) {
	if ((ret = xml_yang_validate_all(h, x, xret)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
    }
    /* Check unique and min-max after choice test for example*/
    if (yang_config(ys) != 0){
	/* Checks if next level contains any unique list constraints */
	if ((ret = check_list_unique_minmax(xt, xret)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
    }
 ok:
    retval = 1;
 done:
    if (cb)
	cbuf_free(cb);
    if (nsc)
	xml_nsctx_free(nsc);
    return retval;
 fail:
    retval = 0;
    goto done;
}
/*! Translate a single xml node to a cligen variable vector. Note not recursive 
 * @param[out] xret    Error XML tree (if ret == 0). Free with xml_free after use
 * @retval     1     Validation OK
 * @retval     0     Validation failed (xret set)
 * @retval    -1     Error
 */
int
xml_yang_validate_all_top(clicon_handle h,
			  cxobj        *xt, 
			  cxobj       **xret)
{
    int    ret;
    cxobj *x;

    x = NULL;
    while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL) {
	if ((ret = xml_yang_validate_all(h, x, xret)) < 1)
	    return ret;
    }
    if ((ret = check_list_unique_minmax(xt, xret)) < 1)
	return ret;
    return 1;
}
