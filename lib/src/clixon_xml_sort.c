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

 * XML search functions when used with YANG
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
#include <assert.h>
#include <syslog.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_options.h"
#include "clixon_xml_map.h"
#include "clixon_yang_type.h"
#include "clixon_xml_sort.h"

/*! Get xml body value as cligen variable
 * @param[in]  x   XML node (body and leaf/leaf-list)
 * @param[out] cvp Pointer to cligen variable containing value of x body
 * @retval     0   OK, cvp contains cv or NULL
 * @retval    -1   Error
 * @note only applicable if x is body and has yang-spec and is leaf or leaf-list
 * Move to clixon_xml.c?
 */
static int
xml_cv_cache(cxobj   *x,
	     cg_var **cvp)
{
    int          retval = -1;
    cg_var      *cv = NULL;
    yang_stmt   *y;
    yang_stmt   *yrestype;
    enum cv_type cvtype;
    int          ret;
    char        *reason=NULL;
    int          options = 0;
    uint8_t      fraction = 0;
    char        *body;
		 
    if ((body = xml_body(x)) == NULL)
	body="";
    if ((cv = xml_cv(x)) != NULL)
	goto ok;
    if ((y = xml_spec(x)) == NULL)
	goto ok;
    if (yang_type_get(y, NULL, &yrestype, &options, NULL, NULL, NULL, &fraction) < 0)
	goto done;
    yang2cv_type(yang_argument_get(yrestype), &cvtype);
    if (cvtype==CGV_ERR){
	clicon_err(OE_YANG, errno, "yang->cligen type %s mapping failed",
		   yang_argument_get(yrestype));
	goto done;
    }
    if ((cv = cv_new(cvtype)) == NULL){
	clicon_err(OE_YANG, errno, "cv_new");
	goto done;
    }
    if (cvtype == CGV_DEC64)
	cv_dec64_n_set(cv, fraction);
	
    if ((ret = cv_parse1(body, cv, &reason)) < 0){
	clicon_err(OE_YANG, errno, "cv_parse1");
	goto done;
    }
    if (ret == 0){
	clicon_err(OE_YANG, EINVAL, "cv parse error: %s\n", reason);
	goto done;
    }
    if (xml_cv_set(x, cv) < 0)
	goto done;
 ok:
    *cvp = cv;
    cv = NULL;
    retval = 0;
 done:
    if (reason)
	free(reason);
    if (cv)
	cv_free(cv);
    return retval;
}

/*! Given a child name and an XML object, return yang stmt of child
 * If no xml parent, find root yang stmt matching name
 * @param[in]  x        Child
 * @param[in]  xp       XML parent, can be NULL.
 * @param[in]  yspec    Yang specification (top level)
 * @param[out] yresult  Pointer to yang stmt of result, or NULL, if not found
 * @retval     0       OK
 * @retval    -1       Error
 * @note special rule for rpc, ie <rpc><foo>,look for top "foo" node.
 * @note works for import prefix, but not work for generic XML parsing where
 *       xmlns and xmlns:ns are used.
 */
int
xml_child_spec(cxobj      *x,
	       cxobj      *xp,
	       yang_stmt  *yspec,
	       yang_stmt **yresult)
{
    int        retval = -1;
    yang_stmt *y = NULL;  /* result yang node */   
    yang_stmt *yparent; /* parent yang */
    yang_stmt *ymod = NULL;
    yang_stmt *yi;
    char      *name;
	    
    name = xml_name(x);
    if (xp && (yparent = xml_spec(xp)) != NULL){
	/* First case: parent already has an associated yang statement,
	 * then find matching child of that */
	if (yang_keyword_get(yparent) == Y_RPC){
	    if ((yi = yang_find(yparent, Y_INPUT, NULL)) != NULL)
		y = yang_find_datanode(yi, name);
	}
	else
	    y = yang_find_datanode(yparent, name);
    }
    else if (yspec){
	/* Second case, this is a "root", need to find yang stmt from spec
	 */
	if (ys_module_by_xml(yspec, xp, &ymod) < 0)
	    goto done;
	if (ymod != NULL)
	    y = yang_find_schemanode(ymod, name);
    }
    else
	y = NULL;
    /* kludge rpc -> input */
    if (y && yang_keyword_get(y) == Y_RPC && yang_find(y, Y_INPUT, NULL))
	y = yang_find(y, Y_INPUT, NULL);
    *yresult = y;
    retval = 0;
 done:
    return retval;
}

