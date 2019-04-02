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

  * Datastore text-based XML read functions
 */
#ifndef _CLIXON_DATASTORE_READ_H
#define _CLIXON_DATASTORE_READ_H

/*
 * Prototypes
 */
int xmldb_get_cache(clicon_handle h, const char *db, char *xpath, int config, cxobj **xret, modstate_diff_t *msd);
int xmldb_get_nocache(clicon_handle h, const char *db, char *xpath, int config, cxobj **xret, modstate_diff_t *msd);
int xmldb_readfile(clicon_handle h, const char *db, yang_spec *yspec, cxobj **xp,	modstate_diff_t *msd);

#endif /* _CLIXON_DATASTORE_READ_H */