/*! Help function to qsort for sorting entries in xml child vector same parent
 * @param[in]  x1    object 1
 * @param[in]  x2    object 2
 * @param[in]  same  If set, x1 and x2 are member of same parent & enumeration 
 *                   is used (see explanation below)
 * @retval     0     If equal
 * @retval    <0     If x1 is less than x2
 * @retval    >0     If x1 is greater than x2
 * @see xml_cmp1   Similar, but for one object
 *
 * There are distinct calls for this function:
 * 1. For sorting in an existing list of XML children
 * 2. For searching of an existing element in a list
 * In the first case, there is a special case for "ordered-by-user", where
 * if they have the same yang-spec, the existing order is used as tie-breaker.
 * In other words, if order-by-system, or if the case (2) above, the existing
 * order is ignored and the actual xml element contents is examined.
 * @note empty value/NULL is smallest value
 * @note some error cases return as -1 (qsort cant handle errors)
 * @note some error cases return as -1 (qsort cant handle errors)
 * 
 * NOTE: "comparing" x1 and x2 here judges equality from a structural (model)
 * perspective, ie both have the same yang spec, if they are lists, they have the
 * the same keys. NOT that the values are equal!
 * In other words, if x is a leaf with the same yang spec, <x>1</x> and <x>2</x> are
 * "equal". 
 * If x is a list element (with key "k"), 
 * <x><k>42</42><y>foo</y></x> and <x><k>42</42><y>bar</y></x> are equal, 
 * but is not equal to <x><k>71</42><y>bar</y></x>
 */
int
xml_cmp(cxobj *x1,
	cxobj *x2,
	int    same)
{
    yang_stmt  *y1;
    yang_stmt  *y2;
    int         yi1 = 0;
    int         yi2 = 0;
    cvec       *cvk = NULL; /* vector of index keys */
    cg_var     *cvi;
    int         equal = 0;
    char       *b1;
    char       *b2;
    char       *keyname;
    cg_var     *cv1; 
    cg_var     *cv2;
    int         nr1 = 0;
    int         nr2 = 0;
    cxobj      *x1b;
    cxobj      *x2b;

    if (x1==NULL || x2==NULL)
	goto done; /* shouldnt happen */
    y1 = xml_spec(x1);
    y2 = xml_spec(x2);
    if (same){
	nr1 = xml_enumerate_get(x1);
	nr2 = xml_enumerate_get(x2);
    }
    if (y1==NULL && y2==NULL){
	if (same)
	    equal = nr1-nr2;
	goto done;
    }
    if (y1==NULL){
        equal = -1;
        goto done;
    }
    if (y2==NULL){
        equal = 1;
        goto done;
    }
    if (y1 != y2){ 
	yi1 = yang_order(y1);
	yi2 = yang_order(y2);
	if ((equal = yi1-yi2) != 0)
	    goto done;
    }
    /* Now y1==y2, same Yang spec, can only be list or leaf-list,
     * But first check exceptions, eg config false or ordered-by user
     * otherwise sort according to key
     * If the two elements are in the same list, and they are ordered-by user
     * then do not look more into equivalence, use the enumeration in the
     * existing list.
     */
    if (same &&
	(yang_config(y1)==0 || yang_find(y1, Y_ORDERED_BY, "user") != NULL)){
	    equal = nr1-nr2;
	    goto done; /* Ordered by user or state data : maintain existing order */
	}
    switch (yang_keyword_get(y1)){
    case Y_LEAF_LIST: /* Match with name and value */
	if ((b1 = xml_body(x1)) == NULL)
	    equal = -1;
	else if ((b2 = xml_body(x2)) == NULL)
	    equal = 1;
	else{
	    if (xml_cv_cache(x1, &cv1) < 0) /* error case */
		goto done;
	    if (xml_cv_cache(x2, &cv2) < 0) /* error case */
		goto done;
	    if (cv1 != NULL && cv2 != NULL)
		equal = cv_cmp(cv1, cv2);
	    else if (cv1 == NULL && cv2 == NULL)
		equal = 0;
	    else if (cv1 == NULL)
		equal = -1;
	    else
		equal = 1;
	}
	break;
    case Y_LIST: /* Match with key values 
		  * Use Y_LIST cache (see struct yang_stmt)
		  */
	cvk = yang_cvec_get(y1); /* Use Y_LIST cache, see ys_populate_list() */
	cvi = NULL;
	while ((cvi = cvec_each(cvk, cvi)) != NULL) {
	    keyname = cv_string_get(cvi); /* operational data may have NULL keys*/
	    if ((x1b = xml_find(x1, keyname)) == NULL ||
		xml_body(x1b) == NULL)
		equal = -1;
	    else if ((x2b = xml_find(x2, keyname)) == NULL ||
		     xml_body(x2b) == NULL)
		equal = 1;
	    else{
		if (xml_cv_cache(x1b, &cv1) < 0) /* error case */
		    goto done;
		if (xml_cv_cache(x2b, &cv2) < 0) /* error case */
		    goto done;
		assert(cv1 && cv2);
		if ((equal = cv_cmp(cv1, cv2)) != 0)
		    goto done;
	    }
	}
	equal = 0;
	break;
    default:
	break;
    }
 done:
    clicon_debug(2, "%s %s %s %d nr: %d %d yi: %d %d", __FUNCTION__, xml_name(x1), xml_name(x2), equal, nr1, nr2, yi1, yi2);
    return equal;
}

/*!
 * @note args are pointer ot pointers, to fit into qsort cmp function
 */
static int
xml_cmp_qsort(const void* arg1, 
	      const void* arg2)
{
    return xml_cmp(*(struct xml**)arg1, *(struct xml**)arg2, 1);
}

/*! Sort children of an XML node 
 * Assume populated by yang spec.
 * @param[in] x0   XML node
 * @param[in] arg  Dummy so it can be called by xml_apply()
 * @retval    -1    Error, aborted at first error encounter
 * @retval     0    OK, all nodes traversed (subparts may have been skipped)
 * @retval     1    OK, aborted on first fn returned 1
 * @see xml_apply  - typically called by recursive apply function
 */
int
xml_sort(cxobj *x,
	 void  *arg)
{
    yang_stmt *ys;

    /* Abort sort if non-config (=state) data */
    if ((ys = xml_spec(x)) != 0 && yang_config(ys)==0)
	return 1;
    xml_enumerate_children(x);
    qsort(xml_childvec_get(x), xml_child_nr(x), sizeof(cxobj *), xml_cmp_qsort);
    return 0;
}

/*! Special case search for ordered-by user where linear sort is used
 * @param[in]  xp    Parent XML node (go through its childre)
 * @param[in]  x1    XML node to match
 * @param[in]  yangi Yang order number (according to spec)
 * @param[in]  mid   Where to start from (may be in middle of interval)
 * @param[out] xretp XML return object, or NULL
 * @retval     0     OK, see xretp
 */
static int
xml_search_userorder(cxobj        *xp,
		     cxobj        *x1,
		     int           yangi,
		     int           mid,
		     cxobj       **xretp)
{
    int        i;
    cxobj     *xc;
    yang_stmt *yc;
    
    for (i=mid+1; i<xml_child_nr(xp); i++){ /* First increment */
	xc = xml_child_i(xp, i);
	yc = xml_spec(xc);
	if (yangi!=yang_order(yc))
	    break;
	if (xml_cmp(xc, x1, 0) == 0){
	    *xretp = xc;
	    goto done;
	}
    }
    for (i=mid-1; i>=0; i--){ /* Then decrement */
	xc = xml_child_i(xp, i);
	yc = xml_spec(xc);
	if (yangi!=yang_order(yc))
	    break;
	if (xml_cmp(xc, x1, 0) == 0){
	    *xretp = xc;
	    goto done;
	}
    }
    *xretp = NULL;
 done:
    return 0;
}

/*! Find XML child under xp matching x1 using binary search
 * @param[in]  xp        Parent xml node. 
 * @param[in]  x1        Find this object among xp:s children
 * @param[in]  userorder If x1 is ordered by user
 * @param[in]  yangi     Yang order
 * @param[in]  low       Lower bound of childvec search interval 
 * @param[in]  upper     Lower bound of childvec search interval 
 * @param[out] xretp     XML return object, or NULL
 * @retval     0         OK, see xretp
 */
static int
xml_search1(cxobj        *xp,
	    cxobj        *x1,
	    int           userorder,
	    int           yangi,
	    int           low, 
	    int           upper,
	    cxobj       **xretp)
{
    int        mid;
    int        cmp;
    cxobj     *xc;
    yang_stmt *y;

    if (upper < low)
    	goto notfound;
    mid = (low + upper) / 2;
    if (mid >= xml_child_nr(xp))  /* beyond range */
    	goto notfound;
    xc = xml_child_i(xp, mid);
    if ((y = xml_spec(xc)) == NULL)
	goto notfound;
    cmp = yangi-yang_order(y);
    /* Here is right yang order == same yang? */
    if (cmp == 0){
	cmp = xml_cmp(x1, xc, 0);
	if (cmp && userorder){ /* Ordered by user (if not equal) */
	    xml_search_userorder(xp, x1, yangi, mid, xretp);	    
	    goto done;
	}
    }
    if (cmp == 0)
	*xretp = xc;
    else if (cmp < 0)
	xml_search1(xp, x1, userorder, yangi, low, mid-1, xretp);
    else 
	xml_search1(xp, x1, userorder, yangi, mid+1, upper, xretp);
 done:
    return 0;
 notfound:
    *xretp = NULL;
    goto done;
}

/*! Find XML child under xp matching x1 using binary search
 * @param[in]  xp    Parent xml node. 
 * @param[in]  x1    Find this object among xp:s children
 * @param[in]  yc    Yang spec of x1
 * @param[out] xretp XML return object, or NULL
 * @retval     0     OK, see xretp
 */
static int
xml_search(cxobj        *xp,
	   cxobj        *x1,
	   yang_stmt    *yc,
	   cxobj       **xretp)
{
    cxobj     *xa;
    int        low = 0;
    int        upper = xml_child_nr(xp);
    int        userorder=0;
    int        yangi;
    
    /* Assume if there are any attributes, they are first in the list, mask
       them by raising low to skip them */
    for (low=0; low<upper; low++)
	if ((xa = xml_child_i(xp, low)) == NULL || xml_type(xa)!=CX_ATTR)
	    break;
    /* Find if non-config and if ordered-by-user */
    if (yang_config(yc)==0)
	userorder = 1;
    else if (yang_keyword_get(yc) == Y_LIST || yang_keyword_get(yc) == Y_LEAF_LIST)
	userorder = (yang_find(yc, Y_ORDERED_BY, "user") != NULL);
    yangi = yang_order(yc);
    return xml_search1(xp, x1, userorder, yangi, low, upper, xretp);
}

/*! Insert xn in xp:s sorted child list (special case of ordered-by user)
 * @param[in] xp      Parent xml node. If NULL just remove from old parent.
 * @param[in] xn      Child xml node to insert under xp
 * @param[in] yn      Yang stmt of xml child node
 * @param[in] ins     Insert operation (if ordered-by user)
 * @param[in] key_val Key if LIST and ins is before/after, val if LEAF_LIST
 * @param[in] nsc_key Network namespace for key
 * @retval    i       Order where xn should be inserted into xp:s children
 * @retval   -1       Error
 * LIST: RFC 7950 7.8.6:
 * The value of the "key" attribute is the key predicates of the
 *  full instance identifier (see Section 9.13) for the list entry.
 * This means the value can be [x='a'] but the full instance-id should be prepended,
 * such as /ex:system/ex:services[x='a']
 * 
 * LEAF-LIST: RFC7950 7.7.9 
 *                       yang:insert="after"
 *                       yang:value="3des-cbc">blowfish-cbc</cipher>)
 */
static int
xml_insert_userorder(cxobj           *xp,
		     cxobj           *xn,
		     yang_stmt       *yn,
		     int              mid,
		     enum insert_type ins,
		     char            *key_val,
		     cvec            *nsc_key)
{
    int        retval = -1;
    int        i;
    cxobj     *xc;
    yang_stmt *yc;

    switch (ins){
    case INS_FIRST:
	for (i=mid-1; i>=0; i--){ /* decrement */
	    xc = xml_child_i(xp, i);
	    yc = xml_spec(xc);
	    if (yc != yn){
		retval = i+1;
		goto done;
	    }
	}
	retval = i+1;
	break;
    case INS_LAST:
	for (i=mid+1; i<xml_child_nr(xp); i++){ /* First increment */
	    xc = xml_child_i(xp, i);
	    yc = xml_spec(xc);
	    if (yc != yn){
		retval = i;
		goto done;
	    }
	}
	retval = i;
	break;
    case INS_BEFORE:
    case INS_AFTER: /* see retval handling different between before and after */
	if (key_val == NULL)
	    /* shouldnt happen */
	    clicon_err(OE_YANG, 0, "Missing key/value attribute when insert is before");
	else{
	    switch (yang_keyword_get(yn)){
	    case Y_LEAF_LIST:
		if ((xc = xpath_first(xp, nsc_key, "%s[.='%s']", xml_name(xn), key_val)) == NULL)
		    clicon_err(OE_YANG, 0, "bad-attribute: value, missing-instance: %s", key_val);				    
		else {
		    if ((i = xml_child_order(xp, xc)) < 0)
			clicon_err(OE_YANG, 0, "internal error xpath found but not in child list");
		    else
			retval = (ins==INS_BEFORE)?i:i+1; 
		}
		break;
	    case Y_LIST:
		if ((xc = xpath_first(xp, nsc_key, "%s%s", xml_name(xn), key_val)) == NULL)
		    clicon_err(OE_YANG, 0, "bad-attribute: key, missing-instance: %s", key_val);				    
		else {
		    if ((i = xml_child_order(xp, xc)) < 0)
			clicon_err(OE_YANG, 0, "internal error xpath found but not in child list");
		    else
			retval = (ins==INS_BEFORE)?i:i+1; 
		}
		break;
	    default:
		clicon_err(OE_YANG, 0, "insert only for leaf or leaf-list");
		break;
	    } /* switch */
	}
    }
 done:
    return retval;
}

/*! Insert xn in xp:s sorted child list
 * Find a point in xp childvec with two adjacent nodes xi,xi+1 such that
 * xi<=xn<=xi+1 or xn<=x0 or xmax<=xn
 * @param[in] xp      Parent xml node. If NULL just remove from old parent.
 * @param[in] xn      Child xml node to insert under xp
 * @param[in] yn      Yang stmt of xml child node
 * @param[in] yni     yang order
 * @param[in] userorder Set if ordered-by user, otherwise 0
 * @param[in] ins     Insert operation (if ordered-by user)
 * @param[in] key_val Key if LIST and ins is before/after, val if LEAF_LIST
 * @param[in] nsc_key Network namespace for key
 * @param[in] low     Lower range limit
 * @param[in] upper   Upper range limit
 * @retval    i       Order where xn should be inserted into xp:s children
 * @retval   -1       Error
 */
static int
xml_insert2(cxobj           *xp,
	    cxobj           *xn,
	    yang_stmt       *yn,
	    int              yni,
	    int              userorder,
	    enum insert_type ins,
	    char            *key_val,
	    cvec            *nsc_key,
	    int              low, 
	    int              upper)
{
    int        retval = -1;
    int        mid;
    int        cmp;
    cxobj     *xc;
    yang_stmt *yc;
    
    if (low > upper){  /* beyond range */
	clicon_err(OE_XML, 0, "low>upper %d %d", low, upper);	
	goto done;
    }
    if (low == upper){
	retval = low;
	goto done;
    }
    mid = (low + upper) / 2;
    if (mid >= xml_child_nr(xp)){  /* beyond range */
	clicon_err(OE_XML, 0, "Beyond range %d %d %d", low, mid, upper);	
	goto done;
    }
    xc = xml_child_i(xp, mid);
    if ((yc = xml_spec(xc)) == NULL){
	if (xml_type(xc) != CX_ELMNT)
	    clicon_err(OE_XML, 0, "No spec found %s (wrong xml type:%s)",
		       xml_name(xc), xml_type2str(xml_type(xc)));
	else
	    clicon_err(OE_XML, 0, "No spec found %s", xml_name(xc));	
	goto done;
    }
    if (yc == yn){ /* Same yang */
	if (userorder){ /* append: increment linearly until no longer equal */
	    retval = xml_insert_userorder(xp, xn, yn, mid, ins, key_val, nsc_key);
	    goto done;
	}
	else /* Ordered by system */
	    cmp = xml_cmp(xn, xc, 0);
    }
    else{ /* Not equal yang - compute diff */
	cmp = yni - yang_order(yc);
	/* One case is a choice where 
	 * xc = <tcp/>, xn = <udp/>
	 * same order but different yang spec
	 */
    }
    if (low +1 == upper){ /* termination criterium */
	if (cmp<0) {
	    retval = mid;
	    goto done;
	}
	retval = mid+1;
	goto done;
    }
    if (cmp == 0){
	retval = mid;
	goto done;
    }
    else if (cmp < 0)
	return xml_insert2(xp, xn, yn, yni, userorder, ins, key_val, nsc_key, low, mid);
    else 
	return xml_insert2(xp, xn, yn, yni, userorder, ins, key_val, nsc_key, mid+1, upper);
 done:
    return retval;
}

/*! Insert xc as child to xp in sorted place. Remove xc from previous parent.
 * @param[in] xp      Parent xml node. If NULL just remove from old parent.
 * @param[in] x       Child xml node to insert under xp
 * @param[in] ins     Insert operation (if ordered-by user)
 * @param[in] key_val Key if x is LIST and ins is before/after, val if LEAF_LIST
 * @param[in] nsc_key Network namespace for key
 * @retval    0       OK
 * @retval   -1       Error
 * @see xml_addsub where xc is appended. xml_insert is xml_addsub();xml_sort()
 */
int
xml_insert(cxobj           *xp,
	   cxobj           *xi,
	   enum insert_type ins,
	   char            *key_val,
	   cvec            *nsc_key)
{
    int        retval = -1;
    cxobj     *xa;
    int        low = 0;
    int        upper;
    yang_stmt *y;
    int        userorder= 0;
    int        yi; /* Global yang-stmt order */
    int        i;

    /* Ensure the intermediate state that xp is parent of x but has not yet been
     * added as a child
     */
    if (xml_parent(xi) != NULL){
	clicon_err(OE_XML, 0, "XML node %s should not have parent", xml_name(xi));
	goto done;
    }
    if ((y = xml_spec(xi)) == NULL){
	clicon_err(OE_XML, 0, "No spec found %s", xml_name(xi));
	goto done;
    }
    upper = xml_child_nr(xp);
    /* Assume if there are any attributes, they are first in the list, mask
       them by raising low to skip them */
    for (low=0; low<upper; low++)
	if ((xa = xml_child_i(xp, low)) == NULL || xml_type(xa)!=CX_ATTR)
	    break;
    /* Find if non-config and if ordered-by-user */
    if (yang_config(y)==0)
	userorder = 1;
    else if (yang_keyword_get(y) == Y_LIST || yang_keyword_get(y) == Y_LEAF_LIST)
	userorder = (yang_find(y, Y_ORDERED_BY, "user") != NULL);
    yi = yang_order(y);
    if ((i = xml_insert2(xp, xi, y, yi,
			 userorder, ins, key_val, nsc_key,
			 low, upper)) < 0)
	goto done;
    if (xml_child_insert_pos(xp, xi, i) < 0)
	goto done;
    xml_parent_set(xi, xp);
    /* clear namespace context cache of child */
    nscache_clear(xi);

    retval = 0;
 done:
    return retval;
}

/*! Verify all children of XML node are sorted according to xml_sort()
 * @param[in]   x       XML node. Check its children
 * @param[in]   arg     Dummy. Ensures xml_apply can be used with this fn
 @ @retval      0       Sorted
 @ @retval     -1       Not sorted
 * @see xml_apply
 */
int
xml_sort_verify(cxobj *x0,
		void  *arg)
{
    int    retval = -1;
    cxobj *x = NULL;
    cxobj *xprev = NULL;
    yang_stmt *ys;

    /* Abort sort if non-config (=state) data */
    if ((ys = xml_spec(x0)) != 0 && yang_config(ys)==0){
	retval = 1;
	goto done;
    }
    xml_enumerate_children(x0);
    while ((x = xml_child_each(x0, x, -1)) != NULL) {
	if (xprev != NULL){ /* Check xprev <= x */
	    if (xml_cmp(xprev, x, 1) > 0)
		goto done;
	}
	xprev = x;
    }
    retval = 0;
 done:
    return retval;
}

/*! Given child tree x1c, find matching child in base tree x0 and return as x0cp
 * @param[in]  x0      Base tree node
 * @param[in]  x1c     Modification tree child
 * @param[in]  yc      Yang spec of tree child
 * @param[out] x0cp    Matching base tree child (if any)
 * @retval     0       OK
 * @retval    -1       Error
 */
int
match_base_child(cxobj      *x0, 
		 cxobj      *x1c,
		 yang_stmt  *yc,
		 cxobj     **x0cp)
{
    int        retval = -1;
    cvec      *cvk = NULL; /* vector of index keys */
    cg_var    *cvi;
    cxobj     *xb;
    char      *keyname;
    cxobj     *x0c = NULL;
    yang_stmt *y0c;
    yang_stmt *y0p;
    yang_stmt *yp; /* yang parent */
    
    *x0cp = NULL; /* init return value */
    /* Special case is if yc parent (yp) is choice/case
     * then find x0 child with same yc even though it does not match lexically
     * However this will give another y0c != yc
     */
    if ((yp = yang_choice(yc)) != NULL){
	x0c = NULL;
	while ((x0c = xml_child_each(x0, x0c, CX_ELMNT)) != NULL) {
	    if ((y0c = xml_spec(x0c)) != NULL &&
		(y0p = yang_choice(y0c)) != NULL &&
		y0p == yp)
		break;	/* x0c will have a value */
	}
	goto ok; /* What to do if not found? */
    }
    switch (yang_keyword_get(yc)){
    case Y_CONTAINER: 	/* Equal regardless */
    case Y_LEAF: 	/* Equal regardless */
	break;
    case Y_LEAF_LIST: /* Match with name and value */
	if (xml_body(x1c) == NULL){ /* Treat as empty string */
	    //	    assert(0);
	    goto ok;
	}
	break;
    case Y_LIST: /* Match with key values */
	cvk = yang_cvec_get(yc); /* Use Y_LIST cache, see ys_populate_list() */
	/* Count number of key indexes 
	 * Then create two vectors one with names and one with values of x1c,
	 * ec: keyvec: [a,b,c]  keyval: [1,2,3]
	 */
	cvi = NULL; 
	while ((cvi = cvec_each(cvk, cvi)) != NULL) {
	    keyname = cv_string_get(cvi);
	    //	    keyvec[i] = keyname;
	    if ((xb = xml_find(x1c, keyname)) == NULL){
		goto ok;
	    }
	}
    default:
	break;
    }
    /* Get match. */
    xml_search(x0, x1c, yc, &x0c);
 ok:
    *x0cp = x0c;
    retval = 0;
    return retval;
}
	   
/*! Experimental API for binary search
 *
 * Create a temporary search object: a list (xc) with a key (xk) and call the binary search.
 * @param[in]  xp      Parent xml node. 
 * @param[in]  yc      Yang spec of list child
 * @param[in]  cvk     List of keys and values as CLIgen vector
 * @param[out] xret    Found XML object, NULL if not founs
 * @retval     0       OK, see xret
 * @code
 *    cvec        *cvk = NULL; vector of index keys 
 *    ... Populate cvk with key/values eg a:5 b:6
 *    if (xml_binsearch(xp, yc, cvk, xp) < 0)
 *       err;
 * @endcode
 * @retval    -1       Error
 * Can extend to leaf-list?
 */
int
xml_binsearch(cxobj     *xp,
	      yang_stmt *yc,
	      cvec      *cvk,
	      cxobj    **xretp)
{
    int        retval = -1;
    cxobj     *xc = NULL;
    cxobj     *xk;
    cg_var    *cvi = NULL;
    cbuf      *cb = NULL;
    yang_stmt *yk;
    char      *name;
    
    if (yc == NULL || xml_spec(xp) == NULL){
	clicon_err(OE_YANG, ENOENT, "yang spec not found");
	goto done;
    }
    name = yang_argument_get(yc);
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    cprintf(cb, "<%s>", name);
    cvi = NULL;
    while ((cvi = cvec_each(cvk, cvi)) != NULL) {
	cprintf(cb, "<%s>%s</%s>",
		cv_name_get(cvi),
		cv_string_get(cvi),
		cv_name_get(cvi));
    }
    cprintf(cb, "</%s>", name);
    if (xml_parse_string(cbuf_get(cb), yc, &xc) < 0)
	goto done;
    if (xml_rootchild(xc, 0, &xc) < 0)
	goto done;
    if (xml_spec_set(xc, yc) < 0)
	goto done;
    xk = NULL;
    while ((xk = xml_child_each(xc, xk, CX_ELMNT)) != NULL) {
	if ((yk = yang_find(yc, Y_LEAF, xml_name(xk))) == NULL){
	    clicon_err(OE_YANG, ENOENT, "yang spec of key %s not found", xml_name(xk));
	    goto done; 
	}
	if (xml_spec_set(xk, yk) < 0) 
	    goto done;
    }
    retval = xml_search(xp, xc, yc, xretp);
 done:
    if (cb)
	cbuf_free(cb);
    if (xc)
	xml_free(xc);
    return retval;
}
