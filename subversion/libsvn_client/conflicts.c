/*
 * conflicts.c:  conflict resolver implementation
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

/* ==================================================================== */



/*** Includes. ***/

#include "svn_types.h"
#include "svn_wc.h"
#include "svn_client.h"
#include "svn_error.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_pools.h"
#include "svn_props.h"
#include "svn_hash.h"
#include "svn_sorts.h"
#include "client.h"
#include "private/svn_sorts_private.h"
#include "private/svn_token.h"
#include "private/svn_wc_private.h"

#include "svn_private_config.h"

#define ARRAY_LEN(ary) ((sizeof (ary)) / (sizeof ((ary)[0])))


/*** Dealing with conflicts. ***/

/* Describe a tree conflict. */
typedef svn_error_t *(*tree_conflict_get_description_func_t)(
  const char **change_description,
  svn_client_conflict_t *conflict,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool);

/* Get more information about a tree conflict.
 * This function may contact the repository. */
typedef svn_error_t *(*tree_conflict_get_details_func_t)(
  svn_client_conflict_t *conflict,
  apr_pool_t *scratch_pool);

struct svn_client_conflict_t
{
  const char *local_abspath;
  svn_client_ctx_t *ctx;
  apr_hash_t *prop_conflicts;

  /* Indicate which options were chosen to resolve a text or tree conflict
   * on the conflited node. */
  svn_client_conflict_option_id_t resolution_text;
  svn_client_conflict_option_id_t resolution_tree;

  /* A mapping from const char* property name to pointers to
   * svn_client_conflict_option_t for all properties which had their
   * conflicts resolved. Indicates which options were chosen to resolve
   * the property conflicts. */
  apr_hash_t *resolved_props;

  /* Ask a tree conflict to describe itself. */
  tree_conflict_get_description_func_t
    tree_conflict_get_incoming_description_func;
  tree_conflict_get_description_func_t
    tree_conflict_get_local_description_func;

  /* Ask a tree conflict to find out more information about itself
   * by contacting the repository. */
  tree_conflict_get_details_func_t tree_conflict_get_incoming_details_func;
  tree_conflict_get_details_func_t tree_conflict_get_local_details_func;

  /* Any additional information found can be stored here and may be used
   * when describing a tree conflict. */
  void *tree_conflict_incoming_details;
  void *tree_conflict_local_details;

  /* The pool this conflict was allocated from. */
  apr_pool_t *pool;

  /* Conflict data provided by libsvn_wc. */
  const svn_wc_conflict_description2_t *legacy_text_conflict;
  const char *legacy_prop_conflict_propname;
  const svn_wc_conflict_description2_t *legacy_tree_conflict;
};

/* Resolves conflict to OPTION and sets CONFLICT->RESOLUTION accordingly.
 *
 * May raise an error in case the conflict could not be resolved. A common
 * case would be a tree conflict the resolution of which depends on other
 * tree conflicts to be resolved first. */
typedef svn_error_t *(*conflict_option_resolve_func_t)(
  svn_client_conflict_option_t *option,
  svn_client_conflict_t *conflict,
  apr_pool_t *scratch_pool);

struct svn_client_conflict_option_t
{
  svn_client_conflict_option_id_t id;
  const char *description;

  svn_client_conflict_t *conflict;
  conflict_option_resolve_func_t do_resolve_func;

  /* Data which is specific to particular conflicts and options. */
  union {
    struct {
      /* Indicates the property to resolve in case of a property conflict.
       * If set to "", all properties are resolved to this option. */
      const char *propname;

      /* A merged property value, if supplied by the API user, else NULL. */
      const svn_string_t *merged_propval;
    } prop;
  } type_data;

};

/*
 * Return a legacy conflict choice corresponding to OPTION_ID.
 * Return svn_wc_conflict_choose_undefined if no corresponding
 * legacy conflict choice exists.
 */
static svn_wc_conflict_choice_t
conflict_option_id_to_wc_conflict_choice(
  svn_client_conflict_option_id_t option_id)
{

  switch (option_id)
    {
      case svn_client_conflict_option_undefined:
        return svn_wc_conflict_choose_undefined;

      case svn_client_conflict_option_postpone:
        return svn_wc_conflict_choose_postpone;

      case svn_client_conflict_option_base_text:
        return svn_wc_conflict_choose_base;

      case svn_client_conflict_option_incoming_text:
        return svn_wc_conflict_choose_theirs_full;

      case svn_client_conflict_option_working_text:
        return svn_wc_conflict_choose_mine_full;

      case svn_client_conflict_option_incoming_text_where_conflicted:
        return svn_wc_conflict_choose_theirs_conflict;

      case svn_client_conflict_option_working_text_where_conflicted:
        return svn_wc_conflict_choose_mine_conflict;

      case svn_client_conflict_option_merged_text:
        return svn_wc_conflict_choose_merged;

      case svn_client_conflict_option_unspecified:
        return svn_wc_conflict_choose_unspecified;

      default:
        break;
    }

  return svn_wc_conflict_choose_undefined;
}

static void
add_legacy_desc_to_conflict(const svn_wc_conflict_description2_t *desc,
                            svn_client_conflict_t *conflict,
                            apr_pool_t *result_pool)
{
  switch (desc->kind)
    {
      case svn_wc_conflict_kind_text:
        conflict->legacy_text_conflict = desc;
        break;

      case svn_wc_conflict_kind_property:
        if (conflict->prop_conflicts == NULL)
          conflict->prop_conflicts = apr_hash_make(result_pool);
        svn_hash_sets(conflict->prop_conflicts, desc->property_name, desc);
        conflict->legacy_prop_conflict_propname = desc->property_name;
        break;

      case svn_wc_conflict_kind_tree:
        conflict->legacy_tree_conflict = desc;
        break;

      default:
        SVN_ERR_ASSERT_NO_RETURN(FALSE); /* unknown kind of conflict */
    }
}

/* A map for svn_wc_conflict_action_t values to strings */
static const svn_token_map_t map_conflict_action[] =
{
  { "edit",             svn_wc_conflict_action_edit },
  { "delete",           svn_wc_conflict_action_delete },
  { "add",              svn_wc_conflict_action_add },
  { "replace",          svn_wc_conflict_action_replace },
  { NULL,               0 }
};

/* A map for svn_wc_conflict_reason_t values to strings */
static const svn_token_map_t map_conflict_reason[] =
{
  { "edit",             svn_wc_conflict_reason_edited },
  { "delete",           svn_wc_conflict_reason_deleted },
  { "missing",          svn_wc_conflict_reason_missing },
  { "obstruction",      svn_wc_conflict_reason_obstructed },
  { "add",              svn_wc_conflict_reason_added },
  { "replace",          svn_wc_conflict_reason_replaced },
  { "unversioned",      svn_wc_conflict_reason_unversioned },
  { "moved-away",       svn_wc_conflict_reason_moved_away },
  { "moved-here",       svn_wc_conflict_reason_moved_here },
  { NULL,               0 }
};

/* Describes a server-side move (really a copy+delete within the same
 * revision) which was identified by scanning the revision log. */
struct repos_move_info {
  /* The repository relpath the node was moved from. */
  const char *moved_from_repos_relpath;

  /* The repository relpath the node was moved to. */
  const char *moved_to_repos_relpath;
 
  /* The revision in which this move was committed. */
  svn_revnum_t rev;

  /* The author who commited the revision in which this move was committed. */
  const char *rev_author;

  /* The copyfrom revision of the moved-to path. */
  svn_revnum_t copyfrom_rev;

  /* Prev and next pointers. NULL if no prior or next move exists. */
  struct repos_move_info *prev;
  struct repos_move_info *next;
};

/* Set *RELATED to true if the deleted node at repository relpath
 * DELETED_REPOS_RELPATH@DELETED_REV is ancestrally related to the node at
 * repository relpath COPYFROM_PATH@COPYFROM_REV, else set it to false. */
static svn_error_t *
check_move_ancestry(svn_boolean_t *related,
                    const char *repos_root_url,
                    const char *deleted_repos_relpath,
                    svn_revnum_t deleted_rev,
                    const char *copyfrom_path,
                    svn_revnum_t copyfrom_rev,
                    svn_client_ctx_t *ctx,
                    apr_pool_t *scratch_pool)
{
  apr_hash_t *locations;
  const char *deleted_url;
  const char *deleted_location;
  svn_ra_session_t *ra_session;
  const char *corrected_url;
  apr_array_header_t *location_revisions;

  *related = FALSE;

  location_revisions = apr_array_make(scratch_pool, 1, sizeof(svn_revnum_t));
  APR_ARRAY_PUSH(location_revisions, svn_revnum_t) = copyfrom_rev;
  deleted_url = svn_uri_canonicalize(apr_pstrcat(scratch_pool,
                                                 repos_root_url, "/",
                                                 deleted_repos_relpath,
                                                 NULL),
                                     scratch_pool);
  SVN_ERR(svn_client__open_ra_session_internal(&ra_session, &corrected_url,
                                               deleted_url, NULL,
                                               NULL, FALSE, FALSE,
                                               ctx, scratch_pool,
                                               scratch_pool));
  SVN_ERR(svn_ra_get_locations(ra_session, &locations, "",
                               deleted_rev - 1, location_revisions,
                               scratch_pool));

  deleted_location = apr_hash_get(locations, &copyfrom_rev,
                                  sizeof(svn_revnum_t));
  if (deleted_location)
    {
      if (deleted_location[0] == '/')
        deleted_location++;
      *related = (strcmp(deleted_location, copyfrom_path) == 0);
    }

  return SVN_NO_ERROR;
}

struct copy_info {
  const char *copyto_path;
  const char *copyfrom_path;
  svn_revnum_t copyfrom_rev;
};

/* Update MOVES_TABLE and MOVED_PATHS based on information from
 * revision data in LOG_ENTRY, COPIES, and DELETED_PATHS. */
static svn_error_t *
find_moves_in_revision(apr_hash_t *moves_table,
                       apr_hash_t *moved_paths,
                       svn_log_entry_t *log_entry,
                       apr_hash_t *copies,
                       apr_array_header_t *deleted_paths,
                       const char *repos_root_url,
                       svn_client_ctx_t *ctx,
                       apr_pool_t *result_pool,
                       apr_pool_t *scratch_pool)
{
  apr_pool_t *iterpool;
  apr_array_header_t *copies_with_same_source_path;
  svn_boolean_t related;
  int i, j;

  iterpool = svn_pool_create(scratch_pool);
  for (i = 0; i < deleted_paths->nelts; i++)
    {
      const char *deleted_repos_relpath;

      deleted_repos_relpath = APR_ARRAY_IDX(deleted_paths, i, const char *);

      /* See if we can match any copies to this deleted path. */
      copies_with_same_source_path = apr_hash_get(copies,
                                                  deleted_repos_relpath,
                                                  APR_HASH_KEY_STRING);
      if (copies_with_same_source_path == NULL)
        continue;

      for (j = 0; j < copies_with_same_source_path->nelts; j++)
        {
          struct copy_info *copy;
          struct repos_move_info *move;
          struct repos_move_info *next_move;
          svn_string_t *author;
          apr_array_header_t *moves;
          
          svn_pool_clear(iterpool);

          copy = APR_ARRAY_IDX(copies_with_same_source_path, j,
                               struct copy_info *);

          /* We found a deleted node which matches the copyfrom path of
           * a copied node. Verify that the deleted node is an ancestor
           * of the copied node. When tracing back history of the deleted node
           * from revision log_entry->revision-1 (where the deleted node is
           * guaranteed to exist) to the copyfrom-revision, we must end up
           * at the copyfrom-path. */
          SVN_ERR(check_move_ancestry(&related, repos_root_url,
                                      deleted_repos_relpath,
                                      log_entry->revision,
                                      copy->copyfrom_path,
                                      copy->copyfrom_rev,
                                      ctx, iterpool));
          if (!related)
            continue;

          /* ### TODO:
           * If the node was not copied from the most recent last-changed
           * revision of the deleted node, this is not a move but a
           * "copy from the past + delete". */

          /* Remember details of this move. */
          move = apr_pcalloc(result_pool, sizeof(*move));
          move->moved_from_repos_relpath = apr_pstrdup(result_pool,
                                                       deleted_repos_relpath);
          move->moved_to_repos_relpath = apr_pstrdup(result_pool,
                                                     copy->copyto_path);
          move->rev = log_entry->revision;
          author = svn_hash_gets(log_entry->revprops, SVN_PROP_REVISION_AUTHOR);
          move->rev_author = apr_pstrdup(result_pool, author->data);
          move->copyfrom_rev = copy->copyfrom_rev;

          /* Link together multiple moves of the same node.
           * Note that we're traversing history backwards, so moves already
           * present in the list happened in younger revisions. */
          next_move = svn_hash_gets(moved_paths, move->moved_to_repos_relpath);
          if (next_move)
            {
              /* Tracing back history of the delete-half of the next move
               * to the copyfrom-revision of the prior move we must end up
               * at the delete-half of the prior move. */
              SVN_ERR(check_move_ancestry(&related, repos_root_url,
                                          next_move->moved_from_repos_relpath,
                                          next_move->rev,
                                          move->moved_from_repos_relpath,
                                          move->copyfrom_rev,
                                          ctx, iterpool));
              if (related)
                {
                  SVN_ERR_ASSERT(move->rev < next_move->rev);

                  /* Prepend this move to the linked list. */
                  move->next = next_move;
                  next_move->prev = move;
                }
            }

          /* Make this move the head of our next-move linking map. */
          svn_hash_sets(moved_paths, move->moved_from_repos_relpath, move);

          /* Add this move to the list of moves in this revision. */
          moves = apr_hash_get(moves_table, &move->rev, sizeof(svn_revnum_t));
          if (moves == NULL)
            {
              /* This is the first move in this revision. Create the list. */
              moves = apr_array_make(result_pool, 1,
                                     sizeof(struct repos_move_info *));
              apr_hash_set(moves_table, &move->rev, sizeof(svn_revnum_t),
                           moves);
            }
          APR_ARRAY_PUSH(moves, struct repos_move_info *) = move;
        }
    }
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

struct find_deleted_rev_baton
{
  /* Variables below are arguments provided by the caller of
   * svn_ra_get_log2(). */
  const char *deleted_repos_relpath;
  const char *related_repos_relpath;
  svn_revnum_t related_repos_peg_rev;
  const char *repos_root_url;
  const char *repos_uuid;
  svn_client_ctx_t *ctx;

  /* Variables below are results for the caller of svn_ra_get_log2(). */
  svn_revnum_t deleted_rev;
  const char *deleted_rev_author;
  svn_node_kind_t replacing_node_kind;
  apr_pool_t *result_pool;

  /* A hash table mapping a revision number to an array of struct
   * repos_move_info * elements, describing moves.
   *
   * Must be allocated in RESULT_POOL by the caller of svn_ra_get_log2().
   *
   * If the node was moved, the DELETED_REV is present in this table,
   * perhaps along with additional revisions.
   *
   * Given a sequence of moves which happened in the repository, such as:
   *   rA: mv x->z
   *   rA: mv a->b
   *   rB: mv b->c
   *   rC: mv c->d
   * we map each revision number to all the moves which happened in the
   * revision, which looks as follows: 
   *   rA : [(x->z), (a->b)]
   *   rB : [(b->c)]
   *   rC : [(c->d)]
   * This allows us to later find relevant moves based on a revision number.
   *
   * Additionally, we embed the number of the revision in which a move was
   * found inside the repos_move_info structure:
   *   rA : [(rA, x->z), (rA, a->b)]
   *   rB : [(rB, b->c)]
   *   rC : [(rC, c->d)]
   * And also, all moves pertaining to the same node are chained into a
   * doubly-linked list via 'next' and 'prev' pointers (see definition of
   * struct repos_move_info). This can be visualized as follows:
   *   rA : [(rA, x->z, prev=>NULL, next=>NULL),
   *         (rA, a->b, prev=>NULL, next=>(rB, b->c))]
   *   rB : [(rB, b->c), prev=>(rA, a->b), next=>(rC, c->d)]
   *   rC : [(rC, c->d), prev=>(rB, c->d), next=>NULL]
   * This way, we can look up all moves relevant to a node, forwards and
   * backwards in history, once we have located one move in the chain.
   *
   * In the above example, the data tells us that within the revision
   * range rA:C, a was moved to d. However, within the revision range
   * rA;B, a was moved to b.
   */
  apr_hash_t *moves_table;

  /* Variables below hold state for find_deleted_rev() and are not
   * intended to be used by the caller of svn_ra_get_log2().
   * Like all other variables, they must be initialized, however. */

  /* Temporary map of moved paths to struct repos_move_info.
   * Used to link multiple moves of the same node across revisions. */
  apr_hash_t *moved_paths;
};

/* Implements svn_log_entry_receiver_t.
 *
 * Find the revision in which a node, optionally ancestrally related to the
 * node specified via find_deleted_rev_baton, was deleted, When the revision
 * was found, store it in BATON->DELETED_REV and abort the log operation
 * by raising SVN_ERR_CANCELLED.
 *
 * If no such revision can be found, leave BATON->DELETED_REV and
 * BATON->REPLACING_NODE_KIND alone.
 *
 * If the node was replaced, set BATON->REPLACING_NODE_KIND to the node
 * kind of the node which replaced the original node. If the node was not
 * replaced, set BATON->REPLACING_NODE_KIND to svn_node_none.
 *
 * This function answers the same question as svn_ra_get_deleted_rev() but
 * works in cases where we do not already know a revision in which the deleted
 * node once used to exist.
 * 
 * If the node node was moved, rather than deleted, return move information
 * in BATON->MOVE.
 */
static svn_error_t *
find_deleted_rev(void *baton,
                 svn_log_entry_t *log_entry,
                 apr_pool_t *scratch_pool)
{
  struct find_deleted_rev_baton *b = baton;
  apr_hash_index_t *hi;
  apr_pool_t *iterpool;
  svn_boolean_t deleted_node_found = FALSE;
  apr_array_header_t *deleted_paths;
  apr_hash_t *copies;

  /* No paths were changed in this revision.  Nothing to do. */
  if (! log_entry->changed_paths2)
    return SVN_NO_ERROR;

  copies = apr_hash_make(scratch_pool);
  deleted_paths = apr_array_make(scratch_pool, 0, sizeof(const char *));
  iterpool = svn_pool_create(scratch_pool);
  for (hi = apr_hash_first(scratch_pool, log_entry->changed_paths2);
       hi != NULL;
       hi = apr_hash_next(hi))
    {
      const char *changed_path = apr_hash_this_key(hi);
      svn_log_changed_path2_t *log_item = apr_hash_this_val(hi);

      svn_pool_clear(iterpool);

      /* ### Remove leading slash from paths in log entries. */
      if (changed_path[0] == '/')
          changed_path++;

      /* For move detection, scan for copied nodes in this revision. */
      if (log_item->action == 'A' && log_item->copyfrom_path)
        {
          struct copy_info *copy;
          apr_array_header_t *copies_with_same_source_path;

          if (log_item->copyfrom_path[0] == '/')
            log_item->copyfrom_path++;

          copy = apr_palloc(scratch_pool, sizeof(*copy));
          copy->copyto_path = changed_path;
          copy->copyfrom_path = log_item->copyfrom_path;
          copy->copyfrom_rev = log_item->copyfrom_rev;
          copies_with_same_source_path = apr_hash_get(copies,
                                                      log_item->copyfrom_path,
                                                      APR_HASH_KEY_STRING);
          if (copies_with_same_source_path == NULL)
            {
              copies_with_same_source_path = apr_array_make(
                                               scratch_pool, 1,
                                               sizeof(struct copy_info *));
              apr_hash_set(copies, copy->copyfrom_path, APR_HASH_KEY_STRING,
                           copies_with_same_source_path);
            }
          APR_ARRAY_PUSH(copies_with_same_source_path,
                         struct copy_info *) = copy;
        }

      /* For move detection, store all deleted_paths.
       *
       * ### This also stores deletions which happened inside copies.
       * ### But we are not able to handle them at present.
       * ### Consider: cp A B; mv B/foo C/foo
       * ### Copyfrom for C/foo is now A/foo, even though C/foo was moved
       * ### here from B/foo. We don't detect such moves at present since
       * ### A/foo was not deleted. It is B/foo which was deleted.
       */
      if (log_item->action == 'D' || log_item->action == 'R')
        APR_ARRAY_PUSH(deleted_paths, const char *) =
          apr_pstrdup(scratch_pool, changed_path);

      /* Check if we found the deleted node we're looking for. */
      if (!deleted_node_found &&
          svn_path_compare_paths(b->deleted_repos_relpath, changed_path) == 0 &&
          (log_item->action == 'D' || log_item->action == 'R'))
        {
          deleted_node_found = TRUE;

          if (b->related_repos_relpath != NULL &&
              b->related_repos_peg_rev != SVN_INVALID_REVNUM)
            {
              svn_client__pathrev_t *yca_loc = NULL;
              svn_client__pathrev_t *loc1;
              svn_client__pathrev_t *loc2;

              /* We found a deleted node which occupies the correct path.
               * To be certain that this is the deleted node we're looking for,
               * we must establish whether it is ancestrally related to the
               * "related node" specified in our baton. */
              loc1 = svn_client__pathrev_create_with_relpath(
                       b->repos_root_url, b->repos_uuid,
                       b->related_repos_peg_rev,
                       b->related_repos_relpath, iterpool);
              loc2 = svn_client__pathrev_create_with_relpath(
                       b->repos_root_url, b->repos_uuid,
                       log_entry->revision - 1,
                       b->deleted_repos_relpath, iterpool);
              SVN_ERR(svn_client__get_youngest_common_ancestor(&yca_loc,
                                                               loc1, loc2,
                                                               NULL, b->ctx,
                                                               iterpool,
                                                               iterpool));
              deleted_node_found = (yca_loc != NULL);
            }

          if (deleted_node_found)
            {
              svn_string_t *author;

              b->deleted_rev = log_entry->revision;
              author = svn_hash_gets(log_entry->revprops,
                                     SVN_PROP_REVISION_AUTHOR);
              b->deleted_rev_author = apr_pstrdup(b->result_pool, author->data);
                  
              if (log_item->action == 'R')
                b->replacing_node_kind = log_item->node_kind;
              else
                b->replacing_node_kind = svn_node_none;
            }
        }
    }
  svn_pool_destroy(iterpool);

  /* Check for moves in this revision */
  SVN_ERR(find_moves_in_revision(b->moves_table, b->moved_paths,
                                 log_entry, copies, deleted_paths,
                                 b->repos_root_url, b->ctx,
                                 b->result_pool, scratch_pool));
  if (deleted_node_found)
    {
      /* We're done. Abort the log operation. */
      return svn_error_create(SVN_ERR_CANCELLED, NULL, NULL);
    }

  return SVN_NO_ERROR;
}

/* Return a localised string representation of the local part of a tree
   conflict on a file. */
static svn_error_t *
describe_local_file_node_change(const char **description,
                                svn_client_conflict_t *conflict,
                                apr_pool_t *result_pool,
                                apr_pool_t *scratch_pool)
{
  svn_wc_conflict_reason_t local_change;
  svn_wc_operation_t operation;

  local_change = svn_client_conflict_get_local_change(conflict);
  operation = svn_client_conflict_get_operation(conflict);

  switch (local_change)
    {
      case svn_wc_conflict_reason_edited:
        if (operation == svn_wc_operation_update ||
            operation == svn_wc_operation_switch)
          *description = _("A file containing uncommitted changes was "
                           "found in the working copy.");
        else if (operation == svn_wc_operation_merge)
          *description = _("A file which differs from the corresponding "
                           "file on the merge source branch was found "
                           "in the working copy.");
        break;
      case svn_wc_conflict_reason_obstructed:
        *description = _("A file which already occupies this path was found "
                         "in the working copy.");
        break;
      case svn_wc_conflict_reason_unversioned:
        *description = _("An unversioned file was found in the working "
                         "copy.");
        break;
      case svn_wc_conflict_reason_deleted:
        *description = _("A deleted file was found in the working copy.");
        break;
      case svn_wc_conflict_reason_missing:
        if (operation == svn_wc_operation_update ||
            operation == svn_wc_operation_switch)
          *description = _("No such file was found in the working copy.");
        else if (operation == svn_wc_operation_merge)
          {
            /* ### display deleted revision */
            *description = _("No such file was found in the merge target "
                             "working copy.\nPerhaps the file has been "
                             "deleted or moved away in the repository's "
                             "history?");
          }
        break;
      case svn_wc_conflict_reason_added:
      case svn_wc_conflict_reason_replaced:
        {
          /* ### show more details about copies or replacements? */
          *description = _("A file scheduled to be added to the "
                           "repository in the next commit was found in "
                           "the working copy.");
        }
        break;
      case svn_wc_conflict_reason_moved_away:
        {
          const char *moved_to_abspath;

          SVN_ERR(svn_wc__node_was_moved_away(&moved_to_abspath, NULL, 
                                              conflict->ctx->wc_ctx,
                                              conflict->local_abspath,
                                              scratch_pool,
                                              scratch_pool));
          if (operation == svn_wc_operation_update ||
              operation == svn_wc_operation_switch)
            {
              if (moved_to_abspath == NULL)
                {
                  /* The move no longer exists. */
                  *description = _("The file in the working copy had "
                                   "been moved away at the time this "
                                   "conflict was recorded.");
                }
              else
                {
                  const char *wcroot_abspath;

                  SVN_ERR(svn_wc__get_wcroot(&wcroot_abspath,
                                             conflict->ctx->wc_ctx,
                                             conflict->local_abspath,
                                             scratch_pool,
                                             scratch_pool));
                  *description = apr_psprintf(
                                   result_pool,
                                   _("The file in the working copy was "
                                     "moved away to\n'%s'."),
                                   svn_dirent_local_style(
                                     svn_dirent_skip_ancestor(
                                       wcroot_abspath,
                                       moved_to_abspath),
                                     scratch_pool));
                }
            }
          else if (operation == svn_wc_operation_merge)
            {
              if (moved_to_abspath == NULL)
                {
                  /* The move probably happened in branch history.
                   * This case cannot happen until we detect incoming
                   * moves, which we currently don't do. */
                  /* ### find deleted/moved revision? */
                  *description = _("The file in the working copy had "
                                   "been moved away at the time this "
                                   "conflict was recorded.");
                }
              else
                {
                  /* This is a local move in the working copy. */
                  const char *wcroot_abspath;

                  SVN_ERR(svn_wc__get_wcroot(&wcroot_abspath,
                                             conflict->ctx->wc_ctx,
                                             conflict->local_abspath,
                                             scratch_pool,
                                             scratch_pool));
                  *description = apr_psprintf(
                                   result_pool,
                                   _("The file in the working copy was "
                                     "moved away to\n'%s'."),
                                   svn_dirent_local_style(
                                     svn_dirent_skip_ancestor(
                                       wcroot_abspath,
                                       moved_to_abspath),
                                     scratch_pool));
                }
            }
          break;
        }
      case svn_wc_conflict_reason_moved_here:
        {
          const char *moved_from_abspath;

          SVN_ERR(svn_wc__node_was_moved_here(&moved_from_abspath, NULL, 
                                              conflict->ctx->wc_ctx,
                                              conflict->local_abspath,
                                              scratch_pool,
                                              scratch_pool));
          if (operation == svn_wc_operation_update ||
              operation == svn_wc_operation_switch)
            {
              if (moved_from_abspath == NULL)
                {
                  /* The move no longer exists. */
                  *description = _("A file had been moved here in the "
                                   "working copy at the time this "
                                   "conflict was recorded.");
                }
              else
                {
                  const char *wcroot_abspath;

                  SVN_ERR(svn_wc__get_wcroot(&wcroot_abspath,
                                             conflict->ctx->wc_ctx,
                                             conflict->local_abspath,
                                             scratch_pool,
                                             scratch_pool));
                  *description = apr_psprintf(
                                   result_pool,
                                   _("A file was moved here in the "
                                     "working copy from\n'%s'."),
                                   svn_dirent_local_style(
                                     svn_dirent_skip_ancestor(
                                       wcroot_abspath,
                                       moved_from_abspath),
                                     scratch_pool));
                }
            }
          else if (operation == svn_wc_operation_merge)
            {
              if (moved_from_abspath == NULL)
                {
                  /* The move probably happened in branch history.
                   * This case cannot happen until we detect incoming
                   * moves, which we currently don't do. */
                  /* ### find deleted/moved revision? */
                  *description = _("A file had been moved here in the "
                                   "working copy at the time this "
                                   "conflict was recorded.");
                }
              else
                {
                  const char *wcroot_abspath;

                  SVN_ERR(svn_wc__get_wcroot(&wcroot_abspath,
                                             conflict->ctx->wc_ctx,
                                             conflict->local_abspath,
                                             scratch_pool,
                                             scratch_pool));
                  /* This is a local move in the working copy. */
                  *description = apr_psprintf(
                                   result_pool,
                                   _("A file was moved here in the "
                                     "working copy from\n'%s'."),
                                   svn_dirent_local_style(
                                     svn_dirent_skip_ancestor(
                                       wcroot_abspath,
                                       moved_from_abspath),
                                     scratch_pool));
                }
            }
          break;
        }
    }

  return SVN_NO_ERROR;
}

/* Return a localised string representation of the local part of a tree
   conflict on a directory. */
static svn_error_t *
describe_local_dir_node_change(const char **description,
                               svn_client_conflict_t *conflict,
                               apr_pool_t *result_pool,
                               apr_pool_t *scratch_pool)
{
  svn_wc_conflict_reason_t local_change;
  svn_wc_operation_t operation;

  local_change = svn_client_conflict_get_local_change(conflict);
  operation = svn_client_conflict_get_operation(conflict);

  switch (local_change)
    {
      case svn_wc_conflict_reason_edited:
        if (operation == svn_wc_operation_update ||
            operation == svn_wc_operation_switch)
          *description = _("A directory containing uncommitted changes "
                           "was found in the working copy.");
        else if (operation == svn_wc_operation_merge)
          *description = _("A directory which differs from the "
                           "corresponding directory on the merge source "
                           "branch was found in the working copy.");
        break;
      case svn_wc_conflict_reason_obstructed:
        *description = _("A directory which already occupies this path was "
                         "found in the working copy.");
        break;
      case svn_wc_conflict_reason_unversioned:
        *description = _("An unversioned directory was found in the "
                         "working copy.");
        break;
      case svn_wc_conflict_reason_deleted:
        *description = _("A deleted directory was found in the "
                         "working copy.");
        break;
      case svn_wc_conflict_reason_missing:
        if (operation == svn_wc_operation_update ||
            operation == svn_wc_operation_switch)
          *description = _("No such directory was found in the working "
                           "copy.");
        else if (operation == svn_wc_operation_merge)
          {
            /* ### display deleted revision */
            *description = _("No such directory was found in the merge "
                             "target working copy.\nPerhaps the "
                             "directory has been deleted or moved away "
                             "in the repository's history?");
          }
        break;
      case svn_wc_conflict_reason_added:
      case svn_wc_conflict_reason_replaced:
        {
          /* ### show more details about copies or replacements? */
          *description = _("A directory scheduled to be added to the "
                           "repository in the next commit was found in "
                           "the working copy.");
        }
        break;
      case svn_wc_conflict_reason_moved_away:
        {
          const char *moved_to_abspath;

          SVN_ERR(svn_wc__node_was_moved_away(&moved_to_abspath, NULL, 
                                              conflict->ctx->wc_ctx,
                                              conflict->local_abspath,
                                              scratch_pool,
                                              scratch_pool));
          if (operation == svn_wc_operation_update ||
              operation == svn_wc_operation_switch)
            {
              if (moved_to_abspath == NULL)
                {
                  /* The move no longer exists. */
                  *description = _("The directory in the working copy "
                                   "had been moved away at the time "
                                   "this conflict was recorded.");
                }
              else
                {
                  const char *wcroot_abspath;

                  SVN_ERR(svn_wc__get_wcroot(&wcroot_abspath,
                                             conflict->ctx->wc_ctx,
                                             conflict->local_abspath,
                                             scratch_pool,
                                             scratch_pool));
                  *description = apr_psprintf(
                                   result_pool,
                                   _("The directory in the working copy "
                                     "was moved away to\n'%s'."),
                                   svn_dirent_local_style(
                                     svn_dirent_skip_ancestor(
                                       wcroot_abspath,
                                       moved_to_abspath),
                                     scratch_pool));
                }
            }
          else if (operation == svn_wc_operation_merge)
            {
              if (moved_to_abspath == NULL)
                {
                  /* The move probably happened in branch history.
                   * This case cannot happen until we detect incoming
                   * moves, which we currently don't do. */
                  /* ### find deleted/moved revision? */
                  *description = _("The directory had been moved away "
                                   "at the time this conflict was "
                                   "recorded.");
                }
              else
                {
                  /* This is a local move in the working copy. */
                  const char *wcroot_abspath;

                  SVN_ERR(svn_wc__get_wcroot(&wcroot_abspath,
                                             conflict->ctx->wc_ctx,
                                             conflict->local_abspath,
                                             scratch_pool,
                                             scratch_pool));
                  *description = apr_psprintf(
                                   result_pool,
                                   _("The directory was moved away to\n"
                                     "'%s'."),
                                   svn_dirent_local_style(
                                     svn_dirent_skip_ancestor(
                                       wcroot_abspath,
                                       moved_to_abspath),
                                     scratch_pool));
                }
            }
          }
          break;
      case svn_wc_conflict_reason_moved_here:
        {
          const char *moved_from_abspath;

          SVN_ERR(svn_wc__node_was_moved_here(&moved_from_abspath, NULL, 
                                              conflict->ctx->wc_ctx,
                                              conflict->local_abspath,
                                              scratch_pool,
                                              scratch_pool));
          if (operation == svn_wc_operation_update ||
              operation == svn_wc_operation_switch)
            {
              if (moved_from_abspath == NULL)
                {
                  /* The move no longer exists. */
                  *description = _("A directory had been moved here at "
                                   "the time this conflict was "
                                   "recorded.");
                }
              else
                {
                  const char *wcroot_abspath;

                  SVN_ERR(svn_wc__get_wcroot(&wcroot_abspath,
                                             conflict->ctx->wc_ctx,
                                             conflict->local_abspath,
                                             scratch_pool,
                                             scratch_pool));
                  *description = apr_psprintf(
                                   result_pool,
                                   _("A directory was moved here from\n"
                                     "'%s'."),
                                   svn_dirent_local_style(
                                     svn_dirent_skip_ancestor(
                                       wcroot_abspath,
                                       moved_from_abspath),
                                     scratch_pool));
                }
            }
          else if (operation == svn_wc_operation_merge)
            {
              if (moved_from_abspath == NULL)
                {
                  /* The move probably happened in branch history.
                   * This case cannot happen until we detect incoming
                   * moves, which we currently don't do. */
                  /* ### find deleted/moved revision? */
                  *description = _("A directory had been moved here at "
                                   "the time this conflict was "
                                   "recorded.");
                }
              else
                {
                  /* This is a local move in the working copy. */
                  const char *wcroot_abspath;

                  SVN_ERR(svn_wc__get_wcroot(&wcroot_abspath,
                                             conflict->ctx->wc_ctx,
                                             conflict->local_abspath,
                                             scratch_pool,
                                             scratch_pool));
                  *description = apr_psprintf(
                                   result_pool,
                                   _("A directory was moved here in "
                                     "the working copy from\n'%s'."),
                                   svn_dirent_local_style(
                                     svn_dirent_skip_ancestor(
                                       wcroot_abspath,
                                       moved_from_abspath),
                                     scratch_pool));
                }
            }
        }
    }

  return SVN_NO_ERROR;
}

/* Try to find a revision older than START_REV, and its author, which deleted
 * DELETED_BASENAME in the directory PARENT_REPOS_RELPATH. Assume the deleted
 * node is ancestrally related to RELATED_REPOS_RELPATH@RELATED_PEG_REV.
 * If no such revision can be found, set *DELETED_REV to SVN_INVALID_REVNUM
 * and *DELETED_REV_AUTHOR to NULL.
 * If the node was replaced rather than deleted, set *REPLACING_NODE_KIND to
 * the node kind of the replacing node. Else, set it to svn_node_unknown.
 * Only request the log for revisions up to END_REV from the server.
 * If the deleted node was moved, provide move information in *MOVE.
 * If the node was not moved,set *MOVE to NULL.
 */
static svn_error_t *
find_revision_for_suspected_deletion(svn_revnum_t *deleted_rev,
                                     const char **deleted_rev_author,
                                     svn_node_kind_t *replacing_node_kind,
                                     struct repos_move_info **move,
                                     svn_client_conflict_t *conflict,
                                     const char *deleted_basename,
                                     const char *parent_repos_relpath,
                                     svn_revnum_t start_rev,
                                     svn_revnum_t end_rev,
                                     const char *related_repos_relpath,
                                     svn_revnum_t related_peg_rev,
                                     apr_pool_t *result_pool,
                                     apr_pool_t *scratch_pool)
{
  svn_ra_session_t *ra_session;
  const char *url;
  const char *corrected_url;
  apr_array_header_t *paths;
  apr_array_header_t *revprops;
  const char *repos_root_url;
  const char *repos_uuid;
  struct find_deleted_rev_baton b;
  svn_error_t *err;

  SVN_ERR(svn_client_conflict_get_repos_info(&repos_root_url, &repos_uuid,
                                             conflict, scratch_pool,
                                             scratch_pool));
  url = svn_path_url_add_component2(repos_root_url, parent_repos_relpath,
                                    scratch_pool);
  SVN_ERR(svn_client__open_ra_session_internal(&ra_session, &corrected_url,
                                               url, NULL, NULL, FALSE, FALSE,
                                               conflict->ctx, scratch_pool,
                                               scratch_pool));

  paths = apr_array_make(scratch_pool, 1, sizeof(const char *));
  APR_ARRAY_PUSH(paths, const char *) = "";

  revprops = apr_array_make(scratch_pool, 1, sizeof(const char *));
  APR_ARRAY_PUSH(revprops, const char *) = SVN_PROP_REVISION_AUTHOR;

  b.deleted_repos_relpath = svn_relpath_join(parent_repos_relpath,
                                             deleted_basename, scratch_pool);
  b.related_repos_relpath = related_repos_relpath;
  b.related_repos_peg_rev = related_peg_rev;
  b.deleted_rev = SVN_INVALID_REVNUM;
  b.replacing_node_kind = svn_node_unknown;
  b.repos_root_url = repos_root_url;
  b.repos_uuid = repos_uuid;
  b.ctx = conflict->ctx;
  b.moves_table = apr_hash_make(result_pool);
  b.moved_paths = apr_hash_make(scratch_pool);
  b.result_pool = result_pool;

  err = svn_ra_get_log2(ra_session, paths, start_rev, end_rev,
                        0, /* no limit */
                        TRUE, /* need the changed paths list */
                        FALSE, /* need to traverse copies */
                        FALSE, /* no need for merged revisions */
                        revprops,
                        find_deleted_rev, &b,
                        scratch_pool);
  if (err)
    {
      if (err->apr_err == SVN_ERR_CANCELLED &&
          b.deleted_rev != SVN_INVALID_REVNUM)
        {
          /* Log operation was aborted because we found deleted rev. */
          svn_error_clear(err);
        }
      else
        return svn_error_trace(err);
    }

  if (b.deleted_rev == SVN_INVALID_REVNUM)
    {
      /* We could not determine the revision in which the node was
       * deleted. */
      *deleted_rev = SVN_INVALID_REVNUM;
      *deleted_rev_author = NULL;
      *replacing_node_kind = svn_node_unknown;
      *move = NULL;
      return SVN_NO_ERROR;
    }
  else
    {
      apr_array_header_t *moves;

      *deleted_rev = b.deleted_rev;
      *deleted_rev_author = b.deleted_rev_author;
      *replacing_node_kind = b.replacing_node_kind;

      /* Look for a move which affects the deleted node. */
      moves = apr_hash_get(b.moves_table, &b.deleted_rev,
                           sizeof(svn_revnum_t));
      if (moves)
        {
          int i;

          for (i = 0; i < moves->nelts; i++)
            {
              struct repos_move_info *this_move;
              
              this_move = APR_ARRAY_IDX(moves, i, struct repos_move_info *);
              if (strcmp(b.deleted_repos_relpath,
                         this_move->moved_from_repos_relpath) == 0)
                {
                  /* Because b->moves_table lives in result_pool
                   * there is no need to deep-copy here. */
                  *move = this_move;
                  break;
                }
            }
        }
      else
        *move = NULL;
    }

  return SVN_NO_ERROR;
}

/* Details for tree conflicts involving a locally missing node. */
struct conflict_tree_local_missing_details
{
  /* If not SVN_INVALID_REVNUM, the node was deleted in DELETED_REV. */
  svn_revnum_t deleted_rev;

  /* Author who committed DELETED_REV. */
  const char *deleted_rev_author;

  /* Move information. If not NULL, the first move happened in DELETED_REV.
   * Follow MOVE->NEXT for subsequent moves in later revisions. */
  struct repos_move_info *move;
};

/* Implements tree_conflict_get_details_func_t. */
static svn_error_t *
conflict_tree_get_details_local_missing(svn_client_conflict_t *conflict,
                                        apr_pool_t *scratch_pool)
{
  const char *old_repos_relpath;
  const char *new_repos_relpath;
  const char *parent_repos_relpath;
  svn_revnum_t old_rev;
  svn_revnum_t new_rev;
  svn_revnum_t deleted_rev;
  const char *deleted_rev_author;
  svn_node_kind_t replacing_node_kind;
  const char *deleted_basename;
  struct conflict_tree_local_missing_details *details;
  struct repos_move_info *move;

  /* We only handle merges here. */
  if (svn_client_conflict_get_operation(conflict) != svn_wc_operation_merge)
    return SVN_NO_ERROR;

  SVN_ERR(svn_client_conflict_get_incoming_old_repos_location(
            &old_repos_relpath, &old_rev, NULL, conflict,
            scratch_pool, scratch_pool));
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &new_repos_relpath, &new_rev, NULL, conflict,
            scratch_pool, scratch_pool));

  /* A deletion of the node may have happened on the branch we
   * merged to. Scan the conflict victim's parent's log to find
   * a revision which deleted the node. */
  deleted_basename = svn_dirent_basename(conflict->local_abspath,
                                         scratch_pool);
  SVN_ERR(svn_wc__node_get_repos_info(NULL, &parent_repos_relpath,
                                      NULL, NULL,
                                      conflict->ctx->wc_ctx,
                                      svn_dirent_dirname(
                                        conflict->local_abspath,
                                        scratch_pool),
                                      scratch_pool,
                                      scratch_pool));
  SVN_ERR(find_revision_for_suspected_deletion(
            &deleted_rev, &deleted_rev_author, &replacing_node_kind, &move,
            conflict, deleted_basename, parent_repos_relpath,
            old_rev < new_rev ? new_rev : old_rev, 0,
            old_rev < new_rev ? new_repos_relpath : old_repos_relpath,
            old_rev < new_rev ? new_rev : old_rev,
            conflict->pool, scratch_pool));

  if (deleted_rev == SVN_INVALID_REVNUM)
    return SVN_NO_ERROR;

  details = apr_pcalloc(conflict->pool, sizeof(*details));
  details->deleted_rev = deleted_rev;
  details->deleted_rev_author = deleted_rev_author;
  details->move = move;
                                         
  conflict->tree_conflict_local_details = details;

  return SVN_NO_ERROR;
}

/* Return a localised string representation of the local part of a tree
   conflict on a non-existent node. */
static svn_error_t *
describe_local_none_node_change(const char **description,
                                svn_client_conflict_t *conflict,
                                apr_pool_t *result_pool,
                                apr_pool_t *scratch_pool)
{
  svn_wc_conflict_reason_t local_change;
  svn_wc_operation_t operation;

  local_change = svn_client_conflict_get_local_change(conflict);
  operation = svn_client_conflict_get_operation(conflict);

  switch (local_change)
    {
    case svn_wc_conflict_reason_edited:
      *description = _("An item containing uncommitted changes was "
                       "found in the working copy.");
      break;
    case svn_wc_conflict_reason_obstructed:
      *description = _("An item which already occupies this path was found in "
                       "the working copy.");
      break;
    case svn_wc_conflict_reason_deleted:
      *description = _("A deleted item was found in the working copy.");
      break;
    case svn_wc_conflict_reason_missing:
      if (operation == svn_wc_operation_update ||
          operation == svn_wc_operation_switch)
        *description = _("No such file or directory was found in the "
                         "working copy.");
      else if (operation == svn_wc_operation_merge)
        {
          /* ### display deleted revision */
          *description = _("No such file or directory was found in the "
                           "merge target working copy.\nThe item may "
                           "have been deleted or moved away in the "
                           "repository's history.");
        }
      break;
    case svn_wc_conflict_reason_unversioned:
      *description = _("An unversioned item was found in the working "
                       "copy.");
      break;
    case svn_wc_conflict_reason_added:
    case svn_wc_conflict_reason_replaced:
      *description = _("An item scheduled to be added to the repository "
                       "in the next commit was found in the working "
                       "copy.");
      break;
    case svn_wc_conflict_reason_moved_away:
      *description = _("The item in the working copy had been moved "
                       "away at the time this conflict was recorded.");
      break;
    case svn_wc_conflict_reason_moved_here:
      *description = _("An item had been moved here in the working copy "
                       "at the time this conflict was recorded.");
      break;
    }

  return SVN_NO_ERROR;
}

/* Append a description of all moves in the MOVE chain to DESCRIPTION. */
static const char *
append_moved_to_chain_description(const char *description,
                                  struct repos_move_info *move,
                                  apr_pool_t *result_pool,
                                  apr_pool_t *scratch_pool)
{
  if (move == NULL)
    return description;

  while (move)
    {
      description = apr_psprintf(scratch_pool,
                                 _("%s\nAnd then moved away to '^/%s' by "
                                   "%s in r%ld."),
                                 description, move->moved_to_repos_relpath,
                                 move->rev_author, move->rev);
      move = move->next;
    }

  return apr_pstrdup(result_pool, description);
}

/* Implements tree_conflict_get_description_func_t. */
static svn_error_t *
conflict_tree_get_local_description_generic(const char **description,
                                            svn_client_conflict_t *conflict,
                                            apr_pool_t *result_pool,
                                            apr_pool_t *scratch_pool)
{
  svn_node_kind_t victim_node_kind;

  victim_node_kind = svn_client_conflict_tree_get_victim_node_kind(conflict);

  *description = NULL;

  switch (victim_node_kind)
    {
      case svn_node_file:
      case svn_node_symlink:
        SVN_ERR(describe_local_file_node_change(description, conflict,
                                                result_pool, scratch_pool));
        break;
      case svn_node_dir:
        SVN_ERR(describe_local_dir_node_change(description, conflict,
                                               result_pool, scratch_pool));
        break;
      case svn_node_none:
      case svn_node_unknown:
        SVN_ERR(describe_local_none_node_change(description, conflict,
                                                result_pool, scratch_pool));
        break;
    }

  return SVN_NO_ERROR;
}

/* Implements tree_conflict_get_description_func_t. */
static svn_error_t *
conflict_tree_get_description_local_missing(const char **description,
                                            svn_client_conflict_t *conflict,
                                            apr_pool_t *result_pool,
                                            apr_pool_t *scratch_pool)
{
  struct conflict_tree_local_missing_details *details;

  details = conflict->tree_conflict_local_details;
  if (details == NULL)
    return svn_error_trace(conflict_tree_get_local_description_generic(
                             description, conflict, result_pool, scratch_pool));

  if (details->move)
    {
      *description = apr_psprintf(
                       result_pool,
                       _("No such file or directory was found in the "
                         "merge target working copy.\nThe item was "
                         "moved away to '^/%s' in r%ld by %s."),
                       details->move->moved_to_repos_relpath,
                       details->move->rev, details->move->rev_author);
      *description = append_moved_to_chain_description(*description,
                                                       details->move->next,
                                                       result_pool,
                                                       scratch_pool);
    }
  else
    *description = apr_psprintf(
                     result_pool,
                     _("No such file or directory was found in the "
                       "merge target working copy.\nThe item was "
                       "deleted in r%ld by %s."),
                     details->deleted_rev, details->deleted_rev_author);

  return SVN_NO_ERROR;
}

/* Return a localised string representation of the incoming part of a
   conflict; NULL for non-localised odd cases. */
static const char *
describe_incoming_change(svn_node_kind_t kind, svn_wc_conflict_action_t action,
                         svn_wc_operation_t operation)
{
  switch (kind)
    {
      case svn_node_file:
      case svn_node_symlink:
        if (operation == svn_wc_operation_update)
          {
            switch (action)
              {
                case svn_wc_conflict_action_edit:
                  return _("An update operation tried to edit a file.");
                case svn_wc_conflict_action_add:
                  return _("An update operation tried to add a file.");
                case svn_wc_conflict_action_delete:
                  return _("An update operation tried to delete or move "
                           "a file.");
                case svn_wc_conflict_action_replace:
                  return _("An update operation tried to replace a file.");
              }
          }
        else if (operation == svn_wc_operation_switch)
          {
            switch (action)
              {
                case svn_wc_conflict_action_edit:
                  return _("A switch operation tried to edit a file.");
                case svn_wc_conflict_action_add:
                  return _("A switch operation tried to add a file.");
                case svn_wc_conflict_action_delete:
                  return _("A switch operation tried to delete or move "
                           "a file.");
                case svn_wc_conflict_action_replace:
                  return _("A switch operation tried to replace a file.");
              }
          }
        else if (operation == svn_wc_operation_merge)
          {
            switch (action)
              {
                case svn_wc_conflict_action_edit:
                  return _("A merge operation tried to edit a file.");
                case svn_wc_conflict_action_add:
                  return _("A merge operation tried to add a file.");
                case svn_wc_conflict_action_delete:
                  return _("A merge operation tried to delete or move "
                           "a file.");
                case svn_wc_conflict_action_replace:
                  return _("A merge operation tried to replace a file.");
            }
          }
        break;
      case svn_node_dir:
        if (operation == svn_wc_operation_update)
          {
            switch (action)
              {
                case svn_wc_conflict_action_edit:
                  return _("An update operation tried to change a directory.");
                case svn_wc_conflict_action_add:
                  return _("An update operation tried to add a directory.");
                case svn_wc_conflict_action_delete:
                  return _("An update operation tried to delete or move "
                           "a directory.");
                case svn_wc_conflict_action_replace:
                  return _("An update operation tried to replace a directory.");
              }
          }
        else if (operation == svn_wc_operation_switch)
          {
            switch (action)
              {
                case svn_wc_conflict_action_edit:
                  return _("A switch operation tried to edit a directory.");
                case svn_wc_conflict_action_add:
                  return _("A switch operation tried to add a directory.");
                case svn_wc_conflict_action_delete:
                  return _("A switch operation tried to delete or move "
                           "a directory.");
                case svn_wc_conflict_action_replace:
                  return _("A switch operation tried to replace a directory.");
              }
          }
        else if (operation == svn_wc_operation_merge)
          {
            switch (action)
              {
                case svn_wc_conflict_action_edit:
                  return _("A merge operation tried to edit a directory.");
                case svn_wc_conflict_action_add:
                  return _("A merge operation tried to add a directory.");
                case svn_wc_conflict_action_delete:
                  return _("A merge operation tried to delete or move "
                           "a directory.");
                case svn_wc_conflict_action_replace:
                  return _("A merge operation tried to replace a directory.");
            }
          }
        break;
      case svn_node_none:
      case svn_node_unknown:
        if (operation == svn_wc_operation_update)
          {
            switch (action)
              {
                case svn_wc_conflict_action_edit:
                  return _("An update operation tried to edit an item.");
                case svn_wc_conflict_action_add:
                  return _("An update operation tried to add an item.");
                case svn_wc_conflict_action_delete:
                  return _("An update operation tried to delete or move "
                           "an item.");
                case svn_wc_conflict_action_replace:
                  return _("An update operation tried to replace an item.");
              }
          }
        else if (operation == svn_wc_operation_switch)
          {
            switch (action)
              {
                case svn_wc_conflict_action_edit:
                  return _("A switch operation tried to edit an item.");
                case svn_wc_conflict_action_add:
                  return _("A switch operation tried to add an item.");
                case svn_wc_conflict_action_delete:
                  return _("A switch operation tried to delete or move "
                           "an item.");
                case svn_wc_conflict_action_replace:
                  return _("A switch operation tried to replace an item.");
              }
          }
        else if (operation == svn_wc_operation_merge)
          {
            switch (action)
              {
                case svn_wc_conflict_action_edit:
                  return _("A merge operation tried to edit an item.");
                case svn_wc_conflict_action_add:
                  return _("A merge operation tried to add an item.");
                case svn_wc_conflict_action_delete:
                  return _("A merge operation tried to delete or move "
                           "an item.");
                case svn_wc_conflict_action_replace:
                  return _("A merge operation tried to replace an item.");
              }
          }
        break;
    }

  return NULL;
}

/* Return a localised string representation of the operation part of a
   conflict. */
static const char *
operation_str(svn_wc_operation_t operation)
{
  switch (operation)
    {
    case svn_wc_operation_update: return _("upon update");
    case svn_wc_operation_switch: return _("upon switch");
    case svn_wc_operation_merge:  return _("upon merge");
    case svn_wc_operation_none:   return _("upon none");
    }
  SVN_ERR_MALFUNCTION_NO_RETURN();
  return NULL;
}

svn_error_t *
svn_client_conflict_prop_get_description(const char **description,
                                         svn_client_conflict_t *conflict,
                                         apr_pool_t *result_pool,
                                         apr_pool_t *scratch_pool)
{
  const char *reason_str, *action_str;

  /* We provide separately translatable strings for the values that we
   * know about, and a fall-back in case any other values occur. */
  switch (svn_client_conflict_get_local_change(conflict))
    {
      case svn_wc_conflict_reason_edited:
        reason_str = _("local edit");
        break;
      case svn_wc_conflict_reason_added:
        reason_str = _("local add");
        break;
      case svn_wc_conflict_reason_deleted:
        reason_str = _("local delete");
        break;
      case svn_wc_conflict_reason_obstructed:
        reason_str = _("local obstruction");
        break;
      default:
        reason_str = apr_psprintf(
                       scratch_pool, _("local %s"),
                       svn_token__to_word(
                         map_conflict_reason,
                         svn_client_conflict_get_local_change(conflict)));
        break;
    }
  switch (svn_client_conflict_get_incoming_change(conflict))
    {
      case svn_wc_conflict_action_edit:
        action_str = _("incoming edit");
        break;
      case svn_wc_conflict_action_add:
        action_str = _("incoming add");
        break;
      case svn_wc_conflict_action_delete:
        action_str = _("incoming delete");
        break;
      default:
        action_str = apr_psprintf(
                       scratch_pool, _("incoming %s"),
                       svn_token__to_word(
                         map_conflict_action,
                         svn_client_conflict_get_incoming_change(conflict)));
        break;
    }
  SVN_ERR_ASSERT(reason_str && action_str);

  *description = apr_psprintf(result_pool, _("%s, %s %s"),
                              reason_str, action_str,
                              operation_str(
                                svn_client_conflict_get_operation(conflict)));

  return SVN_NO_ERROR;
}

/* Implements tree_conflict_get_description_func_t. */
static svn_error_t *
conflict_tree_get_incoming_description_generic(
  const char **incoming_change_description,
  svn_client_conflict_t *conflict,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool)
{
  const char *action;
  svn_node_kind_t incoming_kind;
  svn_wc_conflict_action_t conflict_action;
  svn_wc_operation_t conflict_operation;
  svn_node_kind_t conflict_node_kind;

  conflict_action = svn_client_conflict_get_incoming_change(conflict);
  conflict_operation = svn_client_conflict_get_operation(conflict);
  conflict_node_kind = svn_client_conflict_tree_get_victim_node_kind(conflict);

  /* Determine the node kind of the incoming change. */
  incoming_kind = svn_node_unknown;
  if (conflict_action == svn_wc_conflict_action_edit ||
      conflict_action == svn_wc_conflict_action_delete)
    {
      /* Change is acting on 'src_left' version of the node. */
      SVN_ERR(svn_client_conflict_get_incoming_old_repos_location(
                NULL, NULL, &incoming_kind, conflict, scratch_pool,
                scratch_pool));
    }
  else if (conflict_action == svn_wc_conflict_action_add ||
           conflict_action == svn_wc_conflict_action_replace)
    {
      /* Change is acting on 'src_right' version of the node.
       *
       * ### For 'replace', the node kind is ambiguous. However, src_left
       * ### is NULL for replace, so we must use src_right. */
      SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
                NULL, NULL, &incoming_kind, conflict, scratch_pool,
                scratch_pool));
    }

  action = describe_incoming_change(incoming_kind, conflict_action,
                                    conflict_operation);
  if (action)
    {
      *incoming_change_description = apr_pstrdup(result_pool, action);
    }
  else
    {
      /* A catch-all message for very rare or nominally impossible cases.
         It will not be pretty, but is closer to an internal error than
         an ordinary user-facing string. */
      *incoming_change_description = apr_psprintf(result_pool,
                                       _("incoming %s %s"),
                                       svn_node_kind_to_word(incoming_kind),
                                       svn_token__to_word(map_conflict_action,
                                                          conflict_action));
    }
  return SVN_NO_ERROR;
}

/* Details for tree conflicts involving incoming deletions and replacements. */
struct conflict_tree_incoming_delete_details
{
  /* If not SVN_INVALID_REVNUM, the node was deleted in DELETED_REV. */
  svn_revnum_t deleted_rev;

  /* If not SVN_INVALID_REVNUM, the node was added in ADDED_REV. The incoming
   * delete is the result of a reverse application of this addition. */
  svn_revnum_t added_rev;

  /* The path which was deleted/added relative to the repository root. */
  const char *repos_relpath;

  /* Author who committed DELETED_REV/ADDED_REV. */
  const char *rev_author;

  /* New node kind for a replaced node. This is svn_node_none for deletions. */
  svn_node_kind_t replacing_node_kind;

  /* Move information. If not NULL, the first move happened in DELETED_REV
   * or in ADDED_REV (in which case moves should be interpreted in reverse).
   * Follow MOVE->NEXT for subsequent moves in later revisions. */
  struct repos_move_info *move;
};

static const char *
describe_incoming_deletion_upon_update(
  struct conflict_tree_incoming_delete_details *details,
  svn_node_kind_t victim_node_kind,
  svn_revnum_t old_rev,
  svn_revnum_t new_rev,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool)
{
  if (details->replacing_node_kind == svn_node_file ||
      details->replacing_node_kind == svn_node_symlink)
    {
      if (victim_node_kind == svn_node_dir)
        return apr_psprintf(result_pool,
                            _("Directory updated from r%ld to r%ld was "
                              "replaced with a file by %s in r%ld."),
                            old_rev, new_rev,
                            details->rev_author, details->deleted_rev);
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        return apr_psprintf(result_pool,
                            _("File updated from r%ld to r%ld was replaced "
                              "with a file from another line of history by "
                              "%s in r%ld."),
                            old_rev, new_rev,
                            details->rev_author, details->deleted_rev);
      else
        return apr_psprintf(result_pool,
                            _("Item updated from r%ld to r%ld was replaced "
                              "with a file by %s in r%ld."), old_rev, new_rev,
                            details->rev_author, details->deleted_rev);
    }
  else if (details->replacing_node_kind == svn_node_dir)
    {
      if (victim_node_kind == svn_node_dir)
        return apr_psprintf(result_pool,
                            _("Directory updated from r%ld to r%ld was "
                              "replaced with a directory from another line "
                              "of history by %s in r%ld."),
                            old_rev, new_rev,
                            details->rev_author, details->deleted_rev);
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        return apr_psprintf(result_pool,
                            _("Directory updated from r%ld to r%ld was "
                              "replaced with a file by %s in r%ld."),
                            old_rev, new_rev,
                            details->rev_author, details->deleted_rev);
      else
        return apr_psprintf(result_pool,
                            _("Item updated from r%ld to r%ld was replaced "
                              "by %s in r%ld."), old_rev, new_rev,
                            details->rev_author, details->deleted_rev);
    }
  else
    {
      if (victim_node_kind == svn_node_dir)
        {
          if (details->move)
            {
              const char *description =
                apr_psprintf(result_pool,
                             _("Directory updated from r%ld to r%ld was "
                               "moved to '^/%s' by %s in r%ld."),
                             old_rev, new_rev,
                             details->move->moved_to_repos_relpath,
                             details->rev_author, details->deleted_rev);
              return append_moved_to_chain_description(description,
                                                       details->move->next,
                                                       result_pool,
                                                       scratch_pool);
            }
          else
            return apr_psprintf(result_pool,
                                _("Directory updated from r%ld to r%ld was "
                                  "deleted by %s in r%ld."),
                                old_rev, new_rev,
                                details->rev_author, details->deleted_rev);
        }
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        {
          if (details->move)
            {
              const char *description =
                apr_psprintf(result_pool,
                             _("File updated from r%ld to r%ld was moved "
                               "to '^/%s' by %s in r%ld."), old_rev, new_rev,
                             details->move->moved_to_repos_relpath,
                             details->rev_author, details->deleted_rev);
              return append_moved_to_chain_description(description,
                                                       details->move->next,
                                                       result_pool,
                                                       scratch_pool);
            }
          else
            return apr_psprintf(result_pool,
                                _("File updated from r%ld to r%ld was "
                                  "deleted by %s in r%ld."), old_rev, new_rev,
                                details->rev_author, details->deleted_rev);
        }
      else
        {
          if (details->move)
            {
              const char *description = 
                apr_psprintf(result_pool,
                             _("Item updated from r%ld to r%ld was moved "
                               "to '^/%s' by %s in r%ld."), old_rev, new_rev,
                             details->move->moved_to_repos_relpath,
                             details->rev_author, details->deleted_rev);
              return append_moved_to_chain_description(description,
                                                       details->move->next,
                                                       result_pool,
                                                       scratch_pool);
            }
          else
            return apr_psprintf(result_pool,
                                _("Item updated from r%ld to r%ld was "
                                  "deleted by %s in r%ld."), old_rev, new_rev,
                                details->rev_author, details->deleted_rev);
        }
    }
}

static const char *
describe_incoming_reverse_addition_upon_update(
  struct conflict_tree_incoming_delete_details *details,
  svn_node_kind_t victim_node_kind,
  svn_revnum_t old_rev,
  svn_revnum_t new_rev,
  apr_pool_t *result_pool)
{
  if (details->replacing_node_kind == svn_node_file ||
      details->replacing_node_kind == svn_node_symlink)
    {
      if (victim_node_kind == svn_node_dir)
        return apr_psprintf(result_pool,
                            _("Directory updated backwards from r%ld to r%ld "
                              "was a file before the replacement made by %s "
                              "in r%ld."), old_rev, new_rev,
                            details->rev_author, details->added_rev);
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        return apr_psprintf(result_pool,
                            _("File updated backwards from r%ld to r%ld was a "
                              "file from another line of history before the "
                              "replacement made by %s in r%ld."),
                            old_rev, new_rev,
                            details->rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("Item updated backwards from r%ld to r%ld was "
                              "replaced with a file by %s in r%ld."),
                            old_rev, new_rev,
                            details->rev_author, details->added_rev);
    }
  else if (details->replacing_node_kind == svn_node_dir)
    {
      if (victim_node_kind == svn_node_dir)
        return apr_psprintf(result_pool,
                            _("Directory updated backwards from r%ld to r%ld "
                              "was a directory from another line of history "
                              "before the replacement made by %s in "
                              "r%ld."), old_rev, new_rev,
                            details->rev_author, details->added_rev);
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        return apr_psprintf(result_pool,
                            _("File updated backwards from r%ld to r%ld was a "
                              "directory before the replacement made by %s "
                              "in r%ld."), old_rev, new_rev,
                            details->rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("Item updated backwards from r%ld to r%ld was "
                              "replaced with a directory by %s in r%ld."),
                            old_rev, new_rev,
                            details->rev_author, details->added_rev);
    }
  else
    {
      if (victim_node_kind == svn_node_dir)
        return apr_psprintf(result_pool,
                            _("Directory updated backwards from r%ld to r%ld "
                              "did not exist before it was added by %s in "
                              "r%ld."), old_rev, new_rev,
                            details->rev_author, details->added_rev);
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        return apr_psprintf(result_pool,
                            _("File updated backwards from r%ld to r%ld did "
                              "not exist before it was added by %s in r%ld."),
                            old_rev, new_rev,
                            details->rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("Item updated backwards from r%ld to r%ld did "
                              "not exist before it was added by %s in r%ld."),
                            old_rev, new_rev,
                            details->rev_author, details->added_rev);
    }
}

static const char *
describe_incoming_deletion_upon_switch(
  struct conflict_tree_incoming_delete_details *details,
  svn_node_kind_t victim_node_kind,
  const char *old_repos_relpath,
  svn_revnum_t old_rev,
  const char *new_repos_relpath,
  svn_revnum_t new_rev,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool)
{
  if (details->replacing_node_kind == svn_node_file ||
      details->replacing_node_kind == svn_node_symlink)
    {
      if (victim_node_kind == svn_node_dir)
        return apr_psprintf(result_pool,
                            _("Directory switched from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\n"
                              "was replaced with a file by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->deleted_rev);
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        return apr_psprintf(result_pool,
                            _("File switched from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                              "replaced with a file from another line of "
                              "history by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->deleted_rev);
      else
        return apr_psprintf(result_pool,
                            _("Item switched from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                              "replaced with a file by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->deleted_rev);
    }
  else if (details->replacing_node_kind == svn_node_dir)
    {
      if (victim_node_kind == svn_node_dir)
        return apr_psprintf(result_pool,
                            _("Directory switched from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\n"
                              "was replaced with a directory from another "
                              "line of history by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->deleted_rev);
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        return apr_psprintf(result_pool,
                            _("File switched from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\n"
                              "was replaced with a directory by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->deleted_rev);
      else
        return apr_psprintf(result_pool,
                            _("Item switched from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                              "replaced with a directory by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->deleted_rev);
    }
  else
    {
      if (victim_node_kind == svn_node_dir)
        {
          if (details->move)
            {
              const char *description =
                apr_psprintf(result_pool,
                             _("Directory switched from\n"
                               "'^/%s@%ld'\nto\n'^/%s@%ld'\n"
                               "was moved to '^/%s' by %s in r%ld."),
                             old_repos_relpath, old_rev,
                             new_repos_relpath, new_rev,
                             details->move->moved_to_repos_relpath,
                             details->rev_author, details->deleted_rev);
              return append_moved_to_chain_description(description,
                                                       details->move->next,
                                                       result_pool,
                                                       scratch_pool);
            }
          else
            return apr_psprintf(result_pool,
                                _("Directory switched from\n"
                                  "'^/%s@%ld'\nto\n'^/%s@%ld'\n"
                                  "was deleted by %s in r%ld."),
                                old_repos_relpath, old_rev,
                                new_repos_relpath, new_rev,
                                details->rev_author, details->deleted_rev);
        }
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        {
          if (details->move)
            {
              const char *description =
                apr_psprintf(result_pool,
                             _("File switched from\n"
                               "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                               "moved to '^/%s' by %s in r%ld."),
                             old_repos_relpath, old_rev,
                             new_repos_relpath, new_rev,
                             details->move->moved_to_repos_relpath,
                             details->rev_author, details->deleted_rev);
              return append_moved_to_chain_description(description,
                                                       details->move->next,
                                                       result_pool,
                                                       scratch_pool);
            }
          else
            return apr_psprintf(result_pool,
                                _("File switched from\n"
                                  "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                                  "deleted by %s in r%ld."),
                                old_repos_relpath, old_rev,
                                new_repos_relpath, new_rev,
                                details->rev_author, details->deleted_rev);
        }
      else
        {
          if (details->move)
            {
              const char *description =
                apr_psprintf(result_pool,
                             _("Item switched from\n"
                               "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                               "moved to '^/%s' by %s in r%ld."),
                             old_repos_relpath, old_rev,
                             new_repos_relpath, new_rev,
                             details->move->moved_to_repos_relpath,
                             details->rev_author, details->deleted_rev);
              return append_moved_to_chain_description(description,
                                                       details->move->next,
                                                       result_pool,
                                                       scratch_pool);
            }
          else
            return apr_psprintf(result_pool,
                                _("Item switched from\n"
                                  "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                                  "deleted by %s in r%ld."),
                                old_repos_relpath, old_rev,
                                new_repos_relpath, new_rev,
                                details->rev_author, details->deleted_rev);
        }
    }
}

static const char *
describe_incoming_reverse_addition_upon_switch(
  struct conflict_tree_incoming_delete_details *details,
  svn_node_kind_t victim_node_kind,
  const char *old_repos_relpath,
  svn_revnum_t old_rev,
  const char *new_repos_relpath,
  svn_revnum_t new_rev,
  apr_pool_t *result_pool)
{
  if (details->replacing_node_kind == svn_node_file ||
      details->replacing_node_kind == svn_node_symlink)
    {
      if (victim_node_kind == svn_node_dir)
        return apr_psprintf(result_pool,
                            _("Directory switched from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\n"
                              "was a file before the replacement made by %s "
                              "in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        return apr_psprintf(result_pool,
                            _("File switched from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas a "
                              "file from another line of history before the "
                              "replacement made by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("Item switched from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                              "replaced with a file by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
    }
  else if (details->replacing_node_kind == svn_node_dir)
    {
      if (victim_node_kind == svn_node_dir)
        return apr_psprintf(result_pool,
                            _("Directory switched from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\n"
                              "was a directory from another line of history "
                              "before the replacement made by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        return apr_psprintf(result_pool,
                            _("Directory switched from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\n"
                              "was a file before the replacement made by %s "
                              "in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("Item switched from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                              "replaced with a directory by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
    }
  else
    {
      if (victim_node_kind == svn_node_dir)
        return apr_psprintf(result_pool,
                            _("Directory switched from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\n"
                              "did not exist before it was added by %s in "
                              "r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        return apr_psprintf(result_pool,
                            _("File switched from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\ndid "
                              "not exist before it was added by %s in "
                              "r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("Item switched from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\ndid "
                              "not exist before it was added by %s in "
                              "r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
    }
}

static const char *
describe_incoming_deletion_upon_merge(
  struct conflict_tree_incoming_delete_details *details,
  svn_node_kind_t victim_node_kind,
  const char *old_repos_relpath,
  svn_revnum_t old_rev,
  const char *new_repos_relpath,
  svn_revnum_t new_rev,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool)
{
  if (details->replacing_node_kind == svn_node_file ||
      details->replacing_node_kind == svn_node_symlink)
    {
      if (victim_node_kind == svn_node_dir)
        return apr_psprintf(result_pool,
                            _("Directory merged from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\n"
                              "was replaced with a file by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->deleted_rev);
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        return apr_psprintf(result_pool,
                            _("File merged from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                              "replaced with a file from another line of "
                              "history by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->deleted_rev);
      else
        return apr_psprintf(result_pool,
                            _("Item merged from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                              "replaced with a file by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->deleted_rev);
    }
  else if (details->replacing_node_kind == svn_node_dir)
    {
      if (victim_node_kind == svn_node_dir)
        return apr_psprintf(result_pool,
                            _("Directory merged from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\n"
                              "was replaced with a directory from another "
                              "line of history by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->deleted_rev);
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        return apr_psprintf(result_pool,
                            _("File merged from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\n"
                              "was replaced with a directory by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->deleted_rev);
      else
        return apr_psprintf(result_pool,
                            _("Item merged from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                              "replaced with a directory by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->deleted_rev);
    }
  else
    {
      if (victim_node_kind == svn_node_dir)
        {
          if (details->move)
            {
              const char *description =
                apr_psprintf(result_pool,
                             _("Directory merged from\n"
                               "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                               "moved to '^/%s' by %s in r%ld."),
                             old_repos_relpath, old_rev,
                             new_repos_relpath, new_rev,
                             details->move->moved_to_repos_relpath,
                             details->rev_author, details->deleted_rev);
              return append_moved_to_chain_description(description,
                                                       details->move->next,
                                                       result_pool,
                                                       scratch_pool);
            }
          else
            return apr_psprintf(result_pool,
                                _("Directory merged from\n"
                                  "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                                  "deleted by %s in r%ld."),
                                old_repos_relpath, old_rev,
                                new_repos_relpath, new_rev,
                                details->rev_author, details->deleted_rev);
        }
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        {
          if (details->move)
            {
              const char *description =
                apr_psprintf(result_pool,
                             _("File merged from\n"
                               "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                               "moved to '^/%s' by %s in r%ld."),
                             old_repos_relpath, old_rev,
                             new_repos_relpath, new_rev,
                             details->move->moved_to_repos_relpath,
                             details->rev_author, details->deleted_rev);
              return append_moved_to_chain_description(description,
                                                       details->move->next,
                                                       result_pool,
                                                       scratch_pool);
            }
          else
            return apr_psprintf(result_pool,
                                _("File merged from\n"
                                  "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                                  "deleted by %s in r%ld."),
                                old_repos_relpath, old_rev,
                                new_repos_relpath, new_rev,
                                details->rev_author, details->deleted_rev);
        }
      else
        {
          if (details->move)
            {
              const char *description =
                apr_psprintf(result_pool,
                             _("Item merged from\n"
                               "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                               "moved to '^/%s' by %s in r%ld."),
                             old_repos_relpath, old_rev,
                             new_repos_relpath, new_rev,
                             details->move->moved_to_repos_relpath,
                             details->rev_author, details->deleted_rev);
              return append_moved_to_chain_description(description,
                                                       details->move->next,
                                                       result_pool,
                                                       scratch_pool);
            }
          else
            return apr_psprintf(result_pool,
                                _("Item merged from\n"
                                  "'^/%s@%ld'\nto\n'^/%s@%ld'\nwas "
                                  "deleted by %s in r%ld."),
                                old_repos_relpath, old_rev,
                                new_repos_relpath, new_rev,
                                details->rev_author, details->deleted_rev);
        }
    }
}

static const char *
describe_incoming_reverse_addition_upon_merge(
  struct conflict_tree_incoming_delete_details *details,
  svn_node_kind_t victim_node_kind,
  const char *old_repos_relpath,
  svn_revnum_t old_rev,
  const char *new_repos_relpath,
  svn_revnum_t new_rev,
  apr_pool_t *result_pool)
{
  if (details->replacing_node_kind == svn_node_file ||
      details->replacing_node_kind == svn_node_symlink)
    {
      if (victim_node_kind == svn_node_dir)
        return apr_psprintf(result_pool,
                            _("Directory reverse-merged from\n'^/%s@%ld'\nto "
                              "^/%s@%ld was a file before the replacement "
                              "made by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        return apr_psprintf(result_pool,
                            _("File reverse-merged from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\n"
                              "was a file from another line of history before "
                              "the replacement made by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("Item reverse-merged from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\n"
                              "was replaced with a file by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
    }
  else if (details->replacing_node_kind == svn_node_dir)
    {
      if (victim_node_kind == svn_node_dir)
        return apr_psprintf(result_pool,
                            _("Directory reverse-merged from\n'^/%s@%ld'\nto "
                              "^/%s@%ld was a directory from another line "
                              "of history before the replacement made by %s "
                              "in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        return apr_psprintf(result_pool,
                            _("Directory reverse-merged from\n'^/%s@%ld'\nto "
                              "^/%s@%ld was a file before the replacement "
                              "made by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("Item reverse-merged from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\n"
                              "was replaced with a directory by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
    }
  else
    {
      if (victim_node_kind == svn_node_dir)
        return apr_psprintf(result_pool,
                            _("Directory reverse-merged from\n'^/%s@%ld'\nto "
                              "^/%s@%ld did not exist before it was added "
                              "by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
      else if (victim_node_kind == svn_node_file ||
               victim_node_kind == svn_node_symlink)
        return apr_psprintf(result_pool,
                            _("File reverse-merged from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\n"
                              "did not exist before it was added by %s in "
                              "r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("Item reverse-merged from\n"
                              "'^/%s@%ld'\nto\n'^/%s@%ld'\n"
                              "did not exist before it was added by %s in "
                              "r%ld."),
                            old_repos_relpath, old_rev,
                            new_repos_relpath, new_rev,
                            details->rev_author, details->added_rev);
    }
}

/* Implements tree_conflict_get_description_func_t. */
static svn_error_t *
conflict_tree_get_description_incoming_delete(
  const char **incoming_change_description,
  svn_client_conflict_t *conflict,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool)
{
  const char *action;
  svn_node_kind_t victim_node_kind;
  svn_wc_operation_t conflict_operation;
  const char *old_repos_relpath;
  svn_revnum_t old_rev;
  const char *new_repos_relpath;
  svn_revnum_t new_rev;
  struct conflict_tree_incoming_delete_details *details;

  if (conflict->tree_conflict_incoming_details == NULL)
    return svn_error_trace(conflict_tree_get_incoming_description_generic(
                             incoming_change_description,
                             conflict, result_pool, scratch_pool));

  conflict_operation = svn_client_conflict_get_operation(conflict);
  victim_node_kind = svn_client_conflict_tree_get_victim_node_kind(conflict);
  SVN_ERR(svn_client_conflict_get_incoming_old_repos_location(
            &old_repos_relpath, &old_rev, NULL, conflict, scratch_pool,
            scratch_pool));
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &new_repos_relpath, &new_rev, NULL, conflict, scratch_pool,
            scratch_pool));

  details = conflict->tree_conflict_incoming_details;

  if (conflict_operation == svn_wc_operation_update)
    {
      if (details->deleted_rev != SVN_INVALID_REVNUM)
        {
          action = describe_incoming_deletion_upon_update(details,
                                                          victim_node_kind,
                                                          old_rev,
                                                          new_rev,
                                                          result_pool,
                                                          scratch_pool);
        }
      else /* details->added_rev != SVN_INVALID_REVNUM */
        {
          /* This deletion is really the reverse change of an addition. */
          action = describe_incoming_reverse_addition_upon_update(
                     details, victim_node_kind, old_rev, new_rev, result_pool);
        }
    }
  else if (conflict_operation == svn_wc_operation_switch)
    {
      if (details->deleted_rev != SVN_INVALID_REVNUM)
        {
          action = describe_incoming_deletion_upon_switch(details,
                                                          victim_node_kind,
                                                          old_repos_relpath,
                                                          old_rev,
                                                          new_repos_relpath,
                                                          new_rev,
                                                          result_pool,
                                                          scratch_pool);
        }
      else /* details->added_rev != SVN_INVALID_REVNUM */
        {
          /* This deletion is really the reverse change of an addition. */
          action = describe_incoming_reverse_addition_upon_switch(
                     details, victim_node_kind, old_repos_relpath, old_rev,
                     new_repos_relpath, new_rev, result_pool);
            
        }
      }
  else if (conflict_operation == svn_wc_operation_merge)
    {
      if (details->deleted_rev != SVN_INVALID_REVNUM)
        {
          action = describe_incoming_deletion_upon_merge(details,
                                                         victim_node_kind,
                                                         old_repos_relpath,
                                                         old_rev,
                                                         new_repos_relpath,
                                                         new_rev,
                                                         result_pool,
                                                         scratch_pool);
        }
      else /* details->added_rev != SVN_INVALID_REVNUM */
        {
          /* This deletion is really the reverse change of an addition. */
          action = describe_incoming_reverse_addition_upon_merge(
                     details, victim_node_kind, old_repos_relpath, old_rev,
                     new_repos_relpath, new_rev, result_pool);
        }
      }

  *incoming_change_description = apr_pstrdup(result_pool, action);

  return SVN_NO_ERROR;
}

/* Baton for find_added_rev(). */
struct find_added_rev_baton
{
  svn_revnum_t added_rev;
  const char *repos_relpath;
  const char *parent_repos_relpath;
  apr_pool_t *pool;
};

/* Implements svn_location_segment_receiver_t.
 * Finds the revision in which a node was added by tracing 'start'
 * revisions in location segments reported for the node.
 * If the PARENT_REPOS_RELPATH in the baton is not NULL, only consider
 * segments in which the node existed somwhere beneath this path. */
static svn_error_t *
find_added_rev(svn_location_segment_t *segment,
               void *baton,
               apr_pool_t *scratch_pool)
{
  struct find_added_rev_baton *b = baton;

  if (segment->path) /* not interested in gaps */
    {
      if (b->parent_repos_relpath == NULL ||
          svn_relpath_skip_ancestor(b->parent_repos_relpath,
                                    segment->path) != NULL)
        {
          b->added_rev = segment->range_start;
          b->repos_relpath = apr_pstrdup(b->pool, segment->path);
        }
    }

  return SVN_NO_ERROR;
}

/* Find conflict details in the case where a revision which added a node was
 * applied in reverse, resulting in an incoming deletion. */
static svn_error_t *
get_incoming_delete_details_for_reverse_addition(
  struct conflict_tree_incoming_delete_details **details,
  const char *repos_root_url,
  const char *old_repos_relpath,
  svn_revnum_t old_rev,
  svn_revnum_t new_rev,
  svn_client_ctx_t *ctx,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool)
{
  svn_ra_session_t *ra_session;
  const char *url;
  const char *corrected_url;
  svn_string_t *author_revprop;
  struct find_added_rev_baton b;

  url = svn_path_url_add_component2(repos_root_url, old_repos_relpath,
                                    scratch_pool);
  SVN_ERR(svn_client__open_ra_session_internal(&ra_session,
                                               &corrected_url,
                                               url, NULL, NULL,
                                               FALSE,
                                               FALSE,
                                               ctx,
                                               scratch_pool,
                                               scratch_pool));

  *details = apr_pcalloc(result_pool, sizeof(**details));
  b.added_rev = SVN_INVALID_REVNUM;
  b.repos_relpath = NULL;
  b.parent_repos_relpath = NULL;
  b.pool = scratch_pool;
  /* Figure out when this node was added. */
  SVN_ERR(svn_ra_get_location_segments(ra_session, "", old_rev,
                                       old_rev, new_rev,
                                       find_added_rev, &b,
                                       scratch_pool));
  SVN_ERR(svn_ra_rev_prop(ra_session, b.added_rev,
                          SVN_PROP_REVISION_AUTHOR,
                          &author_revprop, scratch_pool));
  (*details)->deleted_rev = SVN_INVALID_REVNUM;
  (*details)->added_rev = b.added_rev;
  (*details)->repos_relpath = apr_pstrdup(result_pool, b.repos_relpath);
  (*details)->rev_author = apr_pstrdup(result_pool, author_revprop->data);

  /* Check for replacement. */
  (*details)->replacing_node_kind = svn_node_none;
  if ((*details)->added_rev > 0)
    {
      svn_node_kind_t replaced_node_kind;

      SVN_ERR(svn_ra_check_path(ra_session, "", (*details)->added_rev - 1,
                                &replaced_node_kind, scratch_pool));
      if (replaced_node_kind != svn_node_none)
        SVN_ERR(svn_ra_check_path(ra_session, "", (*details)->added_rev,
                                  &(*details)->replacing_node_kind,
                                  scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* Implements tree_conflict_get_details_func_t.
 * Find the revision in which the victim was deleted in the repository. */
static svn_error_t *
conflict_tree_get_details_incoming_delete(svn_client_conflict_t *conflict,
                                          apr_pool_t *scratch_pool)
{
  const char *old_repos_relpath;
  const char *new_repos_relpath;
  const char *repos_root_url;
  const char *repos_uuid;
  svn_revnum_t old_rev;
  svn_revnum_t new_rev;
  struct conflict_tree_incoming_delete_details *details;
  svn_wc_operation_t operation;

  SVN_ERR(svn_client_conflict_get_incoming_old_repos_location(
            &old_repos_relpath, &old_rev, NULL, conflict, scratch_pool,
            scratch_pool));
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &new_repos_relpath, &new_rev, NULL, conflict, scratch_pool,
            scratch_pool));
  SVN_ERR(svn_client_conflict_get_repos_info(&repos_root_url, &repos_uuid,
                                             conflict,
                                             scratch_pool, scratch_pool));
  operation = svn_client_conflict_get_operation(conflict);

  if (operation == svn_wc_operation_update)
    {
      if (old_rev < new_rev)
        {
          const char *parent_repos_relpath;
          svn_revnum_t deleted_rev;
          const char *deleted_rev_author;
          svn_node_kind_t replacing_node_kind;
          struct repos_move_info *move;

          /* The update operation went forward in history. */

          SVN_ERR(svn_wc__node_get_repos_info(NULL, &parent_repos_relpath,
                                              NULL, NULL,
                                              conflict->ctx->wc_ctx,
                                              svn_dirent_dirname(
                                                conflict->local_abspath,
                                                scratch_pool),
                                              scratch_pool,
                                              scratch_pool));
          SVN_ERR(find_revision_for_suspected_deletion(
                    &deleted_rev, &deleted_rev_author, &replacing_node_kind,
                    &move, conflict,
                    svn_dirent_basename(conflict->local_abspath, scratch_pool),
                    parent_repos_relpath, old_rev, new_rev,
                    NULL, SVN_INVALID_REVNUM, /* related to self */
                    conflict->pool, scratch_pool));
          if (deleted_rev == SVN_INVALID_REVNUM)
            {
              /* We could not determine the revision in which the node was
               * deleted. We cannot provide the required details so the best
               * we can do is fall back to the default description. */
              return SVN_NO_ERROR;
            }

          details = apr_pcalloc(conflict->pool, sizeof(*details));
          details->deleted_rev = deleted_rev;
          details->added_rev = SVN_INVALID_REVNUM;
          details->repos_relpath = apr_pstrdup(conflict->pool,
                                               new_repos_relpath);
          details->rev_author = deleted_rev_author;
          details->replacing_node_kind = replacing_node_kind;
          details->move = move;
        }
      else /* new_rev < old_rev */
        {
          /* The update operation went backwards in history.
           * Figure out when this node was added. */
          SVN_ERR(get_incoming_delete_details_for_reverse_addition(
                    &details, repos_root_url, old_repos_relpath,
                    old_rev, new_rev, conflict->ctx,
                    conflict->pool, scratch_pool));
        }
    }
  else if (operation == svn_wc_operation_switch ||
           operation == svn_wc_operation_merge)
    {
      if (old_rev < new_rev)
        {
          svn_revnum_t deleted_rev;
          const char *deleted_rev_author;
          svn_node_kind_t replacing_node_kind;
          struct repos_move_info *move;

          /* The switch/merge operation went forward in history.
           *
           * The deletion of the node happened on the branch we switched to
           * or merged from. Scan new_repos_relpath's parent's log to find
           * the revision which deleted the node. */
          SVN_ERR(find_revision_for_suspected_deletion(
                    &deleted_rev, &deleted_rev_author, &replacing_node_kind,
                    &move, conflict,
                    svn_relpath_basename(new_repos_relpath, scratch_pool),
                    svn_relpath_dirname(new_repos_relpath, scratch_pool),
                    new_rev, old_rev, old_repos_relpath, old_rev,
                    scratch_pool, scratch_pool));
          if (deleted_rev == SVN_INVALID_REVNUM)
            {
              /* We could not determine the revision in which the node was
               * deleted. We cannot provide the required details so the best
               * we can do is fall back to the default description. */
              return SVN_NO_ERROR;
            }

          details = apr_pcalloc(conflict->pool, sizeof(*details));
          details->deleted_rev = deleted_rev;
          details->added_rev = SVN_INVALID_REVNUM;
          details->repos_relpath = apr_pstrdup(conflict->pool,
                                               new_repos_relpath);
          details->rev_author = apr_pstrdup(conflict->pool,
                                            deleted_rev_author);
          details->replacing_node_kind = replacing_node_kind;
          details->move = move;
        }
      else /* new_rev < old_rev */
        {
          /* The switch/merge operation went backwards in history.
           * Figure out when the node we switched away from, or merged
           * from another branch, was added. */
          SVN_ERR(get_incoming_delete_details_for_reverse_addition(
                    &details, repos_root_url, old_repos_relpath,
                    old_rev, new_rev, conflict->ctx,
                    conflict->pool, scratch_pool));
        }
    }
  else
    {
      details = NULL;
    }

  conflict->tree_conflict_incoming_details = details;

  return SVN_NO_ERROR;
}

/* Details for tree conflicts involving incoming additions. */
struct conflict_tree_incoming_add_details
{
  /* If not SVN_INVALID_REVNUM, the node was added in ADDED_REV. */
  svn_revnum_t added_rev;

  /* If not SVN_INVALID_REVNUM, the node was deleted in DELETED_REV.
   * Note that both ADDED_REV and DELETED_REV may be valid for update/switch.
   * See comment in conflict_tree_get_details_incoming_add() for details. */
  svn_revnum_t deleted_rev;

  /* The path which was added/deleted relative to the repository root. */
  const char *repos_relpath;

  /* Authors who committed ADDED_REV/DELETED_REV. */
  const char *added_rev_author;
  const char *deleted_rev_author;

  /* Move information, in case the item was not added/deleted but moved here
   * or moved away. Else NULL. */
  struct repos_move_info *move;
};

/* Implements tree_conflict_get_details_func_t.
 * Find the revision in which the victim was added in the repository. */
static svn_error_t *
conflict_tree_get_details_incoming_add(svn_client_conflict_t *conflict,
                                       apr_pool_t *scratch_pool)
{
  const char *old_repos_relpath;
  const char *new_repos_relpath;
  const char *repos_root_url;
  const char *repos_uuid;
  svn_revnum_t old_rev;
  svn_revnum_t new_rev;
  struct conflict_tree_incoming_add_details *details;
  svn_wc_operation_t operation;

  SVN_ERR(svn_client_conflict_get_incoming_old_repos_location(
            &old_repos_relpath, &old_rev, NULL, conflict, scratch_pool,
            scratch_pool));
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &new_repos_relpath, &new_rev, NULL, conflict, scratch_pool,
            scratch_pool));
  SVN_ERR(svn_client_conflict_get_repos_info(&repos_root_url, &repos_uuid,
                                             conflict,
                                             scratch_pool, scratch_pool));
  operation = svn_client_conflict_get_operation(conflict);

  if (operation == svn_wc_operation_update ||
      operation == svn_wc_operation_switch)
    {
      /* Only the new repository location is recorded for the node which
       * caused an incoming addition. There is no pre-update/pre-switch
       * revision to be recorded for the node since it does not exist in
       * the repository at that revision.
       * The implication is that we cannot know whether the operation went
       * forward or backwards in history. So always try to find an added
       * and a deleted revision for the node. Users must figure out by whether
       * the addition or deletion caused the conflict. */
      const char *url;
      const char *corrected_url;
      svn_string_t *author_revprop;
      struct find_added_rev_baton b;
      svn_ra_session_t *ra_session;
      svn_revnum_t deleted_rev;
      svn_revnum_t head_rev;

      url = svn_path_url_add_component2(repos_root_url, new_repos_relpath,
                                        scratch_pool);
      SVN_ERR(svn_client__open_ra_session_internal(&ra_session,
                                                   &corrected_url,
                                                   url, NULL, NULL,
                                                   FALSE,
                                                   FALSE,
                                                   conflict->ctx,
                                                   scratch_pool,
                                                   scratch_pool));

      details = apr_pcalloc(conflict->pool, sizeof(*details));
      b.added_rev = SVN_INVALID_REVNUM;
      b.repos_relpath = NULL;
      b.parent_repos_relpath = NULL;
      b.pool = scratch_pool;
      /* Figure out when this node was added. */
      SVN_ERR(svn_ra_get_location_segments(ra_session, "", new_rev,
                                           new_rev, SVN_INVALID_REVNUM,
                                           find_added_rev, &b,
                                           scratch_pool));
      SVN_ERR(svn_ra_rev_prop(ra_session, b.added_rev,
                              SVN_PROP_REVISION_AUTHOR,
                              &author_revprop, scratch_pool));
      details->repos_relpath = apr_pstrdup(conflict->pool, b.repos_relpath);
      details->added_rev = b.added_rev;
      details->added_rev_author = apr_pstrdup(conflict->pool,
                                        author_revprop->data);

      details->deleted_rev = SVN_INVALID_REVNUM;
      details->deleted_rev_author = NULL;

      /* Figure out whether this node was deleted later.
       * ### Could probably optimize by infering both addition and deletion
       * ### from svn_ra_get_location_segments() call above. */
      SVN_ERR(svn_ra_get_latest_revnum(ra_session, &head_rev, scratch_pool));
      if (new_rev < head_rev)
        {
          SVN_ERR(svn_ra_get_deleted_rev(ra_session, "", new_rev, head_rev,
                                         &deleted_rev, scratch_pool));
          if (SVN_IS_VALID_REVNUM(deleted_rev))
           {
              SVN_ERR(svn_ra_rev_prop(ra_session, deleted_rev,
                                      SVN_PROP_REVISION_AUTHOR,
                                      &author_revprop, scratch_pool));
              details->deleted_rev = deleted_rev;
              details->deleted_rev_author = apr_pstrdup(conflict->pool,
                                                        author_revprop->data);
            }
        }
    }
  else if (operation == svn_wc_operation_merge)
    {
      if (old_rev < new_rev)
        {
          /* The merge operation went forwards in history.
           * The addition of the node happened on the branch we merged form.
           * Scan the nodes's history to find the revision which added it. */
          const char *url;
          const char *corrected_url;
          svn_string_t *author_revprop;
          struct find_added_rev_baton b;
          svn_ra_session_t *ra_session;

          url = svn_path_url_add_component2(repos_root_url, new_repos_relpath,
                                            scratch_pool);
          SVN_ERR(svn_client__open_ra_session_internal(&ra_session,
                                                       &corrected_url,
                                                       url, NULL, NULL,
                                                       FALSE,
                                                       FALSE,
                                                       conflict->ctx,
                                                       scratch_pool,
                                                       scratch_pool));

          details = apr_pcalloc(conflict->pool, sizeof(*details));
          b.added_rev = SVN_INVALID_REVNUM;
          b.repos_relpath = NULL;
          b.parent_repos_relpath = NULL;
          b.pool = scratch_pool;
          /* Figure out when this node was added. */
          SVN_ERR(svn_ra_get_location_segments(ra_session, "", new_rev,
                                               new_rev, old_rev,
                                               find_added_rev, &b,
                                               scratch_pool));
          SVN_ERR(svn_ra_rev_prop(ra_session, b.added_rev,
                                  SVN_PROP_REVISION_AUTHOR,
                                  &author_revprop, scratch_pool));
          details->repos_relpath = apr_pstrdup(conflict->pool, b.repos_relpath);
          details->added_rev = b.added_rev;
          details->added_rev_author = apr_pstrdup(conflict->pool,
                                                  author_revprop->data);

          details->deleted_rev = SVN_INVALID_REVNUM;
          details->deleted_rev_author = NULL;
        }
      else
        {
          /* The merge operation was a reverse-merge.
           * This addition is in fact a deletion, applied in reverse,
           * which happened on the branch we merged from.
           * Find the revision which deleted the node. */
          svn_revnum_t deleted_rev;
          const char *deleted_rev_author;
          svn_node_kind_t replacing_node_kind;
          struct repos_move_info *move;

          SVN_ERR(find_revision_for_suspected_deletion(
                    &deleted_rev, &deleted_rev_author, &replacing_node_kind,
                    &move, conflict, svn_relpath_basename(new_repos_relpath,
                                                   scratch_pool),
                    svn_relpath_dirname(new_repos_relpath, scratch_pool),
                    new_rev, old_rev,
                    NULL, SVN_INVALID_REVNUM, /* related to self */
                    conflict->pool, scratch_pool));
          if (deleted_rev == SVN_INVALID_REVNUM)
            {
              /* We could not determine the revision in which the node was
               * deleted. We cannot provide the required details so the best
               * we can do is fall back to the default description. */
              return SVN_NO_ERROR;
            }

          details = apr_pcalloc(conflict->pool, sizeof(*details));
          details->repos_relpath = apr_pstrdup(conflict->pool,
                                               new_repos_relpath);
          details->deleted_rev = deleted_rev;
          details->deleted_rev_author = apr_pstrdup(conflict->pool,
                                                    deleted_rev_author);

          details->added_rev = SVN_INVALID_REVNUM;
          details->added_rev_author = NULL;
          details->move = move;
        }
    }
  else
    {
      details = NULL;
    }

  conflict->tree_conflict_incoming_details = details;

  return SVN_NO_ERROR;
}

static const char *
describe_incoming_add_upon_update(
  struct conflict_tree_incoming_add_details *details,
  svn_node_kind_t new_node_kind,
  svn_revnum_t new_rev,
  apr_pool_t *result_pool)
{
  if (new_node_kind == svn_node_dir)
    {
      if (SVN_IS_VALID_REVNUM(details->added_rev) &&
          SVN_IS_VALID_REVNUM(details->deleted_rev))
        return apr_psprintf(result_pool,
                            _("A new directory appeared during update to r%ld; "
                              "it was added by %s in r%ld and later deleted "
                              "by %s in r%ld."), new_rev,
                            details->added_rev_author, details->added_rev,
                            details->deleted_rev_author, details->deleted_rev);
      else if (SVN_IS_VALID_REVNUM(details->added_rev))
        return apr_psprintf(result_pool,
                            _("A new directory appeared during update to r%ld; "
                              "it was added by %s in r%ld."), new_rev,
                            details->added_rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("A new directory appeared during update to r%ld; "
                              "it was deleted by %s in r%ld."), new_rev,
                            details->deleted_rev_author, details->deleted_rev);
    }
  else if (new_node_kind == svn_node_file ||
           new_node_kind == svn_node_symlink)
      if (SVN_IS_VALID_REVNUM(details->added_rev) &&
          SVN_IS_VALID_REVNUM(details->deleted_rev))
        return apr_psprintf(result_pool,
                            _("A new file appeared during update to r%ld; "
                              "it was added by %s in r%ld and later deleted "
                              "by %s in r%ld."), new_rev,
                            details->added_rev_author, details->added_rev,
                            details->deleted_rev_author, details->deleted_rev);
      else if (SVN_IS_VALID_REVNUM(details->added_rev))
        return apr_psprintf(result_pool,
                            _("A new file appeared during update to r%ld; "
                              "it was added by %s in r%ld."), new_rev,
                            details->added_rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("A new file appeared during update to r%ld; "
                              "it was deleted by %s in r%ld."), new_rev,
                            details->deleted_rev_author, details->deleted_rev);
  else
      if (SVN_IS_VALID_REVNUM(details->added_rev) &&
          SVN_IS_VALID_REVNUM(details->deleted_rev))
        return apr_psprintf(result_pool,
                            _("A new item appeared during update to r%ld; "
                              "it was added by %s in r%ld and later deleted "
                              "by %s in r%ld."), new_rev,
                            details->added_rev_author, details->added_rev,
                            details->deleted_rev_author, details->deleted_rev);
      else if (SVN_IS_VALID_REVNUM(details->added_rev))
        return apr_psprintf(result_pool,
                            _("A new item appeared during update to r%ld; "
                              "it was added by %s in r%ld."), new_rev,
                            details->added_rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("A new item appeared during update to r%ld; "
                              "it was deleted by %s in r%ld."), new_rev,
                            details->deleted_rev_author, details->deleted_rev);

  return SVN_NO_ERROR;
}

static const char *
describe_incoming_add_upon_switch(
  struct conflict_tree_incoming_add_details *details,
  svn_node_kind_t victim_node_kind,
  const char *new_repos_relpath,
  svn_revnum_t new_rev,
  apr_pool_t *result_pool)
{
  if (victim_node_kind == svn_node_dir)
    {
      if (SVN_IS_VALID_REVNUM(details->added_rev) &&
          SVN_IS_VALID_REVNUM(details->deleted_rev))
        return apr_psprintf(result_pool,
                            _("A new directory appeared during switch to\n"
                              "'^/%s@%ld'.\n"
                              "It was added by %s in r%ld and later deleted "
                              "by %s in r%ld."), new_repos_relpath, new_rev,
                            details->added_rev_author, details->added_rev,
                            details->deleted_rev_author, details->deleted_rev);
      else if (SVN_IS_VALID_REVNUM(details->added_rev))
        return apr_psprintf(result_pool,
                            _("A new directory appeared during switch to\n"
                             "'^/%s@%ld'.\nIt was added by %s in r%ld."),
                            new_repos_relpath, new_rev,
                            details->added_rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("A new directory appeared during switch to\n"
                              "'^/%s@%ld'.\nIt was deleted by %s in r%ld."),
                            new_repos_relpath, new_rev,
                            details->deleted_rev_author, details->deleted_rev);
    }
  else if (victim_node_kind == svn_node_file ||
           victim_node_kind == svn_node_symlink)
    {
      if (SVN_IS_VALID_REVNUM(details->added_rev) &&
          SVN_IS_VALID_REVNUM(details->deleted_rev))
        return apr_psprintf(result_pool,
                            _("A new file appeared during switch to\n"
                              "'^/%s@%ld'.\n"
                              "It was added by %s in r%ld and later deleted "
                              "by %s in r%ld."), new_repos_relpath, new_rev,
                            details->added_rev_author, details->added_rev,
                            details->deleted_rev_author, details->deleted_rev);
      else if (SVN_IS_VALID_REVNUM(details->added_rev))
        return apr_psprintf(result_pool,
                            _("A new file appeared during switch to\n"
                              "'^/%s@%ld'.\n"
                              "It was added by %s in r%ld."),
                            new_repos_relpath, new_rev,
                            details->added_rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("A new file appeared during switch to\n"
                              "'^/%s@%ld'.\n"
                              "It was deleted by %s in r%ld."),
                            new_repos_relpath, new_rev,
                            details->deleted_rev_author, details->deleted_rev);
    }
  else
    {
      if (SVN_IS_VALID_REVNUM(details->added_rev) &&
          SVN_IS_VALID_REVNUM(details->deleted_rev))
        return apr_psprintf(result_pool,
                            _("A new item appeared during switch to\n"
                              "'^/%s@%ld'.\n"
                              "It was added by %s in r%ld and later deleted "
                              "by %s in r%ld."), new_repos_relpath, new_rev,
                            details->added_rev_author, details->added_rev,
                            details->deleted_rev_author, details->deleted_rev);
      else if (SVN_IS_VALID_REVNUM(details->added_rev))
        return apr_psprintf(result_pool,
                            _("A new item appeared during switch to\n"
                              "'^/%s@%ld'.\n"
                              "It was added by %s in r%ld."),
                            new_repos_relpath, new_rev,
                            details->added_rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("A new item appeared during switch to\n"
                              "'^/%s@%ld'.\n"
                              "It was deleted by %s in r%ld."),
                            new_repos_relpath, new_rev,
                            details->deleted_rev_author, details->deleted_rev);
    }
  return SVN_NO_ERROR;
}

static const char *
describe_incoming_add_upon_merge(
  struct conflict_tree_incoming_add_details *details,
  svn_node_kind_t new_node_kind,
  svn_revnum_t old_rev,
  const char *new_repos_relpath,
  svn_revnum_t new_rev,
  apr_pool_t *result_pool)
{
  if (new_node_kind == svn_node_dir)
    {
      if (old_rev + 1 == new_rev)
        return apr_psprintf(result_pool,
                            _("A new directory appeared during merge of\n"
                              "'^/%s:%ld'.\nIt was added by %s in r%ld."),
                            new_repos_relpath, new_rev,
                            details->added_rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("A new directory appeared during merge of\n"
                              "'^/%s:%ld-%ld'.\nIt was added by %s in r%ld."),
                            new_repos_relpath, old_rev + 1, new_rev,
                            details->added_rev_author, details->added_rev);
    }
  else if (new_node_kind == svn_node_file ||
           new_node_kind == svn_node_symlink)
    {
      if (old_rev + 1 == new_rev)
        return apr_psprintf(result_pool,
                            _("A new file appeared during merge of\n"
                              "'^/%s:%ld'.\nIt was added by %s in r%ld."),
                            new_repos_relpath, new_rev,
                            details->added_rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("A new file appeared during merge of\n"
                              "'^/%s:%ld-%ld'.\nIt was added by %s in r%ld."),
                            new_repos_relpath, old_rev + 1, new_rev,
                            details->added_rev_author, details->added_rev);
    }
  else
    {
      if (old_rev + 1 == new_rev)
        return apr_psprintf(result_pool,
                            _("A new item appeared during merge of\n"
                              "'^/%s:%ld'.\nIt was added by %s in r%ld."),
                            new_repos_relpath, new_rev,
                            details->added_rev_author, details->added_rev);
      else
        return apr_psprintf(result_pool,
                            _("A new item appeared during merge of\n"
                              "'^/%s:%ld-%ld'.\nIt was added by %s in r%ld."),
                            new_repos_relpath, old_rev + 1, new_rev,
                            details->added_rev_author, details->added_rev);
    }
  return SVN_NO_ERROR;
}

static const char *
describe_incoming_reverse_deletion_upon_merge(
  struct conflict_tree_incoming_add_details *details,
  svn_node_kind_t new_node_kind,
  const char *old_repos_relpath,
  svn_revnum_t old_rev,
  svn_revnum_t new_rev,
  apr_pool_t *result_pool)
{
  if (new_node_kind == svn_node_dir)
    {
      if (new_rev + 1 == old_rev)
        return apr_psprintf(result_pool,
                            _("A new directory appeared during reverse-merge of"
                              "\n'^/%s:%ld'.\nIt was deleted by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            details->deleted_rev_author,
                            details->deleted_rev);
      else
        return apr_psprintf(result_pool,
                            _("A new directory appeared during reverse-merge "
                              "of\n'^/%s:%ld-%ld'.\n"
                              "It was deleted by %s in r%ld."),
                            old_repos_relpath, new_rev, old_rev - 1,
                            details->deleted_rev_author,
                            details->deleted_rev);
    }
  else if (new_node_kind == svn_node_file ||
           new_node_kind == svn_node_symlink)
    {
      if (new_rev + 1 == old_rev)
        return apr_psprintf(result_pool,
                            _("A new file appeared during reverse-merge of\n"
                              "'^/%s:%ld'.\nIt was deleted by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            details->deleted_rev_author,
                            details->deleted_rev);
      else
        return apr_psprintf(result_pool,
                            _("A new file appeared during reverse-merge of\n"
                              "'^/%s:%ld-%ld'.\nIt was deleted by %s in r%ld."),
                            old_repos_relpath, new_rev + 1, old_rev,
                            details->deleted_rev_author,
                            details->deleted_rev);
    }
  else
    {
      if (new_rev + 1 == old_rev)
        return apr_psprintf(result_pool,
                            _("A new item appeared during reverse-merge of\n"
                              "'^/%s:%ld'.\nIt was deleted by %s in r%ld."),
                            old_repos_relpath, old_rev,
                            details->deleted_rev_author,
                            details->deleted_rev);
      else
        return apr_psprintf(result_pool,
                            _("A new item appeared during reverse-merge of\n"
                              "'^/%s:%ld-%ld'.\nIt was deleted by %s in r%ld."),
                            old_repos_relpath, new_rev + 1, old_rev,
                            details->deleted_rev_author,
                            details->deleted_rev);
    }

  return SVN_NO_ERROR;
}

/* Implements tree_conflict_get_description_func_t. */
static svn_error_t *
conflict_tree_get_description_incoming_add(
  const char **incoming_change_description,
  svn_client_conflict_t *conflict,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool)
{
  const char *action;
  svn_node_kind_t victim_node_kind;
  svn_wc_operation_t conflict_operation;
  const char *old_repos_relpath;
  svn_revnum_t old_rev;
  svn_node_kind_t old_node_kind;
  const char *new_repos_relpath;
  svn_revnum_t new_rev;
  svn_node_kind_t new_node_kind;
  struct conflict_tree_incoming_add_details *details;

  if (conflict->tree_conflict_incoming_details == NULL)
    return svn_error_trace(conflict_tree_get_incoming_description_generic(
                             incoming_change_description,
                             conflict, result_pool, scratch_pool));

  conflict_operation = svn_client_conflict_get_operation(conflict);
  victim_node_kind = svn_client_conflict_tree_get_victim_node_kind(conflict);

  SVN_ERR(svn_client_conflict_get_incoming_old_repos_location(
            &old_repos_relpath, &old_rev, &old_node_kind, conflict,
            scratch_pool, scratch_pool));
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &new_repos_relpath, &new_rev, &new_node_kind, conflict,
            scratch_pool, scratch_pool));

  details = conflict->tree_conflict_incoming_details;

  if (conflict_operation == svn_wc_operation_update)
    {
      action = describe_incoming_add_upon_update(details,
                                                 new_node_kind,
                                                 new_rev,
                                                 result_pool);
    }
  else if (conflict_operation == svn_wc_operation_switch)
    {
      action = describe_incoming_add_upon_switch(details,
                                                 victim_node_kind,
                                                 new_repos_relpath,
                                                 new_rev,
                                                 result_pool);
    }
  else if (conflict_operation == svn_wc_operation_merge)
    {
      if (old_rev < new_rev)
        action = describe_incoming_add_upon_merge(details,
                                                  new_node_kind,
                                                  old_rev,
                                                  new_repos_relpath,
                                                  new_rev,
                                                  result_pool);
      else
        action = describe_incoming_reverse_deletion_upon_merge(
                   details, new_node_kind, old_repos_relpath,
                   old_rev, new_rev, result_pool);
    }

  *incoming_change_description = apr_pstrdup(result_pool, action);

  return SVN_NO_ERROR;
}

/* Details for tree conflicts involving incoming edits.
 * Note that we store an array of these. Each element corresponds to a
 * revision within the old/new range in which a modification occured. */
struct conflict_tree_incoming_edit_details
{
  /* The revision in which the edit ocurred. */
  svn_revnum_t rev;

  /* The author of the revision. */
  const char *author;

  /** Is the text modified? May be svn_tristate_unknown. */
  svn_tristate_t text_modified;

  /** Are properties modified? May be svn_tristate_unknown. */
  svn_tristate_t props_modified;

  /** For directories, are children modified?
   * May be svn_tristate_unknown. */
  svn_tristate_t children_modified;

  /* The path which was edited, relative to the repository root. */
  const char *repos_relpath;
};

/* Baton for find_modified_rev(). */
struct find_modified_rev_baton {
  apr_array_header_t *edits;
  const char *repos_relpath;
  svn_node_kind_t node_kind;
  apr_pool_t *result_pool;
  apr_pool_t *scratch_pool;
};

/* Implements svn_log_entry_receiver_t. */
static svn_error_t *
find_modified_rev(void *baton,
                  svn_log_entry_t *log_entry,
                  apr_pool_t *scratch_pool)
{
  struct find_modified_rev_baton *b = baton;
  struct conflict_tree_incoming_edit_details *details = NULL;
  svn_string_t *author;
  apr_hash_index_t *hi;
  apr_pool_t *iterpool;

  /* No paths were changed in this revision.  Nothing to do. */
  if (! log_entry->changed_paths2)
    return SVN_NO_ERROR;

  details = apr_pcalloc(b->result_pool, sizeof(*details));
  details->rev = log_entry->revision;
  author = svn_hash_gets(log_entry->revprops, SVN_PROP_REVISION_AUTHOR);
  details->author = apr_pstrdup(b->result_pool, author->data);

  details->text_modified = svn_tristate_unknown;
  details->props_modified = svn_tristate_unknown;
  details->children_modified = svn_tristate_unknown;

  iterpool = svn_pool_create(scratch_pool);
  for (hi = apr_hash_first(scratch_pool, log_entry->changed_paths2);
       hi != NULL;
       hi = apr_hash_next(hi))
    {
      void *val;
      const char *path;
      svn_log_changed_path2_t *log_item;

      svn_pool_clear(iterpool);

      apr_hash_this(hi, (void *) &path, NULL, &val);
      log_item = val;

      /* ### Remove leading slash from paths in log entries. */
      if (path[0] == '/')
          path = svn_relpath_canonicalize(path, iterpool);

      if (svn_path_compare_paths(b->repos_relpath, path) == 0 &&
          (log_item->action == 'M' || log_item->action == 'A'))
        {
          details->text_modified = log_item->text_modified;
          details->props_modified = log_item->props_modified;
          details->repos_relpath = apr_pstrdup(b->result_pool, path);

          if (log_item->copyfrom_path)
            b->repos_relpath = apr_pstrdup(b->scratch_pool,
                                           log_item->copyfrom_path);
        }
      else if (b->node_kind == svn_node_dir &&
               svn_relpath_skip_ancestor(b->repos_relpath, path) != NULL)
        details->children_modified = svn_tristate_true;
    }

  if (details)
    {
      if (b->node_kind == svn_node_dir &&
          details->children_modified == svn_tristate_unknown)
            details->children_modified = svn_tristate_false;

      APR_ARRAY_PUSH(b->edits, struct conflict_tree_incoming_edit_details *) =
        details;
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

/* Implements tree_conflict_get_details_func_t.
 * Find one or more revisions in which the victim was modified in the
 * repository. */
static svn_error_t *
conflict_tree_get_details_incoming_edit(svn_client_conflict_t *conflict,
                                        apr_pool_t *scratch_pool)
{
  const char *old_repos_relpath;
  const char *new_repos_relpath;
  const char *repos_root_url;
  const char *repos_uuid;
  svn_revnum_t old_rev;
  svn_revnum_t new_rev;
  svn_node_kind_t old_node_kind;
  svn_node_kind_t new_node_kind;
  svn_wc_operation_t operation;
  const char *url;
  const char *corrected_url;
  svn_ra_session_t *ra_session;
  apr_array_header_t *paths;
  apr_array_header_t *revprops;
  struct find_modified_rev_baton b;

  SVN_ERR(svn_client_conflict_get_incoming_old_repos_location(
            &old_repos_relpath, &old_rev, &old_node_kind, conflict,
            scratch_pool, scratch_pool));
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &new_repos_relpath, &new_rev, &new_node_kind, conflict,
            scratch_pool, scratch_pool));
  SVN_ERR(svn_client_conflict_get_repos_info(&repos_root_url, &repos_uuid,
                                             conflict,
                                             scratch_pool, scratch_pool));
  operation = svn_client_conflict_get_operation(conflict);

  b.result_pool = conflict->pool;
  b.scratch_pool = scratch_pool;
  b.edits = apr_array_make(
               conflict->pool, 0,
               sizeof(struct conflict_tree_incoming_edit_details *));
  paths = apr_array_make(scratch_pool, 1, sizeof(const char *));
  APR_ARRAY_PUSH(paths, const char *) = "";

  revprops = apr_array_make(scratch_pool, 1, sizeof(const char *));
  APR_ARRAY_PUSH(revprops, const char *) = SVN_PROP_REVISION_AUTHOR;

  if (operation == svn_wc_operation_update)
    {
      url = svn_path_url_add_component2(repos_root_url,
                                        old_rev < new_rev ? new_repos_relpath
                                                          : old_repos_relpath,
                                        scratch_pool);

      b.repos_relpath = old_rev < new_rev ? new_repos_relpath
                                          : old_repos_relpath;
      b.node_kind = old_rev < new_rev ? new_node_kind : old_node_kind;
    }
  else if (operation == svn_wc_operation_switch ||
           operation == svn_wc_operation_merge)
    {
      url = svn_path_url_add_component2(repos_root_url, new_repos_relpath,
                                        scratch_pool);

      b.repos_relpath = new_repos_relpath;
      b.node_kind = new_node_kind;
    }

  SVN_ERR(svn_client__open_ra_session_internal(&ra_session,
                                               &corrected_url,
                                               url, NULL, NULL,
                                               FALSE,
                                               FALSE,
                                               conflict->ctx,
                                               scratch_pool,
                                               scratch_pool));
  SVN_ERR(svn_ra_get_log2(ra_session, paths,
                          old_rev < new_rev ? old_rev : new_rev,
                          old_rev < new_rev ? new_rev : old_rev,
                          0, /* no limit */
                          TRUE, /* need the changed paths list */
                          FALSE, /* need to traverse copies */
                          FALSE, /* no need for merged revisions */
                          revprops,
                          find_modified_rev, &b,
                          scratch_pool));

  conflict->tree_conflict_incoming_details = b.edits;

  return SVN_NO_ERROR;
}

static const char *
describe_incoming_edit_upon_update(svn_revnum_t old_rev,
                                   svn_revnum_t new_rev,
                                   svn_node_kind_t old_node_kind,
                                   svn_node_kind_t new_node_kind,
                                   apr_pool_t *result_pool)
{
  if (old_rev < new_rev)
    {
      if (new_node_kind == svn_node_dir)
        return apr_psprintf(result_pool,
                            _("Changes destined for a directory arrived "
                              "via the following revisions during update "
                              "from r%ld to r%ld."), old_rev, new_rev);
      else if (new_node_kind == svn_node_file ||
               new_node_kind == svn_node_symlink)
        return apr_psprintf(result_pool,
                            _("Changes destined for a file arrived "
                              "via the following revisions during update "
                              "from r%ld to r%ld"), old_rev, new_rev);
      else
        return apr_psprintf(result_pool,
                            _("Changes from the following revisions arrived "
                              "during update from r%ld to r%ld"),
                            old_rev, new_rev);
    }
  else
    {
      if (new_node_kind == svn_node_dir)
        return apr_psprintf(result_pool,
                            _("Changes destined for a directory arrived "
                              "via the following revisions during backwards "
                              "update from r%ld to r%ld"),
                            old_rev, new_rev);
      else if (new_node_kind == svn_node_file ||
               new_node_kind == svn_node_symlink)
        return apr_psprintf(result_pool,
                            _("Changes destined for a file arrived "
                              "via the following revisions during backwards "
                              "update from r%ld to r%ld"),
                            old_rev, new_rev);
      else
        return apr_psprintf(result_pool,
                            _("Changes from the following revisions arrived "
                              "during backwards update from r%ld to r%ld"),
                            old_rev, new_rev);
    }
}

static const char *
describe_incoming_edit_upon_switch(const char *new_repos_relpath,
                                   svn_revnum_t new_rev,
                                   svn_node_kind_t new_node_kind,
                                   apr_pool_t *result_pool)
{
  if (new_node_kind == svn_node_dir)
    return apr_psprintf(result_pool,
                        _("Changes destined for a directory arrived via "
                          "the following revisions during switch to\n"
                          "'^/%s@r%ld'"),
                        new_repos_relpath, new_rev);
  else if (new_node_kind == svn_node_file ||
           new_node_kind == svn_node_symlink)
    return apr_psprintf(result_pool,
                        _("Changes destined for a directory arrived via "
                          "the following revisions during switch to\n"
                          "'^/%s@r%ld'"),
                        new_repos_relpath, new_rev);
  else
    return apr_psprintf(result_pool,
                        _("Changes from the following revisions arrived "
                          "during switch to\n'^/%s@r%ld'"),
                        new_repos_relpath, new_rev);
}

/* Return a string showing the list of revisions in EDITS, ensuring
 * the string won't grow too large for display. */
static const char *
describe_incoming_edit_list_modified_revs(apr_array_header_t *edits,
                                          apr_pool_t *result_pool)
{
  int num_revs_to_skip;
  static const int min_revs_for_skipping = 5;
  static const int max_revs_to_display = 8;
  const char *s = "";
  int i;

  if (edits->nelts <= max_revs_to_display)
    num_revs_to_skip = 0;
  else
    {
      /* Check if we should insert a placeholder for some revisions because
       * the string would grow too long for display otherwise. */
      num_revs_to_skip = edits->nelts - max_revs_to_display;
      if (num_revs_to_skip < min_revs_for_skipping)
        {
          /* Don't bother with the placeholder. Just list all revisions. */
          num_revs_to_skip = 0;
        }
    }

  for (i = 0; i < edits->nelts; i++)
    {
      struct conflict_tree_incoming_edit_details *details;

      details = APR_ARRAY_IDX(edits, i,
                              struct conflict_tree_incoming_edit_details *);
      if (num_revs_to_skip > 0)
        {
          /* Insert a placeholder for revisions falling into the middle of
           * the range so we'll get something that looks like:
           * 1, 2, 3, 4, 5 [ placeholder ] 95, 96, 97, 98, 99 */
          if (i < max_revs_to_display / 2)
            s = apr_psprintf(result_pool, _("%s r%ld by %s%s"), s,
                             details->rev, details->author,
                             i < edits->nelts - 1 ? "," : "");
          else if (i >= max_revs_to_display / 2 &&
                   i < edits->nelts - (max_revs_to_display / 2))
              continue;
          else
            {
              if (i == edits->nelts - (max_revs_to_display / 2))
                  s = apr_psprintf(result_pool,
                                   _("%s\n [%d revisions omitted for "
                                     "brevity],\n"),
                                   s, num_revs_to_skip);

              s = apr_psprintf(result_pool, _("%s r%ld by %s%s"), s,
                               details->rev, details->author,
                               i < edits->nelts - 1 ? "," : "");
            }
        } 
      else
        s = apr_psprintf(result_pool, _("%s r%ld by %s%s"), s,
                         details->rev, details->author,
                         i < edits->nelts - 1 ? "," : "");
    }

  return s;
}

/* Implements tree_conflict_get_description_func_t. */
static svn_error_t *
conflict_tree_get_description_incoming_edit(
  const char **incoming_change_description,
  svn_client_conflict_t *conflict,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool)
{
  const char *action;
  svn_node_kind_t victim_node_kind;
  svn_wc_operation_t conflict_operation;
  const char *old_repos_relpath;
  svn_revnum_t old_rev;
  svn_node_kind_t old_node_kind;
  const char *new_repos_relpath;
  svn_revnum_t new_rev;
  svn_node_kind_t new_node_kind;
  apr_array_header_t *edits;

  if (conflict->tree_conflict_incoming_details == NULL)
    return svn_error_trace(conflict_tree_get_incoming_description_generic(
                             incoming_change_description,
                             conflict, result_pool, scratch_pool));

  SVN_ERR(svn_client_conflict_get_incoming_old_repos_location(
            &old_repos_relpath, &old_rev, &old_node_kind, conflict,
            scratch_pool, scratch_pool));
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &new_repos_relpath, &new_rev, &new_node_kind, conflict,
            scratch_pool, scratch_pool));

  conflict_operation = svn_client_conflict_get_operation(conflict);
  victim_node_kind = svn_client_conflict_tree_get_victim_node_kind(conflict);

  edits = conflict->tree_conflict_incoming_details;

  if (conflict_operation == svn_wc_operation_update)
    action = describe_incoming_edit_upon_update(old_rev, new_rev,
                                                old_node_kind, new_node_kind,
                                                scratch_pool);
  else if (conflict_operation == svn_wc_operation_switch)
    action = describe_incoming_edit_upon_switch(new_repos_relpath, new_rev,
                                                new_node_kind, scratch_pool);
  else if (conflict_operation == svn_wc_operation_merge)
    {
      /* Handle merge inline because it returns early sometimes. */
      if (old_rev < new_rev)
        {
          if (old_rev + 1 == new_rev)
            {
              if (new_node_kind == svn_node_dir)
                action = apr_psprintf(scratch_pool,
                                      _("Changes destined for a directory "
                                        "arrived during merge of\n"
                                        "'^/%s:%ld'."),
                                        new_repos_relpath, new_rev);
              else if (new_node_kind == svn_node_file ||
                       new_node_kind == svn_node_symlink)
                action = apr_psprintf(scratch_pool,
                                      _("Changes destined for a file "
                                        "arrived during merge of\n"
                                        "'^/%s:%ld'."),
                                      new_repos_relpath, new_rev);
              else
                action = apr_psprintf(scratch_pool,
                                      _("Changes arrived during merge of\n"
                                        "'^/%s:%ld'."),
                                      new_repos_relpath, new_rev);

              *incoming_change_description = apr_pstrdup(result_pool, action);

              return SVN_NO_ERROR;
            }
          else
            {
              if (new_node_kind == svn_node_dir)
                action = apr_psprintf(scratch_pool,
                                      _("Changes destined for a directory "
                                        "arrived via the following revisions "
                                        "during merge of\n'^/%s:%ld-%ld'"),
                                      new_repos_relpath, old_rev + 1, new_rev);
              else if (new_node_kind == svn_node_file ||
                       new_node_kind == svn_node_symlink)
                action = apr_psprintf(scratch_pool,
                                      _("Changes destined for a file "
                                        "arrived via the following revisions "
                                        "during merge of\n'^/%s:%ld-%ld'"),
                                      new_repos_relpath, old_rev + 1, new_rev);
              else
                action = apr_psprintf(scratch_pool,
                                      _("Changes from the following revisions "
                                        "arrived during merge of\n"
                                        "'^/%s:%ld-%ld'"),
                                      new_repos_relpath, old_rev + 1, new_rev);
            }
        }
      else
        {
          if (new_rev + 1 == old_rev)
            {
              if (new_node_kind == svn_node_dir)
                action = apr_psprintf(scratch_pool,
                                      _("Changes destined for a directory "
                                        "arrived during reverse-merge of\n"
                                        "'^/%s:%ld'."),
                                      new_repos_relpath, old_rev);
              else if (new_node_kind == svn_node_file ||
                       new_node_kind == svn_node_symlink)
                action = apr_psprintf(scratch_pool,
                                      _("Changes destined for a file "
                                        "arrived during reverse-merge of\n"
                                        "'^/%s:%ld'."),
                                      new_repos_relpath, old_rev);
              else
                action = apr_psprintf(scratch_pool,
                                      _("Changes arrived during reverse-merge "
                                        "of\n'^/%s:%ld'."),
                                      new_repos_relpath, old_rev);

              *incoming_change_description = apr_pstrdup(result_pool, action);

              return SVN_NO_ERROR;
            }
          else
            {
              if (new_node_kind == svn_node_dir)
                action = apr_psprintf(scratch_pool,
                                      _("Changes destined for a directory "
                                        "arrived via the following revisions "
                                        "during reverse-merge of\n"
                                        "'^/%s:%ld-%ld'"),
                                      new_repos_relpath, new_rev + 1, old_rev);
              else if (new_node_kind == svn_node_file ||
                       new_node_kind == svn_node_symlink)
                action = apr_psprintf(scratch_pool,
                                      _("Changes destined for a file "
                                        "arrived via the following revisions "
                                        "during reverse-merge of\n"
                                        "'^/%s:%ld-%ld'"),
                                      new_repos_relpath, new_rev + 1, old_rev);
                
              else
                action = apr_psprintf(scratch_pool,
                                      _("Changes from the following revisions "
                                        "arrived during reverse-merge of\n"
                                        "'^/%s:%ld-%ld'"),
                                      new_repos_relpath, new_rev + 1, old_rev);
            }
        }
    }

  action = apr_psprintf(scratch_pool, "%s:\n%s", action,
                        describe_incoming_edit_list_modified_revs(
                          edits, scratch_pool));
  *incoming_change_description = apr_pstrdup(result_pool, action);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_tree_get_description(
  const char **incoming_change_description,
  const char **local_change_description,
  svn_client_conflict_t *conflict,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool)
{
  SVN_ERR(conflict->tree_conflict_get_incoming_description_func(
            incoming_change_description,
            conflict, result_pool, scratch_pool));

  SVN_ERR(conflict->tree_conflict_get_local_description_func(
            local_change_description,
            conflict, result_pool, scratch_pool));
  
  return SVN_NO_ERROR;
}

void
svn_client_conflict_option_set_merged_propval(
  svn_client_conflict_option_t *option,
  const svn_string_t *merged_propval)
{
  option->type_data.prop.merged_propval = merged_propval;
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_postpone(svn_client_conflict_option_t *option,
                      svn_client_conflict_t *conflict,
                      apr_pool_t *scratch_pool)
{
  return SVN_NO_ERROR; /* Nothing to do. */
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_text_conflict(svn_client_conflict_option_t *option,
                      svn_client_conflict_t *conflict,
                      apr_pool_t *scratch_pool)
{
  svn_client_conflict_option_id_t option_id;
  const char *local_abspath;
  const char *lock_abspath;
  svn_wc_conflict_choice_t conflict_choice;
  svn_client_ctx_t *ctx = conflict->ctx;
  svn_error_t *err;

  option_id = svn_client_conflict_option_get_id(option);
  conflict_choice = conflict_option_id_to_wc_conflict_choice(option_id);
  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));
  err = svn_wc__conflict_text_mark_resolved(conflict->ctx->wc_ctx,
                                            local_abspath,
                                            conflict_choice,
                                            conflict->ctx->cancel_func,
                                            conflict->ctx->cancel_baton,
                                            conflict->ctx->notify_func2,
                                            conflict->ctx->notify_baton2,
                                            scratch_pool);
  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  svn_io_sleep_for_timestamps(local_abspath, scratch_pool);
  SVN_ERR(err);

  conflict->resolution_text = option_id;

  return SVN_NO_ERROR;
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_prop_conflict(svn_client_conflict_option_t *option,
                      svn_client_conflict_t *conflict,
                      apr_pool_t *scratch_pool)
{
  svn_client_conflict_option_id_t option_id;
  svn_wc_conflict_choice_t conflict_choice;
  const char *local_abspath;
  const char *lock_abspath;
  const char *propname = option->type_data.prop.propname;
  svn_client_ctx_t *ctx = conflict->ctx;
  svn_error_t *err;

  option_id = svn_client_conflict_option_get_id(option);
  conflict_choice = conflict_option_id_to_wc_conflict_choice(option_id);
  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));
  err = svn_wc__conflict_prop_mark_resolved(ctx->wc_ctx, local_abspath,
                                            propname, conflict_choice,
                                            conflict->ctx->notify_func2,
                                            conflict->ctx->notify_baton2,
                                            scratch_pool);
  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  svn_io_sleep_for_timestamps(local_abspath, scratch_pool);
  SVN_ERR(err);

  if (propname[0] == '\0')
    {
      apr_hash_index_t *hi;

      /* All properties have been resolved to the same option. */
      for (hi = apr_hash_first(scratch_pool, conflict->prop_conflicts);
           hi;
           hi = apr_hash_next(hi))
        {
          const char *this_propname = apr_hash_this_key(hi);

          svn_hash_sets(conflict->resolved_props,
                        apr_pstrdup(apr_hash_pool_get(conflict->resolved_props),
                                    this_propname),
                        option);
          svn_hash_sets(conflict->prop_conflicts, this_propname, NULL);
        }

      conflict->legacy_prop_conflict_propname = NULL;
    }
  else
    {
      svn_hash_sets(conflict->resolved_props,
                    apr_pstrdup(apr_hash_pool_get(conflict->resolved_props),
                                propname),
                   option);
      svn_hash_sets(conflict->prop_conflicts, propname, NULL);

      if (apr_hash_count(conflict->prop_conflicts) > 0)
        conflict->legacy_prop_conflict_propname =
            apr_hash_this_key(apr_hash_first(scratch_pool,
                                             conflict->prop_conflicts));
      else
        conflict->legacy_prop_conflict_propname = NULL;
    }

  return SVN_NO_ERROR;
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_accept_current_wc_state(svn_client_conflict_option_t *option,
                                svn_client_conflict_t *conflict,
                                apr_pool_t *scratch_pool)
{
  svn_client_conflict_option_id_t option_id;
  const char *local_abspath;
  const char *lock_abspath;
  svn_client_ctx_t *ctx = conflict->ctx;
  svn_error_t *err;

  option_id = svn_client_conflict_option_get_id(option);
  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  if (option_id != svn_client_conflict_option_accept_current_wc_state)
    return svn_error_createf(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE, NULL,
                             _("Tree conflict on '%s' can only be resolved "
                               "to the current working copy state"),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));

  /* Resolve to current working copy state. */
  err = svn_wc__del_tree_conflict(ctx->wc_ctx, local_abspath, scratch_pool);

  /* svn_wc__del_tree_conflict doesn't handle notification for us */
  if (ctx->notify_func2)
    ctx->notify_func2(ctx->notify_baton2,
                      svn_wc_create_notify(local_abspath,
                                           svn_wc_notify_resolved_tree,
                                           scratch_pool),
                      scratch_pool);

  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  SVN_ERR(err);

  conflict->resolution_tree = option_id;

  return SVN_NO_ERROR;
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_update_break_moved_away(svn_client_conflict_option_t *option,
                                svn_client_conflict_t *conflict,
                                apr_pool_t *scratch_pool)
{
  const char *local_abspath;
  const char *lock_abspath;
  svn_client_ctx_t *ctx = conflict->ctx;
  svn_error_t *err;

  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));
  err = svn_wc__conflict_tree_update_break_moved_away(ctx->wc_ctx,
                                                      local_abspath,
                                                      ctx->cancel_func,
                                                      ctx->cancel_baton,
                                                      ctx->notify_func2,
                                                      ctx->notify_baton2,
                                                      scratch_pool);
  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  SVN_ERR(err);

  conflict->resolution_tree = svn_client_conflict_option_get_id(option);

  return SVN_NO_ERROR;
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_update_raise_moved_away(svn_client_conflict_option_t *option,
                                svn_client_conflict_t *conflict,
                                apr_pool_t *scratch_pool)
{
  const char *local_abspath;
  const char *lock_abspath;
  svn_client_ctx_t *ctx = conflict->ctx;
  svn_error_t *err;

  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));
  err = svn_wc__conflict_tree_update_raise_moved_away(ctx->wc_ctx,
                                                      local_abspath,
                                                      ctx->cancel_func,
                                                      ctx->cancel_baton,
                                                      ctx->notify_func2,
                                                      ctx->notify_baton2,
                                                      scratch_pool);
  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  SVN_ERR(err);

  conflict->resolution_tree = svn_client_conflict_option_get_id(option);

  return SVN_NO_ERROR;
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_update_moved_away_node(svn_client_conflict_option_t *option,
                               svn_client_conflict_t *conflict,
                               apr_pool_t *scratch_pool)
{
  const char *local_abspath;
  const char *lock_abspath;
  svn_client_ctx_t *ctx = conflict->ctx;
  svn_error_t *err;

  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));
  err = svn_wc__conflict_tree_update_moved_away_node(ctx->wc_ctx,
                                                     local_abspath,
                                                     ctx->cancel_func,
                                                     ctx->cancel_baton,
                                                     ctx->notify_func2,
                                                     ctx->notify_baton2,
                                                     scratch_pool);
  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  svn_io_sleep_for_timestamps(local_abspath, scratch_pool);
  SVN_ERR(err);

  conflict->resolution_tree = svn_client_conflict_option_get_id(option);

  return SVN_NO_ERROR;
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_merge_incoming_add_ignore(svn_client_conflict_option_t *option,
                                  svn_client_conflict_t *conflict,
                                  apr_pool_t *scratch_pool)
{
  const char *local_abspath;
  const char *lock_abspath;
  svn_client_ctx_t *ctx = conflict->ctx;
  svn_error_t *err;

  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));

  /* All other options for this conflict actively fetch the incoming
   * new node. We can ignore the incoming new node by doing nothing. */

  /* Resolve to current working copy state. */
  err = svn_wc__del_tree_conflict(ctx->wc_ctx, local_abspath, scratch_pool);

  /* svn_wc__del_tree_conflict doesn't handle notification for us */
  if (ctx->notify_func2)
    ctx->notify_func2(ctx->notify_baton2,
                      svn_wc_create_notify(local_abspath,
                                           svn_wc_notify_resolved_tree,
                                           scratch_pool),
                      scratch_pool);

  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  SVN_ERR(err);

  conflict->resolution_tree = svn_client_conflict_option_get_id(option);

  return SVN_NO_ERROR;
}
/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_merge_incoming_added_file_text_merge(
  svn_client_conflict_option_t *option,
  svn_client_conflict_t *conflict,
  apr_pool_t *scratch_pool)
{
  svn_ra_session_t *ra_session;
  const char *url;
  const char *corrected_url;
  const char *repos_root_url;
  const char *repos_uuid;
  const char *wc_tmpdir;
  const char *incoming_new_repos_relpath;
  svn_revnum_t incoming_new_pegrev;
  const char *local_abspath;
  const char *lock_abspath;
  svn_client_ctx_t *ctx = conflict->ctx;
  svn_wc_merge_outcome_t merge_content_outcome;
  svn_wc_notify_state_t merge_props_outcome;
  apr_file_t *incoming_new_file;
  const char *incoming_new_tmp_abspath;
  apr_file_t *empty_file;
  const char *empty_file_abspath;
  svn_stream_t *incoming_new_stream;
  apr_hash_t *incoming_new_props;
  apr_hash_index_t *hi;
  apr_array_header_t *propdiffs;
  svn_error_t *err;

  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  /* Set up tempory storage for the repository version of file. */
  SVN_ERR(svn_wc__get_tmpdir(&wc_tmpdir, ctx->wc_ctx, local_abspath,
                             scratch_pool, scratch_pool));
  SVN_ERR(svn_io_open_unique_file3(&incoming_new_file,
                                   &incoming_new_tmp_abspath, wc_tmpdir,
                                   svn_io_file_del_on_pool_cleanup,
                                   scratch_pool, scratch_pool));
  incoming_new_stream = svn_stream_from_aprfile2(incoming_new_file, TRUE,
                                                 scratch_pool);

  /* Fetch the incoming added file from the repository. */
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &incoming_new_repos_relpath, &incoming_new_pegrev,
            NULL, conflict, scratch_pool,
            scratch_pool));
  SVN_ERR(svn_client_conflict_get_repos_info(&repos_root_url, &repos_uuid,
                                             conflict, scratch_pool,
                                             scratch_pool));
  url = svn_path_url_add_component2(repos_root_url, incoming_new_repos_relpath,
                                    scratch_pool);
  SVN_ERR(svn_client__open_ra_session_internal(&ra_session, &corrected_url,
                                               url, NULL, NULL, FALSE, FALSE,
                                               conflict->ctx, scratch_pool,
                                               scratch_pool));
  SVN_ERR(svn_ra_get_file(ra_session, "", incoming_new_pegrev,
                          incoming_new_stream, NULL, /* fetched_rev */
                          &incoming_new_props, scratch_pool));

  /* Flush file to disk. */
  SVN_ERR(svn_stream_close(incoming_new_stream));
  SVN_ERR(svn_io_file_flush(incoming_new_file, scratch_pool));

  /* Delete entry and wc props from the returned set of properties.. */
  for (hi = apr_hash_first(scratch_pool, incoming_new_props);
       hi != NULL;
       hi = apr_hash_next(hi))
    {
      const char *propname = apr_hash_this_key(hi);

      if (!svn_wc_is_normal_prop(propname))
        svn_hash_sets(incoming_new_props, propname, NULL);
    }

  /* Create an empty file as fake "merge-base" for the two added files.
   * The files are not ancestrally related so this is the best we can do. */
  SVN_ERR(svn_io_open_unique_file3(&empty_file, &empty_file_abspath, NULL,
                                   svn_io_file_del_on_pool_cleanup,
                                   scratch_pool, scratch_pool));

  /* Create a property diff which shows all props as added. */
  SVN_ERR(svn_prop_diffs(&propdiffs, incoming_new_props,
                         apr_hash_make(scratch_pool), scratch_pool));

  /* ### The following WC modifications should be atomic. */
  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));
  /* Resolve to current working copy state. svn_wc_merge5() requires this. */
  err = svn_wc__del_tree_conflict(ctx->wc_ctx, local_abspath, scratch_pool);
  if (err)
    return svn_error_compose_create(err,
                                    svn_wc__release_write_lock(ctx->wc_ctx,
                                                               lock_abspath,
                                                               scratch_pool));
  /* Perform the file merge. ### Merge into tempfile and then rename on top? */
  err = svn_wc_merge5(&merge_content_outcome, &merge_props_outcome,
                      ctx->wc_ctx, empty_file_abspath,
                      incoming_new_tmp_abspath, local_abspath,
                      NULL, NULL, NULL, /* labels */
                      NULL, NULL, /* conflict versions */
                      FALSE, /* dry run */
                      NULL, NULL, /* diff3_cmd, merge_options */
                      NULL, propdiffs,
                      NULL, NULL, /* conflict func/baton */
                      ctx->cancel_func, ctx->cancel_baton,
                      scratch_pool);
  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  svn_io_sleep_for_timestamps(local_abspath, scratch_pool);
  SVN_ERR(err);

  if (ctx->notify_func2)
    {
      svn_wc_notify_t *notify;

      /* Tell the world about the file merge that just happened. */
      notify = svn_wc_create_notify(local_abspath,
                                    svn_wc_notify_update_update,
                                    scratch_pool);
      if (merge_content_outcome == svn_wc_merge_conflict)
        notify->content_state = svn_wc_notify_state_conflicted;
      else
        notify->content_state = svn_wc_notify_state_merged;
      notify->prop_state = merge_props_outcome;
      notify->kind = svn_node_file;
      ctx->notify_func2(ctx->notify_baton2, notify, scratch_pool);

      /* And also about the successfully resolved tree conflict. */
      notify = svn_wc_create_notify(local_abspath, svn_wc_notify_resolved_tree,
                                    scratch_pool);
      ctx->notify_func2(ctx->notify_baton2, notify, scratch_pool);
    }

  conflict->resolution_tree = svn_client_conflict_option_get_id(option);

  return SVN_NO_ERROR;
}

/* Resolve a file/file "incoming add vs local obstruction" tree conflict by
 * replacing the local file with the incoming file. If MERGE_FILES is set,
 * also merge the files after replacing. */
static svn_error_t *
merge_incoming_added_file_replace(svn_client_conflict_option_t *option,
                                  svn_client_conflict_t *conflict,
                                  svn_boolean_t merge_files,
                                  apr_pool_t *scratch_pool)
{
  svn_ra_session_t *ra_session;
  const char *url;
  const char *corrected_url;
  const char *repos_root_url;
  const char *repos_uuid;
  const char *incoming_new_repos_relpath;
  svn_revnum_t incoming_new_pegrev;
  apr_file_t *incoming_new_file;
  svn_stream_t *incoming_new_stream;
  apr_hash_t *incoming_new_props;
  const char *local_abspath;
  const char *lock_abspath;
  svn_client_ctx_t *ctx = conflict->ctx;
  const char *wc_tmpdir;
  apr_file_t *working_file_tmp;
  svn_stream_t *working_file_tmp_stream;
  const char *working_file_tmp_abspath;
  svn_stream_t *working_file_stream;
  apr_hash_t *working_props;
  svn_error_t *err;

  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  /* Set up tempory storage for the working version of file. */
  SVN_ERR(svn_wc__get_tmpdir(&wc_tmpdir, ctx->wc_ctx, local_abspath,
                             scratch_pool, scratch_pool));
  SVN_ERR(svn_io_open_unique_file3(&working_file_tmp,
                                   &working_file_tmp_abspath, wc_tmpdir,
                                   svn_io_file_del_on_pool_cleanup,
                                   scratch_pool, scratch_pool));
  working_file_tmp_stream = svn_stream_from_aprfile2(working_file_tmp,
                                                     FALSE, scratch_pool);

  /* Copy the working file to temporary storage. */
  SVN_ERR(svn_stream_open_readonly(&working_file_stream, local_abspath,
                                   scratch_pool, scratch_pool));
  SVN_ERR(svn_stream_copy3(working_file_stream, working_file_tmp_stream,
                           ctx->cancel_baton, ctx->cancel_baton,
                           scratch_pool));
  SVN_ERR(svn_io_file_flush(working_file_tmp, scratch_pool));

  /* Get a copy of the working file's properties. */
  SVN_ERR(svn_wc_prop_list2(&working_props, ctx->wc_ctx, local_abspath,
                            scratch_pool, scratch_pool));

  /* Fetch the incoming added file from the repository. */
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &incoming_new_repos_relpath, &incoming_new_pegrev,
            NULL, conflict, scratch_pool,
            scratch_pool));
  SVN_ERR(svn_client_conflict_get_repos_info(&repos_root_url, &repos_uuid,
                                             conflict, scratch_pool,
                                             scratch_pool));
  url = svn_path_url_add_component2(repos_root_url, incoming_new_repos_relpath,
                                    scratch_pool);
  SVN_ERR(svn_client__open_ra_session_internal(&ra_session, &corrected_url,
                                               url, NULL, NULL, FALSE, FALSE,
                                               conflict->ctx, scratch_pool,
                                               scratch_pool));
  if (corrected_url)
    url = corrected_url;
  SVN_ERR(svn_io_open_unique_file3(&incoming_new_file, NULL, wc_tmpdir,
                                   svn_io_file_del_on_pool_cleanup,
                                   scratch_pool, scratch_pool));
  incoming_new_stream = svn_stream_from_aprfile2(incoming_new_file, TRUE,
                                                 scratch_pool);
  SVN_ERR(svn_ra_get_file(ra_session, "", incoming_new_pegrev,
                          incoming_new_stream, NULL, /* fetched_rev */
                          &incoming_new_props, scratch_pool));
  /* Flush file to disk. */
  SVN_ERR(svn_io_file_flush(incoming_new_file, scratch_pool));

  /* Reset the stream in preparation for adding its content to WC. */
  SVN_ERR(svn_stream_reset(incoming_new_stream));

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));

  /* ### The following WC modifications should be atomic. */

  /* Replace the working file with the file from the repository. */
  err = svn_wc_delete4(ctx->wc_ctx, local_abspath, FALSE, FALSE,
                       ctx->cancel_func, ctx->cancel_baton,
                       ctx->notify_func2, ctx->notify_baton2,
                       scratch_pool);
  if (err)
    goto unlock_wc;
  err = svn_wc_add_repos_file4(ctx->wc_ctx, local_abspath,
                               incoming_new_stream,
                               NULL, /* ### could we merge first, then set
                                        ### the merged content here? */
                               incoming_new_props,
                               NULL, /* ### merge props first, set here? */
                               url, incoming_new_pegrev,
                               ctx->cancel_func, ctx->cancel_baton,
                               scratch_pool);
  if (err)
    goto unlock_wc;

  if (ctx->notify_func2)
    {
      svn_wc_notify_t *notify = svn_wc_create_notify(local_abspath,
                                                     svn_wc_notify_add,
                                                     scratch_pool);
      notify->kind = svn_node_file;
      ctx->notify_func2(ctx->notify_baton2, notify, scratch_pool);
    }

  /* Resolve to current working copy state. svn_wc_merge5() requires this. */
  err = svn_wc__del_tree_conflict(ctx->wc_ctx, local_abspath, scratch_pool);
  if (err)
    goto unlock_wc;

  if (merge_files)
    {
      svn_wc_merge_outcome_t merge_content_outcome;
      svn_wc_notify_state_t merge_props_outcome;
      apr_file_t *empty_file;
      const char *empty_file_abspath;
      apr_array_header_t *propdiffs;
      apr_hash_index_t *hi;

      /* Create an empty file as fake "merge-base" for the two added files.
       * The files are not ancestrally related so this is the best we can do. */
      err = svn_io_open_unique_file3(&empty_file, &empty_file_abspath, NULL,
                                     svn_io_file_del_on_pool_cleanup,
                                     scratch_pool, scratch_pool);
      if (err)
        goto unlock_wc;

      /* Delete entry and wc props from the returned set of properties.. */
      for (hi = apr_hash_first(scratch_pool, incoming_new_props);
           hi != NULL;
           hi = apr_hash_next(hi))
        {
          const char *propname = apr_hash_this_key(hi);

          if (!svn_wc_is_normal_prop(propname))
            svn_hash_sets(incoming_new_props, propname, NULL);
        }

      /* Create a property diff for the files. */
      err = svn_prop_diffs(&propdiffs, incoming_new_props,
                           working_props, scratch_pool);
      if (err)
        goto unlock_wc;

      /* Perform the file merge. */
      err = svn_wc_merge5(&merge_content_outcome, &merge_props_outcome,
                          ctx->wc_ctx, empty_file_abspath,
                          working_file_tmp_abspath, local_abspath,
                          NULL, NULL, NULL, /* labels */
                          NULL, NULL, /* conflict versions */
                          FALSE, /* dry run */
                          NULL, NULL, /* diff3_cmd, merge_options */
                          NULL, propdiffs,
                          NULL, NULL, /* conflict func/baton */
                          ctx->cancel_func, ctx->cancel_baton,
                          scratch_pool);
      if (err)
        goto unlock_wc;

      if (ctx->notify_func2)
        {
          svn_wc_notify_t *notify = svn_wc_create_notify(
                                       local_abspath,
                                       svn_wc_notify_update_update,
                                       scratch_pool);

          if (merge_content_outcome == svn_wc_merge_conflict)
            notify->content_state = svn_wc_notify_state_conflicted;
          else
            notify->content_state = svn_wc_notify_state_merged;
          notify->prop_state = merge_props_outcome;
          notify->kind = svn_node_file;
          ctx->notify_func2(ctx->notify_baton2, notify, scratch_pool);
        }
    }

unlock_wc:
  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  svn_io_sleep_for_timestamps(local_abspath, scratch_pool);
  SVN_ERR(err);

  SVN_ERR(svn_stream_close(incoming_new_stream));

  if (ctx->notify_func2)
    {
      svn_wc_notify_t *notify = svn_wc_create_notify(
                                  local_abspath,
                                  svn_wc_notify_resolved_tree,
                                  scratch_pool);

      ctx->notify_func2(ctx->notify_baton2, notify, scratch_pool);
    }

  conflict->resolution_tree = svn_client_conflict_option_get_id(option);

  return SVN_NO_ERROR;
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_merge_incoming_added_file_replace(
  svn_client_conflict_option_t *option,
  svn_client_conflict_t *conflict,
  apr_pool_t *scratch_pool)
{
  return svn_error_trace(merge_incoming_added_file_replace(option,
                                                           conflict,
                                                           FALSE,
                                                           scratch_pool));
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_merge_incoming_added_file_replace_and_merge(
  svn_client_conflict_option_t *option,
  svn_client_conflict_t *conflict,
  apr_pool_t *scratch_pool)
{
  return svn_error_trace(merge_incoming_added_file_replace(option,
                                                           conflict,
                                                           TRUE,
                                                           scratch_pool));
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_merge_incoming_added_dir_merge(svn_client_conflict_option_t *option,
                                       svn_client_conflict_t *conflict,
                                       apr_pool_t *scratch_pool)
{
  const char *repos_root_url;
  const char *repos_uuid;
  const char *incoming_old_repos_relpath;
  svn_revnum_t incoming_old_pegrev;
  const char *incoming_new_repos_relpath;
  svn_revnum_t incoming_new_pegrev;
  const char *local_abspath;
  const char *lock_abspath;
  svn_client_ctx_t *ctx = conflict->ctx;
  struct conflict_tree_incoming_add_details *details;
  svn_client__conflict_report_t *conflict_report;
  const char *source1;
  svn_opt_revision_t revision1;
  const char *source2;
  svn_opt_revision_t revision2;
  svn_error_t *err;

  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  details = conflict->tree_conflict_incoming_details;
  if (details == NULL)
    return svn_error_createf(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE, NULL,
                             _("Conflict resolution option '%d' requires "
                               "details for tree conflict at '%s' to be "
                               "fetched from the repository"),
                            option->id,
                            svn_dirent_local_style(local_abspath,
                                                   scratch_pool));

  /* Set up merge sources to merge the entire incoming added directory tree. */
  SVN_ERR(svn_client_conflict_get_repos_info(&repos_root_url, &repos_uuid,
                                             conflict, scratch_pool,
                                             scratch_pool));
  source1 = svn_path_url_add_component2(repos_root_url,
                                        details->repos_relpath,
                                        scratch_pool);
  revision1.kind = svn_opt_revision_number;
  SVN_ERR(svn_client_conflict_get_incoming_old_repos_location(
            &incoming_old_repos_relpath, &incoming_old_pegrev,
            NULL, conflict, scratch_pool, scratch_pool));
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &incoming_new_repos_relpath, &incoming_new_pegrev,
            NULL, conflict, scratch_pool, scratch_pool));
  if (incoming_old_pegrev < incoming_new_pegrev) /* forward merge */
    {
      if (details->added_rev == SVN_INVALID_REVNUM)
        return svn_error_createf(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE, NULL,
                                 _("Could not determine when '%s' was "
                                   "added the repository"),
                                 svn_dirent_local_style(local_abspath,
                                                        scratch_pool));
      revision1.value.number = details->added_rev;

      source2 = svn_path_url_add_component2(repos_root_url,
                                            incoming_new_repos_relpath,
                                            scratch_pool);
      revision2.kind = svn_opt_revision_number;
      revision2.value.number = incoming_new_pegrev;
    }
  else /* reverse-merge */
    {
      if (details->deleted_rev == SVN_INVALID_REVNUM)
        return svn_error_createf(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE, NULL,
                                 _("Could not determine when '%s' was "
                                   "deleted from the repository"),
                                 svn_dirent_local_style(local_abspath,
                                                        scratch_pool));
      revision1.value.number = details->deleted_rev;

      source2 = svn_path_url_add_component2(repos_root_url,
                                            incoming_old_repos_relpath,
                                            scratch_pool);
      revision2.kind = svn_opt_revision_number;
      revision2.value.number = incoming_old_pegrev;
    }

  /* ### The following WC modifications should be atomic. */
  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));

  /* Resolve to current working copy state. The merge requires this. */
  err = svn_wc__del_tree_conflict(ctx->wc_ctx, local_abspath, scratch_pool);
  if (err)
    return svn_error_compose_create(err,
                                    svn_wc__release_write_lock(ctx->wc_ctx,
                                                               lock_abspath,
                                                               scratch_pool));

  /* ### Should we do anything about mergeinfo? We need to run a no-ancestry
   * ### merge to get a useful result because mergeinfo-aware merges may split
   * ### this merge into several ranges and then abort early as soon as a
   * ### conflict occurs (which will happen invariably when merging unrelated
   * ### trees). The original merge which raised the tree conflict in the
   * ### first place created mergeinfo which also describes this merge,
   * ### unless 1) the working copy's mergeinfo was changed since, or
   * ### 2) the newly added directory's history has location segments with
   * ### paths outside the original merge source's natural history's path
   * ### (see the test_option_merge_incoming_added_dir_merge3() test). */
  err = svn_client__merge_locked(&conflict_report,
                                 source1, &revision1,
                                 source2, &revision2,
                                 local_abspath, svn_depth_infinity,
                                 TRUE, TRUE, /* do a no-ancestry merge */
                                 FALSE, FALSE, FALSE,
                                 TRUE, /* Allow mixed-rev just in case, since
                                        * conflict victims can't be updated to
                                        * straighten out mixed-rev trees. */
                                 NULL, ctx, scratch_pool, scratch_pool);

  err = svn_error_compose_create(err,
                                 svn_client__make_merge_conflict_error(
                                   conflict_report, scratch_pool));
  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  svn_io_sleep_for_timestamps(local_abspath, scratch_pool);
  SVN_ERR(err);

  if (ctx->notify_func2)
    ctx->notify_func2(ctx->notify_baton2,
                      svn_wc_create_notify(local_abspath,
                                           svn_wc_notify_resolved_tree,
                                           scratch_pool),
                      scratch_pool);

  conflict->resolution_tree = svn_client_conflict_option_get_id(option);

  return SVN_NO_ERROR;
}

/* A baton for notification_adjust_func(). */
struct notification_adjust_baton
{
  svn_wc_notify_func2_t inner_func;
  void *inner_baton;
  const char *checkout_abspath;
  const char *final_abspath;
};

/* A svn_wc_notify_func2_t function that wraps BATON->inner_func (whose
 * baton is BATON->inner_baton) and adjusts the notification paths that
 * start with BATON->checkout_abspath to start instead with
 * BATON->final_abspath. */
static void
notification_adjust_func(void *baton,
                         const svn_wc_notify_t *notify,
                         apr_pool_t *pool)
{
  struct notification_adjust_baton *nb = baton;
  svn_wc_notify_t *inner_notify = svn_wc_dup_notify(notify, pool);
  const char *relpath;

  relpath = svn_dirent_skip_ancestor(nb->checkout_abspath, notify->path);
  inner_notify->path = svn_dirent_join(nb->final_abspath, relpath, pool);

  if (nb->inner_func)
    nb->inner_func(nb->inner_baton, inner_notify, pool);
}

/* Resolve a dir/dir "incoming add vs local obstruction" tree conflict by
 * replacing the local directory with the incoming directory.
 * If MERGE_DIRS is set, also merge the directories after replacing. */
static svn_error_t *
merge_incoming_added_dir_replace(svn_client_conflict_option_t *option,
                                 svn_client_conflict_t *conflict,
                                 svn_boolean_t merge_dirs,
                                 apr_pool_t *scratch_pool)
{
  svn_ra_session_t *ra_session;
  const char *url;
  const char *corrected_url;
  const char *repos_root_url;
  const char *repos_uuid;
  const char *incoming_new_repos_relpath;
  svn_revnum_t incoming_new_pegrev;
  const char *local_abspath;
  const char *lock_abspath;
  svn_client_ctx_t *ctx = conflict->ctx;
  const char *tmpdir_abspath, *tmp_abspath;
  svn_error_t *err;
  svn_revnum_t copy_src_revnum;
  svn_opt_revision_t copy_src_peg_revision;
  svn_boolean_t timestamp_sleep;
  svn_wc_notify_func2_t old_notify_func2 = ctx->notify_func2;
  void *old_notify_baton2 = ctx->notify_baton2;
  struct notification_adjust_baton nb;

  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  /* Find the URL of the incoming added directory in the repository. */
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &incoming_new_repos_relpath, &incoming_new_pegrev,
            NULL, conflict, scratch_pool,
            scratch_pool));
  SVN_ERR(svn_client_conflict_get_repos_info(&repos_root_url, &repos_uuid,
                                             conflict, scratch_pool,
                                             scratch_pool));
  url = svn_path_url_add_component2(repos_root_url, incoming_new_repos_relpath,
                                    scratch_pool);
  SVN_ERR(svn_client__open_ra_session_internal(&ra_session, &corrected_url,
                                               url, NULL, NULL, FALSE, FALSE,
                                               conflict->ctx, scratch_pool,
                                               scratch_pool));
  if (corrected_url)
    url = corrected_url;


  /* Find a temporary location in which to check out the copy source. */
  SVN_ERR(svn_wc__get_tmpdir(&tmpdir_abspath, ctx->wc_ctx, local_abspath,
                             scratch_pool, scratch_pool));

  SVN_ERR(svn_io_open_unique_file3(NULL, &tmp_abspath, tmpdir_abspath,
                                   svn_io_file_del_on_close,
                                   scratch_pool, scratch_pool));

  /* Make a new checkout of the requested source. While doing so,
   * resolve copy_src_revnum to an actual revision number in case it
   * was until now 'invalid' meaning 'head'.  Ask this function not to
   * sleep for timestamps, by passing a sleep_needed output param.
   * Send notifications for all nodes except the root node, and adjust
   * them to refer to the destination rather than this temporary path. */

  nb.inner_func = ctx->notify_func2;
  nb.inner_baton = ctx->notify_baton2;
  nb.checkout_abspath = tmp_abspath;
  nb.final_abspath = local_abspath;
  ctx->notify_func2 = notification_adjust_func;
  ctx->notify_baton2 = &nb;

  copy_src_peg_revision.kind = svn_opt_revision_number;
  copy_src_peg_revision.value.number = incoming_new_pegrev;

  err = svn_client__checkout_internal(&copy_src_revnum, &timestamp_sleep,
                                      url, tmp_abspath,
                                      &copy_src_peg_revision,
                                      &copy_src_peg_revision,
                                      svn_depth_infinity,
                                      TRUE, /* we want to ignore externals */
                                      FALSE, /* we don't allow obstructions */
                                      ra_session, ctx, scratch_pool);

  ctx->notify_func2 = old_notify_func2;
  ctx->notify_baton2 = old_notify_baton2;

  SVN_ERR(err);

  /* ### The following WC modifications should be atomic. */

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 svn_dirent_dirname(
                                                   local_abspath,
                                                   scratch_pool),
                                                 scratch_pool, scratch_pool));

  /* Remove the working directory. */
  err = svn_wc_delete4(ctx->wc_ctx, local_abspath, FALSE, FALSE,
                       ctx->cancel_func, ctx->cancel_baton,
                       ctx->notify_func2, ctx->notify_baton2,
                       scratch_pool);
  if (err)
    goto unlock_wc;

  /* Schedule dst_path for addition in parent, with copy history.
     Don't send any notification here.
     Then remove the temporary checkout's .svn dir in preparation for
     moving the rest of it into the final destination. */
  err = svn_wc_copy3(ctx->wc_ctx, tmp_abspath, local_abspath,
                     TRUE /* metadata_only */,
                     ctx->cancel_func, ctx->cancel_baton,
                     NULL, NULL, scratch_pool);
  if (err)
    goto unlock_wc;

  err = svn_wc__acquire_write_lock(NULL, ctx->wc_ctx, tmp_abspath,
                                   FALSE, scratch_pool, scratch_pool);
  if (err)
    goto unlock_wc;
  err = svn_wc_remove_from_revision_control2(ctx->wc_ctx,
                                             tmp_abspath,
                                             FALSE, FALSE,
                                             ctx->cancel_func,
                                             ctx->cancel_baton,
                                             scratch_pool);
  if (err)
    goto unlock_wc;

  /* Move the temporary disk tree into place. */
  err = svn_io_file_rename2(tmp_abspath, local_abspath, FALSE, scratch_pool);
  if (err)
    goto unlock_wc;

  if (ctx->notify_func2)
    {
      svn_wc_notify_t *notify = svn_wc_create_notify(local_abspath,
                                                     svn_wc_notify_add,
                                                     scratch_pool);
      notify->kind = svn_node_dir;
      ctx->notify_func2(ctx->notify_baton2, notify, scratch_pool);
    }

  /* Resolve to current working copy state.
  * svn_client__merge_locked() requires this. */
  err = svn_wc__del_tree_conflict(ctx->wc_ctx, local_abspath, scratch_pool);
  if (err)
    goto unlock_wc;

  if (merge_dirs)
    {
      svn_client__conflict_report_t *conflict_report;
      const char *source1;
      svn_opt_revision_t revision1;
      const char *source2;
      svn_opt_revision_t revision2;
      svn_revnum_t base_revision;
      const char *base_repos_relpath;
      struct find_added_rev_baton b;

      /* Find the URL and revision of the directory we have just replaced. */
      err = svn_wc__node_get_base(NULL, &base_revision, &base_repos_relpath,
                                  NULL, NULL, NULL, ctx->wc_ctx, local_abspath,
                                  FALSE, scratch_pool, scratch_pool);
      if (err)
        goto unlock_wc;

      url = svn_path_url_add_component2(repos_root_url, base_repos_relpath,
                                        scratch_pool);

      /* Trace the replaced directory's history to its origin. */
      err = svn_ra_reparent(ra_session, url, scratch_pool);
      if (err)
        goto unlock_wc;
      b.added_rev = SVN_INVALID_REVNUM;
      b.repos_relpath = NULL;
      b.parent_repos_relpath = svn_relpath_dirname(base_repos_relpath,
                                                   scratch_pool);
      b.pool = scratch_pool;
      err = svn_ra_get_location_segments(ra_session, "", base_revision,
                                         base_revision, SVN_INVALID_REVNUM,
                                         find_added_rev, &b,
                                         scratch_pool);
      if (err)
        goto unlock_wc;

      if (b.added_rev == SVN_INVALID_REVNUM)
        {
          err = svn_error_createf(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE, NULL,
                                  _("Could not determine the revision in "
                                    "which '^/%s' was added to the "
                                    "repository.\n"),
                                  base_repos_relpath);
          goto unlock_wc;
        }

      /* Merge the replaced directory into the directory which replaced it.
       * We do not need to consider a reverse-merge here since the source of
       * this merge was part of the merge target working copy, not a branch
       * in the repository. */
      source1 = url;
      revision1.kind = svn_opt_revision_number;
      /* ### Our merge logic doesn't support the merge -c ADDED_REV case.
       * ### It errors out with 'path not found', unlike diff -c ADDED_REV. */
      if (b.added_rev == base_revision)
        revision1.value.number = b.added_rev - 1; /* merge -c ADDED_REV */
      else
        revision1.value.number = b.added_rev; /* merge -r ADDED_REV:BASE_REV */
      source2 = url;
      revision2.kind = svn_opt_revision_number;
      revision2.value.number = base_revision;
      
      err = svn_client__merge_locked(&conflict_report,
                                     source1, &revision1,
                                     source2, &revision2,
                                     local_abspath, svn_depth_infinity,
                                     TRUE, TRUE, /* do a no-ancestry merge */
                                     FALSE, FALSE, FALSE,
                                     FALSE, /* no need to allow mixed-rev */
                                     NULL, ctx, scratch_pool, scratch_pool);
      err = svn_error_compose_create(err,
                                     svn_client__make_merge_conflict_error(
                                       conflict_report, scratch_pool));
    }

unlock_wc:
  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  svn_io_sleep_for_timestamps(local_abspath, scratch_pool);
  SVN_ERR(err);

  if (ctx->notify_func2)
    {
      svn_wc_notify_t *notify = svn_wc_create_notify(
                                  local_abspath,
                                  svn_wc_notify_resolved_tree,
                                  scratch_pool);

      ctx->notify_func2(ctx->notify_baton2, notify, scratch_pool);
    }

  conflict->resolution_tree = svn_client_conflict_option_get_id(option);

  return SVN_NO_ERROR;
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_merge_incoming_added_dir_replace(svn_client_conflict_option_t *option,
                                         svn_client_conflict_t *conflict,
                                         apr_pool_t *scratch_pool)
{
  return svn_error_trace(merge_incoming_added_dir_replace(option,
                                                          conflict,
                                                          FALSE,
                                                          scratch_pool));
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_merge_incoming_added_dir_replace_and_merge(
  svn_client_conflict_option_t *option,
  svn_client_conflict_t *conflict,
  apr_pool_t *scratch_pool)
{
  return svn_error_trace(merge_incoming_added_dir_replace(option,
                                                          conflict,
                                                          TRUE,
                                                          scratch_pool));
}

/* Verify the local working copy state matches what we expect when an
 * incoming deletion tree conflict exists.
 * We assume update/merge/switch operations leave the working copy in a
 * state which prefers the local change and cancels the deletion.
 * Run a quick sanity check and error out if it looks as if the
 * working copy was modified since, even though it's not easy to make
 * such modifications without also clearing the conflict marker. */
static svn_error_t *
verify_local_state_for_incoming_delete(svn_client_conflict_t *conflict,
                                       svn_client_conflict_option_t *option,
                                       apr_pool_t *scratch_pool)
{
  const char *local_abspath;
  const char *wcroot_abspath;
  svn_wc_operation_t operation;
  svn_client_ctx_t *ctx = conflict->ctx;

  local_abspath = svn_client_conflict_get_local_abspath(conflict);
  SVN_ERR(svn_wc__get_wcroot(&wcroot_abspath, ctx->wc_ctx,
                             local_abspath, scratch_pool,
                             scratch_pool));
  operation = svn_client_conflict_get_operation(conflict);

  if (operation == svn_wc_operation_update ||
      operation == svn_wc_operation_switch)
    {
      struct conflict_tree_incoming_delete_details *details;
      svn_boolean_t is_copy;
      svn_revnum_t copyfrom_rev;
      const char *copyfrom_repos_relpath;

      details = conflict->tree_conflict_incoming_details;
      if (details == NULL)
        return svn_error_createf(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE, NULL,
                                 _("Conflict resolution option '%d' requires "
                                   "details for tree conflict at '%s' to be "
                                   "fetched from the repository."),
                                option->id,
                                svn_dirent_local_style(local_abspath,
                                                       scratch_pool));

      /* Ensure that the item is a copy of itself from before it was deleted.
       * Update and switch are supposed to set this up when flagging the
       * conflict. */
      SVN_ERR(svn_wc__node_get_origin(&is_copy, &copyfrom_rev,
                                      &copyfrom_repos_relpath,
                                      NULL, NULL, NULL, NULL,
                                      ctx->wc_ctx, local_abspath, FALSE,
                                      scratch_pool, scratch_pool));
      if (!is_copy)
        return svn_error_createf(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE, NULL,
                                 _("Cannot resolve tree conflict on '%s' by "
                                   "ignoring the incoming deletion "
                                   "(expected a copied item, but the item "
                                   "is not a copy)"),
                                 svn_dirent_local_style(
                                   svn_dirent_skip_ancestor(
                                     wcroot_abspath,
                                     conflict->local_abspath),
                                 scratch_pool));
      else if (details->deleted_rev == SVN_INVALID_REVNUM &&
               details->added_rev == SVN_INVALID_REVNUM)
        return svn_error_createf(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE, NULL,
                                 _("Could not find the revision in which '%s' "
                                   "was deleted from the repository"),
                                 svn_dirent_local_style(
                                   svn_dirent_skip_ancestor(
                                     wcroot_abspath,
                                     conflict->local_abspath),
                                   scratch_pool));
      else if (details->deleted_rev != SVN_INVALID_REVNUM &&
               copyfrom_rev >= details->deleted_rev)
        return svn_error_createf(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE, NULL,
                                 _("Cannot resolve tree conflict on '%s' by "
                                   "ignoring the incoming deletion (expected "
                                   "an item copied from a revision smaller "
                                   "than r%ld, but the item was copied from "
                                   "r%ld)"),
                                 svn_dirent_local_style(
                                   svn_dirent_skip_ancestor(
                                     wcroot_abspath, conflict->local_abspath),
                                   scratch_pool),
                                 details->deleted_rev, copyfrom_rev);

      else if (details->added_rev != SVN_INVALID_REVNUM &&
               copyfrom_rev < details->added_rev)
        return svn_error_createf(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE, NULL,
                                 _("Cannot resolve tree conflict on '%s' by "
                                   "ignoring the incoming deletion (expected "
                                   "an item copied from a revision larger than "
                                   "r%ld, but the item was copied from r%ld)"),
                                 svn_dirent_local_style(
                                   svn_dirent_skip_ancestor(
                                     wcroot_abspath, conflict->local_abspath),
                                   scratch_pool),
                                  details->added_rev, copyfrom_rev);

      else if (operation == svn_wc_operation_update &&
               strcmp(copyfrom_repos_relpath, details->repos_relpath) != 0)
        return svn_error_createf(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE, NULL,
                                 _("Cannot resolve tree conflict on '%s' by "
                                   "ignoring the incoming deletion (expected "
                                   "an item copied from '^/%s', but the item "
                                   "was copied from '^/%s@%ld')"),
                                 svn_dirent_local_style(
                                   svn_dirent_skip_ancestor(
                                     wcroot_abspath, conflict->local_abspath),
                                   scratch_pool),
                                 details->repos_relpath,
                                 copyfrom_repos_relpath, copyfrom_rev);
      else if (operation == svn_wc_operation_switch)
        {
          const char *old_repos_relpath;

          SVN_ERR(svn_client_conflict_get_incoming_old_repos_location(
                    &old_repos_relpath, NULL, NULL, conflict,
                    scratch_pool, scratch_pool));

          if (strcmp(copyfrom_repos_relpath, old_repos_relpath) != 0)
            return svn_error_createf(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE, NULL,
                                     _("Cannot resolve tree conflict on '%s' "
                                       "by ignoring the incoming deletion "
                                       "(expected an item copied from '^/%s', "
                                       "but the item was copied from "
                                        "'^/%s@%ld')"),
                                     svn_dirent_local_style(
                                       svn_dirent_skip_ancestor(
                                         wcroot_abspath,
                                         conflict->local_abspath),
                                       scratch_pool),
                                     old_repos_relpath,
                                     copyfrom_repos_relpath, copyfrom_rev);
        }
    }
  else if (operation == svn_wc_operation_merge)
    {
      svn_node_kind_t victim_node_kind;
      svn_node_kind_t on_disk_kind;

      /* For merge, all we can do is ensure that the item still exists. */
      victim_node_kind =
        svn_client_conflict_tree_get_victim_node_kind(conflict);
      SVN_ERR(svn_io_check_path(local_abspath, &on_disk_kind, scratch_pool));

      if (victim_node_kind != on_disk_kind)
        return svn_error_createf(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE, NULL,
                                 _("Cannot resolve tree conflict on '%s' by "
                                   "ignoring the incoming deletion (expected "
                                   "node kind '%s' but found '%s')"),
                                 svn_dirent_local_style(
                                   svn_dirent_skip_ancestor(
                                     wcroot_abspath, conflict->local_abspath),
                                   scratch_pool),
                                 svn_node_kind_to_word(victim_node_kind),
                                 svn_node_kind_to_word(on_disk_kind));
    }

  return SVN_NO_ERROR;
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_incoming_delete_ignore(svn_client_conflict_option_t *option,
                               svn_client_conflict_t *conflict,
                               apr_pool_t *scratch_pool)
{
  svn_client_conflict_option_id_t option_id;
  const char *local_abspath;
  const char *lock_abspath;
  svn_client_ctx_t *ctx = conflict->ctx;
  svn_error_t *err;

  option_id = svn_client_conflict_option_get_id(option);
  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));

  err = verify_local_state_for_incoming_delete(conflict, option, scratch_pool);
  if (err)
    goto unlock_wc;

  /* Resolve to the current working copy state. */
  err = svn_wc__del_tree_conflict(ctx->wc_ctx, local_abspath, scratch_pool);

  /* svn_wc__del_tree_conflict doesn't handle notification for us */
  if (ctx->notify_func2)
    ctx->notify_func2(ctx->notify_baton2,
                      svn_wc_create_notify(local_abspath,
                                           svn_wc_notify_resolved_tree,
                                           scratch_pool),
                      scratch_pool);

unlock_wc:
  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  SVN_ERR(err);

  conflict->resolution_tree = option_id;

  return SVN_NO_ERROR;
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_incoming_delete_accept(svn_client_conflict_option_t *option,
                               svn_client_conflict_t *conflict,
                               apr_pool_t *scratch_pool)
{
  svn_client_conflict_option_id_t option_id;
  const char *local_abspath;
  const char *lock_abspath;
  svn_client_ctx_t *ctx = conflict->ctx;
  svn_error_t *err;

  option_id = svn_client_conflict_option_get_id(option);
  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));

  err = verify_local_state_for_incoming_delete(conflict, option, scratch_pool);
  if (err)
    goto unlock_wc;

  /* Delete the tree conflict victim. Marks the conflict resolved. */
  err = svn_wc_delete4(ctx->wc_ctx, local_abspath, FALSE, FALSE,
                       ctx->cancel_func, ctx->cancel_baton,
                       ctx->notify_func2, ctx->notify_baton2,
                       scratch_pool);
  if (err)
    goto unlock_wc;

  if (ctx->notify_func2)
    ctx->notify_func2(ctx->notify_baton2,
                      svn_wc_create_notify(local_abspath,
                                           svn_wc_notify_resolved_tree,
                                           scratch_pool),
                      scratch_pool);

unlock_wc:
  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  SVN_ERR(err);

  conflict->resolution_tree = option_id;

  return SVN_NO_ERROR;
}

/* Resolver options for a text conflict */
static const svn_client_conflict_option_t text_conflict_options[] =
{
  {
    svn_client_conflict_option_postpone,
    N_("skip this conflict and leave it unresolved"),
    NULL,
    resolve_postpone
  },

  {
    svn_client_conflict_option_base_text,
    N_("discard local and incoming changes for this file"),
    NULL,
    resolve_text_conflict
  },

  {
    svn_client_conflict_option_incoming_text,
    N_("accept incoming version of entire file"),
    NULL,
    resolve_text_conflict
  },

  {
    svn_client_conflict_option_working_text,
    N_("reject all incoming changes for this file"),
    NULL,
    resolve_text_conflict
  },

  {
    svn_client_conflict_option_incoming_text_where_conflicted,
    N_("accept changes only where they conflict"),
    NULL,
    resolve_text_conflict
  },

  {
    svn_client_conflict_option_working_text_where_conflicted,
    N_("reject changes which conflict and accept the rest"),
    NULL,
    resolve_text_conflict
  },

  {
    svn_client_conflict_option_merged_text,
    N_("accept the file as it appears in the working copy"),
    NULL,
    resolve_text_conflict
  },

};

/* Resolver options for a binary file conflict */
static const svn_client_conflict_option_t binary_conflict_options[] =
{
  {
    svn_client_conflict_option_postpone,
    N_("skip this conflict and leave it unresolved"),
    NULL,
    resolve_postpone,
  },

  {
    svn_client_conflict_option_incoming_text,
    N_("accept incoming version of binary file"),
    NULL,
    resolve_text_conflict
  },

  {
    svn_client_conflict_option_working_text,
    N_("accept working copy version of binary file"),
    NULL,
    resolve_text_conflict
  },

  {
    svn_client_conflict_option_merged_text,
    N_("accept the file as it appears in the working copy"),
    NULL,
    resolve_text_conflict
  },

};

/* Resolver options for a property conflict */
static const svn_client_conflict_option_t prop_conflict_options[] =
{
  {
    svn_client_conflict_option_postpone,
    N_("skip this conflict and leave it unresolved"),
    NULL,
    resolve_postpone
  },

  {
    svn_client_conflict_option_base_text,
    N_("discard local and incoming changes for this property"),
    NULL,
    resolve_prop_conflict
  },

  {
    svn_client_conflict_option_incoming_text,
    N_("accept incoming version of entire property value"),
    NULL,
    resolve_prop_conflict
  },

  {
    svn_client_conflict_option_working_text,
    N_("accept working copy version of entire property value"),
    NULL,
    resolve_prop_conflict
  },

  {
    svn_client_conflict_option_incoming_text_where_conflicted,
    N_("accept changes only where they conflict"),
    NULL,
    resolve_prop_conflict
  },

  {
    svn_client_conflict_option_working_text_where_conflicted,
    N_("reject changes which conflict and accept the rest"),
    NULL,
    resolve_prop_conflict
  },

  {
    svn_client_conflict_option_merged_text,
    N_("accept merged version of property value"),
    NULL,
    resolve_prop_conflict
  },

};

static svn_error_t *
assert_text_conflict(svn_client_conflict_t *conflict, apr_pool_t *scratch_pool)
{
  svn_boolean_t text_conflicted;

  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted, NULL, NULL,
                                             conflict, scratch_pool,
                                             scratch_pool));

  SVN_ERR_ASSERT(text_conflicted); /* ### return proper error? */

  return SVN_NO_ERROR;
}

static svn_error_t *
assert_prop_conflict(svn_client_conflict_t *conflict, apr_pool_t *scratch_pool)
{
  apr_array_header_t *props_conflicted;

  SVN_ERR(svn_client_conflict_get_conflicted(NULL, &props_conflicted, NULL,
                                             conflict, scratch_pool,
                                             scratch_pool));

  /* ### return proper error? */
  SVN_ERR_ASSERT(props_conflicted && props_conflicted->nelts > 0);

  return SVN_NO_ERROR;
}

static svn_error_t *
assert_tree_conflict(svn_client_conflict_t *conflict, apr_pool_t *scratch_pool)
{
  svn_boolean_t tree_conflicted;

  SVN_ERR(svn_client_conflict_get_conflicted(NULL, NULL, &tree_conflicted,
                                             conflict, scratch_pool,
                                             scratch_pool));

  SVN_ERR_ASSERT(tree_conflicted); /* ### return proper error? */

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_text_get_resolution_options(apr_array_header_t **options,
                                                svn_client_conflict_t *conflict,
                                                apr_pool_t *result_pool,
                                                apr_pool_t *scratch_pool)
{
  const char *mime_type;
  int i;

  SVN_ERR(assert_text_conflict(conflict, scratch_pool));

  *options = apr_array_make(result_pool, ARRAY_LEN(text_conflict_options),
                            sizeof(svn_client_conflict_option_t *));

  mime_type = svn_client_conflict_text_get_mime_type(conflict);
  if (mime_type && svn_mime_type_is_binary(mime_type))
    {
      for (i = 0; i < ARRAY_LEN(binary_conflict_options); i++)
        {
          svn_client_conflict_option_t *option;

          /* We must make a copy to make the memory for option->type_data
           * writable and to localize the description. */
          option = apr_pcalloc(result_pool, sizeof(*option));
          *option = binary_conflict_options[i];
          option->description = _(option->description);
          APR_ARRAY_PUSH((*options), const svn_client_conflict_option_t *) =
            option;
        }
    }
  else
    {
      for (i = 0; i < ARRAY_LEN(text_conflict_options); i++)
        {
          svn_client_conflict_option_t *option;

          /* We must make a copy to make the memory for option->type_data
           * writable and to localize the description. */
          option = apr_pcalloc(result_pool, sizeof(*option));
          *option = text_conflict_options[i];
          option->description = _(option->description);
          APR_ARRAY_PUSH((*options), const svn_client_conflict_option_t *) =
            option;
        }
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_prop_get_resolution_options(apr_array_header_t **options,
                                                svn_client_conflict_t *conflict,
                                                apr_pool_t *result_pool,
                                                apr_pool_t *scratch_pool)
{
  int i;

  SVN_ERR(assert_prop_conflict(conflict, scratch_pool));

  *options = apr_array_make(result_pool, ARRAY_LEN(prop_conflict_options),
                            sizeof(svn_client_conflict_option_t *));
  for (i = 0; i < ARRAY_LEN(prop_conflict_options); i++)
    {
      svn_client_conflict_option_t *option;

      /* We must make a copy to make the memory for option->type_data
       * writable and to localize the description. */
      option = apr_pcalloc(result_pool, sizeof(*option));
      *option = prop_conflict_options[i];
      option->description = _(option->description);
      APR_ARRAY_PUSH((*options), const svn_client_conflict_option_t *) = option;
    }

  return SVN_NO_ERROR;
}

/* Configure 'accept current wc state' resolution option for a tree conflict. */
static svn_error_t *
configure_option_accept_current_wc_state(svn_client_conflict_t *conflict,
                                         apr_array_header_t *options)
{
  svn_wc_operation_t operation;
  svn_wc_conflict_action_t incoming_change;
  svn_wc_conflict_reason_t local_change;
  svn_client_conflict_option_t *option;

  operation = svn_client_conflict_get_operation(conflict);
  incoming_change = svn_client_conflict_get_incoming_change(conflict);
  local_change = svn_client_conflict_get_local_change(conflict);

  option = apr_pcalloc(options->pool, sizeof(*option));
  option->id = svn_client_conflict_option_accept_current_wc_state;
  option->description = _("accept current working copy state");
  option->conflict = conflict;
  if ((operation == svn_wc_operation_update ||
       operation == svn_wc_operation_switch) &&
      (local_change == svn_wc_conflict_reason_moved_away ||
       local_change == svn_wc_conflict_reason_deleted ||
       local_change == svn_wc_conflict_reason_replaced) &&
      incoming_change == svn_wc_conflict_action_edit)
    {
      /* We must break moves if the user accepts the current working copy
       * state instead of updating a moved-away node or updating children
       * moved outside of deleted or replaced directory nodes.
       * Else such moves would be left in an invalid state. */
      option->do_resolve_func = resolve_update_break_moved_away;
    }
  else
    option->do_resolve_func = resolve_accept_current_wc_state;

  APR_ARRAY_PUSH(options, const svn_client_conflict_option_t *) = option;

  return SVN_NO_ERROR;
}

/* Configure 'update move destination' resolution option for a tree conflict. */
static svn_error_t *
configure_option_update_move_destination(svn_client_conflict_t *conflict,
                                         apr_array_header_t *options)
{
  svn_wc_operation_t operation;
  svn_wc_conflict_action_t incoming_change;
  svn_wc_conflict_reason_t local_change;

  operation = svn_client_conflict_get_operation(conflict);
  incoming_change = svn_client_conflict_get_incoming_change(conflict);
  local_change = svn_client_conflict_get_local_change(conflict);

  if ((operation == svn_wc_operation_update ||
       operation == svn_wc_operation_switch) &&
      incoming_change == svn_wc_conflict_action_edit &&
      local_change == svn_wc_conflict_reason_moved_away)
    {
      svn_client_conflict_option_t *option;

      option = apr_pcalloc(options->pool, sizeof(*option));
      option->id = svn_client_conflict_option_update_move_destination;
      option->description = _("apply incoming changes to move destination");
      option->conflict = conflict;
      option->do_resolve_func = resolve_update_moved_away_node;
      APR_ARRAY_PUSH(options, const svn_client_conflict_option_t *) = option;
    }

  return SVN_NO_ERROR;
}

/* Configure 'update raise moved away children' resolution option for a tree
 * conflict. */
static svn_error_t *
configure_option_update_raise_moved_away_children(
  svn_client_conflict_t *conflict,
  apr_array_header_t *options)
{
  svn_wc_operation_t operation;
  svn_wc_conflict_action_t incoming_change;
  svn_wc_conflict_reason_t local_change;
  svn_node_kind_t victim_node_kind;

  operation = svn_client_conflict_get_operation(conflict);
  incoming_change = svn_client_conflict_get_incoming_change(conflict);
  local_change = svn_client_conflict_get_local_change(conflict);
  victim_node_kind = svn_client_conflict_tree_get_victim_node_kind(conflict);

  if ((operation == svn_wc_operation_update ||
       operation == svn_wc_operation_switch) &&
      incoming_change == svn_wc_conflict_action_edit &&
      (local_change == svn_wc_conflict_reason_deleted ||
       local_change == svn_wc_conflict_reason_replaced) &&
      victim_node_kind == svn_node_dir)
    {
      svn_client_conflict_option_t *option;

      option = apr_pcalloc(options->pool, sizeof(*option));
      option->id = svn_client_conflict_option_update_any_moved_away_children;
      option->description = _("prepare for updating moved-away children, "
                              "if any");
      option->conflict = conflict;
      option->do_resolve_func = resolve_update_raise_moved_away;
      APR_ARRAY_PUSH(options, const svn_client_conflict_option_t *) = option;
    }

  return SVN_NO_ERROR;
}

/* Configure 'incoming add ignore' resolution option for a tree conflict. */
static svn_error_t *
configure_option_merge_incoming_add_ignore(svn_client_conflict_t *conflict,
                                           apr_array_header_t *options,
                                           apr_pool_t *scratch_pool)
{
  svn_wc_operation_t operation;
  svn_wc_conflict_action_t incoming_change;
  svn_wc_conflict_reason_t local_change;
  const char *incoming_new_repos_relpath;
  svn_revnum_t incoming_new_pegrev;

  operation = svn_client_conflict_get_operation(conflict);
  incoming_change = svn_client_conflict_get_incoming_change(conflict);
  local_change = svn_client_conflict_get_local_change(conflict);
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &incoming_new_repos_relpath, &incoming_new_pegrev,
            NULL, conflict, scratch_pool,
            scratch_pool));

  if (operation == svn_wc_operation_merge &&
      incoming_change == svn_wc_conflict_action_add &&
      local_change == svn_wc_conflict_reason_obstructed)
    {
      svn_client_conflict_option_t *option;
      const char *wcroot_abspath;

      option = apr_pcalloc(options->pool, sizeof(*option));
      option->id =
        svn_client_conflict_option_merge_incoming_add_ignore;
      SVN_ERR(svn_wc__get_wcroot(&wcroot_abspath, conflict->ctx->wc_ctx,
                                 conflict->local_abspath, scratch_pool,
                                 scratch_pool));
      option->description =
        apr_psprintf(options->pool, _("ignore and do not add '^/%s@%ld' here"),
          incoming_new_repos_relpath, incoming_new_pegrev);
      option->conflict = conflict;
      option->do_resolve_func = resolve_merge_incoming_add_ignore;
      APR_ARRAY_PUSH(options, const svn_client_conflict_option_t *) = option;
    }

  return SVN_NO_ERROR;
}

/* Configure 'incoming added file text merge' resolution option for a tree
 * conflict. */
static svn_error_t *
configure_option_merge_incoming_added_file_text_merge(
  svn_client_conflict_t *conflict,
  apr_array_header_t *options,
  apr_pool_t *scratch_pool)
{
  svn_wc_operation_t operation;
  svn_wc_conflict_action_t incoming_change;
  svn_wc_conflict_reason_t local_change;
  svn_node_kind_t victim_node_kind;
  const char *incoming_new_repos_relpath;
  svn_revnum_t incoming_new_pegrev;
  svn_node_kind_t incoming_new_kind;

  operation = svn_client_conflict_get_operation(conflict);
  incoming_change = svn_client_conflict_get_incoming_change(conflict);
  local_change = svn_client_conflict_get_local_change(conflict);
  victim_node_kind = svn_client_conflict_tree_get_victim_node_kind(conflict);
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &incoming_new_repos_relpath, &incoming_new_pegrev,
            &incoming_new_kind, conflict, scratch_pool,
            scratch_pool));

  if (operation == svn_wc_operation_merge &&
      victim_node_kind == svn_node_file &&
      incoming_new_kind == svn_node_file &&
      incoming_change == svn_wc_conflict_action_add &&
      local_change == svn_wc_conflict_reason_obstructed)
    {
      svn_client_conflict_option_t *option;
      const char *wcroot_abspath;

      option = apr_pcalloc(options->pool, sizeof(*option));
      option->id =
        svn_client_conflict_option_merge_incoming_added_file_text_merge;
      SVN_ERR(svn_wc__get_wcroot(&wcroot_abspath, conflict->ctx->wc_ctx,
                                 conflict->local_abspath, scratch_pool,
                                 scratch_pool));
      option->description =
        apr_psprintf(options->pool, _("merge '^/%s@%ld' into '%s'"),
          incoming_new_repos_relpath, incoming_new_pegrev,
          svn_dirent_local_style(
            svn_dirent_skip_ancestor(wcroot_abspath,
                                     conflict->local_abspath),
            scratch_pool));
      option->conflict = conflict;
      option->do_resolve_func = resolve_merge_incoming_added_file_text_merge;
      APR_ARRAY_PUSH(options, const svn_client_conflict_option_t *) = option;
    }

  return SVN_NO_ERROR;
}

/* Configure 'incoming added file replace' resolution option for a tree
 * conflict. */
static svn_error_t *
configure_option_merge_incoming_added_file_replace(
  svn_client_conflict_t *conflict,
  apr_array_header_t *options,
  apr_pool_t *scratch_pool)
{
  svn_wc_operation_t operation;
  svn_wc_conflict_action_t incoming_change;
  svn_wc_conflict_reason_t local_change;
  svn_node_kind_t victim_node_kind;
  const char *incoming_new_repos_relpath;
  svn_revnum_t incoming_new_pegrev;
  svn_node_kind_t incoming_new_kind;

  operation = svn_client_conflict_get_operation(conflict);
  incoming_change = svn_client_conflict_get_incoming_change(conflict);
  local_change = svn_client_conflict_get_local_change(conflict);
  victim_node_kind = svn_client_conflict_tree_get_victim_node_kind(conflict);
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &incoming_new_repos_relpath, &incoming_new_pegrev,
            &incoming_new_kind, conflict, scratch_pool,
            scratch_pool));

  if (operation == svn_wc_operation_merge &&
      victim_node_kind == svn_node_file &&
      incoming_new_kind == svn_node_file &&
      incoming_change == svn_wc_conflict_action_add &&
      local_change == svn_wc_conflict_reason_obstructed)
    {
      svn_client_conflict_option_t *option;
      const char *wcroot_abspath;

      option = apr_pcalloc(options->pool, sizeof(*option));
      option->id = svn_client_conflict_option_merge_incoming_added_file_replace;
      SVN_ERR(svn_wc__get_wcroot(&wcroot_abspath, conflict->ctx->wc_ctx,
                                 conflict->local_abspath, scratch_pool,
                                 scratch_pool));
      option->description =
        apr_psprintf(options->pool, _("delete '%s' and copy '^/%s@%ld' here"),
                     svn_dirent_local_style(
                       svn_dirent_skip_ancestor(wcroot_abspath,
                                                conflict->local_abspath),
                       scratch_pool),
                     incoming_new_repos_relpath, incoming_new_pegrev);
      option->conflict = conflict;
      option->do_resolve_func = resolve_merge_incoming_added_file_replace;
      APR_ARRAY_PUSH(options, const svn_client_conflict_option_t *) = option;
    }

  return SVN_NO_ERROR;
}

/* Configure 'incoming added file replace and merge' resolution option for a
 * tree conflict. */
static svn_error_t *
configure_option_merge_incoming_added_file_replace_and_merge(
  svn_client_conflict_t *conflict,
  apr_array_header_t *options,
  apr_pool_t *scratch_pool)
{
  svn_wc_operation_t operation;
  svn_wc_conflict_action_t incoming_change;
  svn_wc_conflict_reason_t local_change;
  svn_node_kind_t victim_node_kind;
  const char *incoming_new_repos_relpath;
  svn_revnum_t incoming_new_pegrev;
  svn_node_kind_t incoming_new_kind;

  operation = svn_client_conflict_get_operation(conflict);
  incoming_change = svn_client_conflict_get_incoming_change(conflict);
  local_change = svn_client_conflict_get_local_change(conflict);
  victim_node_kind = svn_client_conflict_tree_get_victim_node_kind(conflict);
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &incoming_new_repos_relpath, &incoming_new_pegrev,
            &incoming_new_kind, conflict, scratch_pool,
            scratch_pool));

  if (operation == svn_wc_operation_merge &&
      victim_node_kind == svn_node_file &&
      incoming_new_kind == svn_node_file &&
      incoming_change == svn_wc_conflict_action_add &&
      local_change == svn_wc_conflict_reason_obstructed)
    {
      svn_client_conflict_option_t *option;
      const char *wcroot_abspath;

      option = apr_pcalloc(options->pool, sizeof(*option));
      option->id =
        svn_client_conflict_option_merge_incoming_added_file_replace_and_merge;
      SVN_ERR(svn_wc__get_wcroot(&wcroot_abspath, conflict->ctx->wc_ctx,
                                 conflict->local_abspath, scratch_pool,
                                 scratch_pool));
      option->description =
        apr_psprintf(options->pool,
          _("delete '%s', copy '^/%s@%ld' here, and merge the files"),
          svn_dirent_local_style(
            svn_dirent_skip_ancestor(wcroot_abspath,
                                     conflict->local_abspath),
            scratch_pool),
          incoming_new_repos_relpath, incoming_new_pegrev);
      option->conflict = conflict;
      option->do_resolve_func =
        resolve_merge_incoming_added_file_replace_and_merge;
      APR_ARRAY_PUSH(options, const svn_client_conflict_option_t *) = option;
    }

  return SVN_NO_ERROR;
}

/* Configure 'incoming added dir merge' resolution option for a tree
 * conflict. */
static svn_error_t *
configure_option_merge_incoming_added_dir_merge(svn_client_conflict_t *conflict,
                                                apr_array_header_t *options,
                                                apr_pool_t *scratch_pool)
{
  svn_wc_operation_t operation;
  svn_wc_conflict_action_t incoming_change;
  svn_wc_conflict_reason_t local_change;
  svn_node_kind_t victim_node_kind;
  const char *incoming_new_repos_relpath;
  svn_revnum_t incoming_new_pegrev;
  svn_node_kind_t incoming_new_kind;

  operation = svn_client_conflict_get_operation(conflict);
  incoming_change = svn_client_conflict_get_incoming_change(conflict);
  local_change = svn_client_conflict_get_local_change(conflict);
  victim_node_kind = svn_client_conflict_tree_get_victim_node_kind(conflict);
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &incoming_new_repos_relpath, &incoming_new_pegrev,
            &incoming_new_kind, conflict, scratch_pool,
            scratch_pool));

  if (operation == svn_wc_operation_merge &&
      victim_node_kind == svn_node_dir &&
      incoming_new_kind == svn_node_dir &&
      incoming_change == svn_wc_conflict_action_add &&
      local_change == svn_wc_conflict_reason_obstructed)
    {
      svn_client_conflict_option_t *option;
      const char *wcroot_abspath;

      option = apr_pcalloc(options->pool, sizeof(*option));
      option->id = svn_client_conflict_option_merge_incoming_added_dir_merge;
      SVN_ERR(svn_wc__get_wcroot(&wcroot_abspath, conflict->ctx->wc_ctx,
                                 conflict->local_abspath, scratch_pool,
                                 scratch_pool));
      option->description =
        apr_psprintf(options->pool, _("merge '^/%s@%ld' into '%s'"),
          incoming_new_repos_relpath, incoming_new_pegrev,
          svn_dirent_local_style(
            svn_dirent_skip_ancestor(wcroot_abspath,
                                     conflict->local_abspath),
            scratch_pool));
      option->conflict = conflict;
      option->do_resolve_func = resolve_merge_incoming_added_dir_merge;
      APR_ARRAY_PUSH(options, const svn_client_conflict_option_t *) = option;
    }

  return SVN_NO_ERROR;
}

/* Configure 'incoming added dir replace' resolution option for a tree
 * conflict. */
static svn_error_t *
configure_option_merge_incoming_added_dir_replace(
  svn_client_conflict_t *conflict,
  apr_array_header_t *options,
  apr_pool_t *scratch_pool)
{
  svn_wc_operation_t operation;
  svn_wc_conflict_action_t incoming_change;
  svn_wc_conflict_reason_t local_change;
  svn_node_kind_t victim_node_kind;
  const char *incoming_new_repos_relpath;
  svn_revnum_t incoming_new_pegrev;
  svn_node_kind_t incoming_new_kind;

  operation = svn_client_conflict_get_operation(conflict);
  incoming_change = svn_client_conflict_get_incoming_change(conflict);
  local_change = svn_client_conflict_get_local_change(conflict);
  victim_node_kind = svn_client_conflict_tree_get_victim_node_kind(conflict);
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &incoming_new_repos_relpath, &incoming_new_pegrev,
            &incoming_new_kind, conflict, scratch_pool,
            scratch_pool));

  if (operation == svn_wc_operation_merge &&
      victim_node_kind == svn_node_dir &&
      incoming_new_kind == svn_node_dir &&
      incoming_change == svn_wc_conflict_action_add &&
      local_change == svn_wc_conflict_reason_obstructed)
    {
      svn_client_conflict_option_t *option;
      const char *wcroot_abspath;

      option = apr_pcalloc(options->pool, sizeof(*option));
      option->id =
        svn_client_conflict_option_merge_incoming_added_dir_replace;
      SVN_ERR(svn_wc__get_wcroot(&wcroot_abspath, conflict->ctx->wc_ctx,
                                 conflict->local_abspath, scratch_pool,
                                 scratch_pool));
      option->description =
        apr_psprintf(options->pool, _("delete '%s' and copy '^/%s@%ld' here"),
          svn_dirent_local_style(
            svn_dirent_skip_ancestor(wcroot_abspath,
                                     conflict->local_abspath),
            scratch_pool),
          incoming_new_repos_relpath, incoming_new_pegrev);
      option->conflict = conflict;
      option->do_resolve_func = resolve_merge_incoming_added_dir_replace;
      APR_ARRAY_PUSH(options, const svn_client_conflict_option_t *) = option;
    }

  return SVN_NO_ERROR;
}

/* Configure 'incoming added dir replace and merge' resolution option
 * for a tree conflict. */
static svn_error_t *
configure_option_merge_incoming_added_dir_replace_and_merge(
  svn_client_conflict_t *conflict,
  apr_array_header_t *options,
  apr_pool_t *scratch_pool)
{
  svn_wc_operation_t operation;
  svn_wc_conflict_action_t incoming_change;
  svn_wc_conflict_reason_t local_change;
  svn_node_kind_t victim_node_kind;
  const char *incoming_new_repos_relpath;
  svn_revnum_t incoming_new_pegrev;
  svn_node_kind_t incoming_new_kind;

  operation = svn_client_conflict_get_operation(conflict);
  incoming_change = svn_client_conflict_get_incoming_change(conflict);
  local_change = svn_client_conflict_get_local_change(conflict);
  victim_node_kind = svn_client_conflict_tree_get_victim_node_kind(conflict);
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &incoming_new_repos_relpath, &incoming_new_pegrev,
            &incoming_new_kind, conflict, scratch_pool,
            scratch_pool));

  if (operation == svn_wc_operation_merge &&
      victim_node_kind == svn_node_dir &&
      incoming_new_kind == svn_node_dir &&
      incoming_change == svn_wc_conflict_action_add &&
      local_change == svn_wc_conflict_reason_obstructed)
    {
      svn_client_conflict_option_t *option;
      const char *wcroot_abspath;

      option = apr_pcalloc(options->pool, sizeof(*option));
      option->id =
        svn_client_conflict_option_merge_incoming_added_dir_replace_and_merge;
      SVN_ERR(svn_wc__get_wcroot(&wcroot_abspath, conflict->ctx->wc_ctx,
                                 conflict->local_abspath, scratch_pool,
                                 scratch_pool));
      option->description =
        apr_psprintf(options->pool,
          _("delete '%s', copy '^/%s@%ld' here, and merge the directories"),
          svn_dirent_local_style(
            svn_dirent_skip_ancestor(wcroot_abspath,
                                     conflict->local_abspath),
            scratch_pool),
          incoming_new_repos_relpath, incoming_new_pegrev);
      option->conflict = conflict;
      option->do_resolve_func =
        resolve_merge_incoming_added_dir_replace_and_merge;
      APR_ARRAY_PUSH(options, const svn_client_conflict_option_t *) = option;
    }

  return SVN_NO_ERROR;
}

/* Configure 'incoming delete ignore' resolution option for a tree conflict. */
static svn_error_t *
configure_option_incoming_delete_ignore(svn_client_conflict_t *conflict,
                                        apr_array_header_t *options,
                                        apr_pool_t *scratch_pool)
{
  svn_wc_operation_t operation;
  svn_wc_conflict_action_t incoming_change;
  svn_wc_conflict_reason_t local_change;
  const char *incoming_new_repos_relpath;
  svn_revnum_t incoming_new_pegrev;

  operation = svn_client_conflict_get_operation(conflict);
  incoming_change = svn_client_conflict_get_incoming_change(conflict);
  local_change = svn_client_conflict_get_local_change(conflict);
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &incoming_new_repos_relpath, &incoming_new_pegrev,
            NULL, conflict, scratch_pool,
            scratch_pool));

  if (incoming_change == svn_wc_conflict_action_delete)
    {
      svn_client_conflict_option_t *option;

      option = apr_pcalloc(options->pool, sizeof(*option));
      option->id = svn_client_conflict_option_incoming_delete_ignore;
      option->description =
        apr_psprintf(options->pool, _("ignore the deletion of '^/%s@%ld'"),
          incoming_new_repos_relpath, incoming_new_pegrev);
      option->conflict = conflict;
      option->do_resolve_func = resolve_incoming_delete_ignore;
      APR_ARRAY_PUSH(options, const svn_client_conflict_option_t *) = option;
    }

  return SVN_NO_ERROR;
}

/* Configure 'incoming delete accept' resolution option for a tree conflict. */
static svn_error_t *
configure_option_incoming_delete_accept(svn_client_conflict_t *conflict,
                                        apr_array_header_t *options,
                                        apr_pool_t *scratch_pool)
{
  svn_wc_operation_t operation;
  svn_wc_conflict_action_t incoming_change;
  svn_wc_conflict_reason_t local_change;
  const char *incoming_new_repos_relpath;
  svn_revnum_t incoming_new_pegrev;

  operation = svn_client_conflict_get_operation(conflict);
  incoming_change = svn_client_conflict_get_incoming_change(conflict);
  local_change = svn_client_conflict_get_local_change(conflict);
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &incoming_new_repos_relpath, &incoming_new_pegrev,
            NULL, conflict, scratch_pool,
            scratch_pool));

  if (incoming_change == svn_wc_conflict_action_delete)
    {
      svn_client_conflict_option_t *option;
      const char *wcroot_abspath;
      const char *local_abspath;

      option = apr_pcalloc(options->pool, sizeof(*option));
      option->id = svn_client_conflict_option_incoming_delete_accept;
      SVN_ERR(svn_wc__get_wcroot(&wcroot_abspath, conflict->ctx->wc_ctx,
                                 conflict->local_abspath, scratch_pool,
                                 scratch_pool));
      local_abspath = svn_client_conflict_get_local_abspath(conflict);
      option->description =
        apr_psprintf(options->pool, _("accept the deletion of '%s'"),
          svn_dirent_local_style(svn_dirent_skip_ancestor(wcroot_abspath,
                                                          local_abspath),
                                 scratch_pool));
      option->conflict = conflict;
      option->do_resolve_func = resolve_incoming_delete_accept;
      APR_ARRAY_PUSH(options, const svn_client_conflict_option_t *) = option;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_tree_get_resolution_options(apr_array_header_t **options,
                                                svn_client_conflict_t *conflict,
                                                apr_pool_t *result_pool,
                                                apr_pool_t *scratch_pool)
{
  svn_client_conflict_option_t *option;

  SVN_ERR(assert_tree_conflict(conflict, scratch_pool));

  *options = apr_array_make(result_pool, 2,
                            sizeof(svn_client_conflict_option_t *));

  /* Add postpone option. */
  option = apr_pcalloc(result_pool, sizeof(*option));
  option->id = svn_client_conflict_option_postpone;
  option->description = _("skip this conflict and leave it unresolved");
  option->conflict = conflict;
  option->do_resolve_func = resolve_postpone;
  APR_ARRAY_PUSH((*options), const svn_client_conflict_option_t *) = option;

  /* Add an option which marks the conflict resolved. */
  SVN_ERR(configure_option_accept_current_wc_state(conflict, *options));

  /* Configure options which offer automatic resolution. */
  SVN_ERR(configure_option_update_move_destination(conflict, *options));
  SVN_ERR(configure_option_update_raise_moved_away_children(conflict,
                                                            *options));
  SVN_ERR(configure_option_merge_incoming_add_ignore(conflict, *options,
                                                     scratch_pool));
  SVN_ERR(configure_option_merge_incoming_added_file_text_merge(conflict,
                                                                *options,
                                                                scratch_pool));
  SVN_ERR(configure_option_merge_incoming_added_file_replace(
            conflict, *options, scratch_pool));
  SVN_ERR(configure_option_merge_incoming_added_file_replace_and_merge(
            conflict, *options, scratch_pool));
  SVN_ERR(configure_option_merge_incoming_added_dir_merge(conflict, *options,
                                                          scratch_pool));
  SVN_ERR(configure_option_merge_incoming_added_dir_replace(conflict, *options,
                                                            scratch_pool));
  SVN_ERR(configure_option_merge_incoming_added_dir_replace_and_merge(
            conflict, *options, scratch_pool));
  SVN_ERR(configure_option_incoming_delete_ignore(conflict, *options,
                                                  scratch_pool));
  SVN_ERR(configure_option_incoming_delete_accept(conflict, *options,
                                                  scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_tree_get_details(svn_client_conflict_t *conflict,
                                     apr_pool_t *scratch_pool)
{
  SVN_ERR(assert_tree_conflict(conflict, scratch_pool));

  if (conflict->tree_conflict_get_incoming_details_func)
    SVN_ERR(conflict->tree_conflict_get_incoming_details_func(conflict,
                                                              scratch_pool));

  if (conflict->tree_conflict_get_local_details_func)
    SVN_ERR(conflict->tree_conflict_get_local_details_func(conflict,
                                                           scratch_pool));

  return SVN_NO_ERROR;
}

svn_client_conflict_option_id_t
svn_client_conflict_option_get_id(svn_client_conflict_option_t *option)
{
  return option->id;
}

svn_error_t *
svn_client_conflict_option_describe(const char **description,
                                    svn_client_conflict_option_t *option,
                                    apr_pool_t *result_pool,
                                    apr_pool_t *scratch_pool)
{
  *description = apr_pstrdup(result_pool, option->description);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_text_resolve(svn_client_conflict_t *conflict,
                                 svn_client_conflict_option_t *option,
                                 apr_pool_t *scratch_pool)
{
  SVN_ERR(assert_text_conflict(conflict, scratch_pool));
  SVN_ERR(option->do_resolve_func(option, conflict, scratch_pool));

  return SVN_NO_ERROR;
}

svn_client_conflict_option_t *
svn_client_conflict_option_find_by_id(apr_array_header_t *options,
                                      svn_client_conflict_option_id_t option_id)
{
  int i;

  for (i = 0; i < options->nelts; i++)
    {
      svn_client_conflict_option_t *this_option;
      svn_client_conflict_option_id_t this_option_id;
      
      this_option = APR_ARRAY_IDX(options, i, svn_client_conflict_option_t *);
      this_option_id = svn_client_conflict_option_get_id(this_option);

      if (this_option_id == option_id)
        return this_option;
    }

  return NULL;
}

svn_error_t *
svn_client_conflict_text_resolve_by_id(
  svn_client_conflict_t *conflict,
  svn_client_conflict_option_id_t option_id,
  apr_pool_t *scratch_pool)
{
  apr_array_header_t *resolution_options;
  svn_client_conflict_option_t *option;

  SVN_ERR(svn_client_conflict_text_get_resolution_options(
            &resolution_options, conflict,
            scratch_pool, scratch_pool));
  option = svn_client_conflict_option_find_by_id(resolution_options,
                                                 option_id);
  if (option == NULL)
    return svn_error_createf(SVN_ERR_CLIENT_CONFLICT_OPTION_NOT_APPLICABLE,
                             NULL,
                             _("Inapplicable conflict resolution option "
                               "ID '%d' given for conflicted path '%s'"),
                             option_id,
                             svn_dirent_local_style(conflict->local_abspath,
                                                    scratch_pool));
  SVN_ERR(svn_client_conflict_text_resolve(conflict, option, scratch_pool));

  return SVN_NO_ERROR;
}

svn_client_conflict_option_id_t
svn_client_conflict_text_get_resolution(svn_client_conflict_t *conflict)
{
  return conflict->resolution_text;
}

svn_error_t *
svn_client_conflict_prop_resolve(svn_client_conflict_t *conflict,
                                 const char *propname,
                                 svn_client_conflict_option_t *option,
                                 apr_pool_t *scratch_pool)
{
  SVN_ERR(assert_prop_conflict(conflict, scratch_pool));
  option->type_data.prop.propname = propname;
  SVN_ERR(option->do_resolve_func(option, conflict, scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_prop_resolve_by_id(
  svn_client_conflict_t *conflict,
  const char *propname,
  svn_client_conflict_option_id_t option_id,
  apr_pool_t *scratch_pool)
{
  apr_array_header_t *resolution_options;
  svn_client_conflict_option_t *option;

  SVN_ERR(svn_client_conflict_prop_get_resolution_options(
            &resolution_options, conflict,
            scratch_pool, scratch_pool));
  option = svn_client_conflict_option_find_by_id(resolution_options,
                                                 option_id);
  if (option == NULL)
    return svn_error_createf(SVN_ERR_CLIENT_CONFLICT_OPTION_NOT_APPLICABLE,
                             NULL,
                             _("Inapplicable conflict resolution option "
                               "ID '%d' given for conflicted path '%s'"),
                             option_id,
                             svn_dirent_local_style(conflict->local_abspath,
                                                    scratch_pool));
  SVN_ERR(svn_client_conflict_prop_resolve(conflict, propname, option,
                                           scratch_pool));

  return SVN_NO_ERROR;
}

svn_client_conflict_option_id_t
svn_client_conflict_prop_get_resolution(svn_client_conflict_t *conflict,
                                        const char *propname)
{
  svn_client_conflict_option_t *option;

  option = svn_hash_gets(conflict->resolved_props, propname);
  if (option == NULL)
    return svn_client_conflict_option_unspecified;

  return svn_client_conflict_option_get_id(option);
}

svn_error_t *
svn_client_conflict_tree_resolve(svn_client_conflict_t *conflict,
                                 svn_client_conflict_option_t *option,
                                 apr_pool_t *scratch_pool)
{
  SVN_ERR(assert_tree_conflict(conflict, scratch_pool));
  SVN_ERR(option->do_resolve_func(option, conflict, scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_tree_resolve_by_id(
  svn_client_conflict_t *conflict,
  svn_client_conflict_option_id_t option_id,
  apr_pool_t *scratch_pool)
{
  apr_array_header_t *resolution_options;
  svn_client_conflict_option_t *option;

  /* Backwards compatibility hack: Upper layers may still try to resolve
   * these two tree conflicts as 'mine-conflict' as Subversion 1.9 did.
   * Fix up if necessary... */
  if (option_id == svn_client_conflict_option_working_text_where_conflicted)
    {
      svn_wc_operation_t operation;

      operation = svn_client_conflict_get_operation(conflict);
      if (operation == svn_wc_operation_update ||
          operation == svn_wc_operation_switch)
        {
          svn_wc_conflict_reason_t reason;

          reason = svn_client_conflict_get_local_change(conflict);
          if (reason == svn_wc_conflict_reason_moved_away)
            {
              /* Map 'mine-conflict' to 'update move destination'. */
              option_id = svn_client_conflict_option_update_move_destination;
            }
          else if (reason == svn_wc_conflict_reason_deleted ||
                   reason == svn_wc_conflict_reason_replaced)
            {
              svn_wc_conflict_action_t action;
              svn_node_kind_t node_kind;

              action = svn_client_conflict_get_incoming_change(conflict);
              node_kind =
                svn_client_conflict_tree_get_victim_node_kind(conflict);

              if (action == svn_wc_conflict_action_edit &&
                  node_kind == svn_node_dir)
                {
                  /* Map 'mine-conflict' to 'update any moved away children'. */
                  option_id =
                    svn_client_conflict_option_update_any_moved_away_children;
                }
            }
        }
    }
  else if (option_id == svn_client_conflict_option_merged_text)
    {
      /* Another backwards compatibility hack for 'choose merged'. */
      option_id = svn_client_conflict_option_accept_current_wc_state;
    }
  
  SVN_ERR(svn_client_conflict_tree_get_resolution_options(
            &resolution_options, conflict,
            scratch_pool, scratch_pool));
  option = svn_client_conflict_option_find_by_id(resolution_options,
                                                 option_id);
  if (option == NULL)
    return svn_error_createf(SVN_ERR_CLIENT_CONFLICT_OPTION_NOT_APPLICABLE,
                             NULL,
                             _("Inapplicable conflict resolution option "
                               "ID '%d' given for conflicted path '%s'"),
                             option_id,
                             svn_dirent_local_style(conflict->local_abspath,
                                                    scratch_pool));
  SVN_ERR(svn_client_conflict_tree_resolve(conflict, option, scratch_pool));

  return SVN_NO_ERROR;
}

svn_client_conflict_option_id_t
svn_client_conflict_tree_get_resolution(svn_client_conflict_t *conflict)
{
  return conflict->resolution_tree;
}

/* Return the legacy conflict descriptor which is wrapped by CONFLICT. */
static const svn_wc_conflict_description2_t *
get_conflict_desc2_t(svn_client_conflict_t *conflict)
{
  if (conflict->legacy_text_conflict)
    return conflict->legacy_text_conflict;

  if (conflict->legacy_tree_conflict)
    return conflict->legacy_tree_conflict;

  if (conflict->prop_conflicts && conflict->legacy_prop_conflict_propname)
    return svn_hash_gets(conflict->prop_conflicts,
                         conflict->legacy_prop_conflict_propname);

  return NULL;
}

svn_error_t *
svn_client_conflict_get_conflicted(svn_boolean_t *text_conflicted,
                                   apr_array_header_t **props_conflicted,
                                   svn_boolean_t *tree_conflicted,
                                   svn_client_conflict_t *conflict,
                                   apr_pool_t *result_pool,
                                   apr_pool_t *scratch_pool)
{
  if (text_conflicted)
    *text_conflicted = (conflict->legacy_text_conflict != NULL);

  if (props_conflicted)
    {
      if (conflict->prop_conflicts)
        SVN_ERR(svn_hash_keys(props_conflicted, conflict->prop_conflicts,
                              result_pool));
      else
        *props_conflicted = apr_array_make(result_pool, 0,
                                           sizeof(const char*));
    }

  if (tree_conflicted)
    *tree_conflicted = (conflict->legacy_tree_conflict != NULL);

  return SVN_NO_ERROR;
}

const char *
svn_client_conflict_get_local_abspath(svn_client_conflict_t *conflict)
{
  return conflict->local_abspath;
}

svn_wc_operation_t
svn_client_conflict_get_operation(svn_client_conflict_t *conflict)
{
  return get_conflict_desc2_t(conflict)->operation;
}

svn_wc_conflict_action_t
svn_client_conflict_get_incoming_change(svn_client_conflict_t *conflict)
{
  return get_conflict_desc2_t(conflict)->action;
}

svn_wc_conflict_reason_t
svn_client_conflict_get_local_change(svn_client_conflict_t *conflict)
{
  return get_conflict_desc2_t(conflict)->reason;
}

svn_error_t *
svn_client_conflict_get_repos_info(const char **repos_root_url,
                                   const char **repos_uuid,
                                   svn_client_conflict_t *conflict,
                                   apr_pool_t *result_pool,
                                   apr_pool_t *scratch_pool)
{
  if (repos_root_url)
    {
      if (get_conflict_desc2_t(conflict)->src_left_version)
        *repos_root_url =
          get_conflict_desc2_t(conflict)->src_left_version->repos_url;
      else if (get_conflict_desc2_t(conflict)->src_right_version)
        *repos_root_url =
          get_conflict_desc2_t(conflict)->src_right_version->repos_url;
      else
        *repos_root_url = NULL;
    }

  if (repos_uuid)
    {
      if (get_conflict_desc2_t(conflict)->src_left_version)
        *repos_uuid =
          get_conflict_desc2_t(conflict)->src_left_version->repos_uuid;
      else if (get_conflict_desc2_t(conflict)->src_right_version)
        *repos_uuid =
          get_conflict_desc2_t(conflict)->src_right_version->repos_uuid;
      else
        *repos_uuid = NULL;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_get_incoming_old_repos_location(
  const char **incoming_old_repos_relpath,
  svn_revnum_t *incoming_old_pegrev,
  svn_node_kind_t *incoming_old_node_kind,
  svn_client_conflict_t *conflict,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool)
{
  if (incoming_old_repos_relpath)
    {
      if (get_conflict_desc2_t(conflict)->src_left_version)
        *incoming_old_repos_relpath =
          get_conflict_desc2_t(conflict)->src_left_version->path_in_repos;
      else
        *incoming_old_repos_relpath = NULL;
    }

  if (incoming_old_pegrev)
    {
      if (get_conflict_desc2_t(conflict)->src_left_version)
        *incoming_old_pegrev =
          get_conflict_desc2_t(conflict)->src_left_version->peg_rev;
      else
        *incoming_old_pegrev = SVN_INVALID_REVNUM;
    }

  if (incoming_old_node_kind)
    {
      if (get_conflict_desc2_t(conflict)->src_left_version)
        *incoming_old_node_kind =
          get_conflict_desc2_t(conflict)->src_left_version->node_kind;
      else
        *incoming_old_node_kind = svn_node_none;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_get_incoming_new_repos_location(
  const char **incoming_new_repos_relpath,
  svn_revnum_t *incoming_new_pegrev,
  svn_node_kind_t *incoming_new_node_kind,
  svn_client_conflict_t *conflict,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool)
{
  if (incoming_new_repos_relpath)
    {
      if (get_conflict_desc2_t(conflict)->src_right_version)
        *incoming_new_repos_relpath =
          get_conflict_desc2_t(conflict)->src_right_version->path_in_repos;
      else
        *incoming_new_repos_relpath = NULL;
    }

  if (incoming_new_pegrev)
    {
      if (get_conflict_desc2_t(conflict)->src_right_version)
        *incoming_new_pegrev =
          get_conflict_desc2_t(conflict)->src_right_version->peg_rev;
      else
        *incoming_new_pegrev = SVN_INVALID_REVNUM;
    }

  if (incoming_new_node_kind)
    {
      if (get_conflict_desc2_t(conflict)->src_right_version)
        *incoming_new_node_kind =
          get_conflict_desc2_t(conflict)->src_right_version->node_kind;
      else
        *incoming_new_node_kind = svn_node_none;
    }

  return SVN_NO_ERROR;
}

svn_node_kind_t
svn_client_conflict_tree_get_victim_node_kind(svn_client_conflict_t *conflict)
{
  SVN_ERR_ASSERT_NO_RETURN(assert_tree_conflict(conflict, conflict->pool)
                           == SVN_NO_ERROR);

  return get_conflict_desc2_t(conflict)->node_kind;
}

svn_error_t *
svn_client_conflict_prop_get_propvals(const svn_string_t **base_propval,
                                      const svn_string_t **working_propval,
                                      const svn_string_t **incoming_old_propval,
                                      const svn_string_t **incoming_new_propval,
                                      svn_client_conflict_t *conflict,
                                      const char *propname,
                                      apr_pool_t *result_pool)
{
  const svn_wc_conflict_description2_t *desc;

  SVN_ERR(assert_prop_conflict(conflict, conflict->pool));

  desc = svn_hash_gets(conflict->prop_conflicts, propname);
  if (desc == NULL)
    return svn_error_createf(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE, NULL,
                             _("Property '%s' is not in conflict."), propname);

  if (base_propval)
    *base_propval =
      svn_string_dup(desc->prop_value_base, result_pool);

  if (working_propval)
    *working_propval =
      svn_string_dup(desc->prop_value_working, result_pool);

  if (incoming_old_propval)
    *incoming_old_propval =
      svn_string_dup(desc->prop_value_incoming_old, result_pool);

  if (incoming_new_propval)
    *incoming_new_propval =
      svn_string_dup(desc->prop_value_incoming_new, result_pool);

  return SVN_NO_ERROR;
}

const char *
svn_client_conflict_prop_get_reject_abspath(svn_client_conflict_t *conflict)
{
  SVN_ERR_ASSERT_NO_RETURN(assert_prop_conflict(conflict, conflict->pool)
                           == SVN_NO_ERROR);

  /* svn_wc_conflict_description2_t stores this path in 'their_abspath' */
  return get_conflict_desc2_t(conflict)->their_abspath;
}

const char *
svn_client_conflict_text_get_mime_type(svn_client_conflict_t *conflict)
{
  SVN_ERR_ASSERT_NO_RETURN(assert_text_conflict(conflict, conflict->pool)
                           == SVN_NO_ERROR);

  return get_conflict_desc2_t(conflict)->mime_type;
}

svn_error_t *
svn_client_conflict_text_get_contents(const char **base_abspath,
                                      const char **working_abspath,
                                      const char **incoming_old_abspath,
                                      const char **incoming_new_abspath,
                                      svn_client_conflict_t *conflict,
                                      apr_pool_t *result_pool,
                                      apr_pool_t *scratch_pool)
{
  SVN_ERR(assert_text_conflict(conflict, scratch_pool));

  if (base_abspath)
    {
      if (svn_client_conflict_get_operation(conflict) ==
          svn_wc_operation_merge)
        *base_abspath = NULL; /* ### WC base contents not available yet */
      else /* update/switch */
        *base_abspath = get_conflict_desc2_t(conflict)->base_abspath;
    }

  if (working_abspath)
    *working_abspath = get_conflict_desc2_t(conflict)->my_abspath;

  if (incoming_old_abspath)
    *incoming_old_abspath = get_conflict_desc2_t(conflict)->base_abspath;

  if (incoming_new_abspath)
    *incoming_new_abspath = get_conflict_desc2_t(conflict)->their_abspath;

  return SVN_NO_ERROR;
}

/* Set up type-specific data for a new conflict object. */
static svn_error_t *
conflict_type_specific_setup(svn_client_conflict_t *conflict,
                             apr_pool_t *scratch_pool)
{
  svn_boolean_t tree_conflicted;
  svn_wc_conflict_action_t incoming_change;
  svn_wc_conflict_reason_t local_change;

  /* For now, we only deal with tree conflicts here. */
  SVN_ERR(svn_client_conflict_get_conflicted(NULL, NULL, &tree_conflicted,
                                             conflict, scratch_pool,
                                             scratch_pool));
  if (!tree_conflicted)
    return SVN_NO_ERROR;

  /* Set a default description function. */
  conflict->tree_conflict_get_incoming_description_func =
    conflict_tree_get_incoming_description_generic;
  conflict->tree_conflict_get_local_description_func =
    conflict_tree_get_local_description_generic;

  incoming_change = svn_client_conflict_get_incoming_change(conflict);
  local_change = svn_client_conflict_get_local_change(conflict);

  /* Set type-specific description and details functions. */
  if (incoming_change == svn_wc_conflict_action_delete ||
      incoming_change == svn_wc_conflict_action_replace)
    {
      conflict->tree_conflict_get_incoming_description_func =
        conflict_tree_get_description_incoming_delete;
      conflict->tree_conflict_get_incoming_details_func =
        conflict_tree_get_details_incoming_delete;
    }
  else if (incoming_change == svn_wc_conflict_action_add)
    {
      conflict->tree_conflict_get_incoming_description_func =
        conflict_tree_get_description_incoming_add;
      conflict->tree_conflict_get_incoming_details_func =
        conflict_tree_get_details_incoming_add;
    }
  else if (incoming_change == svn_wc_conflict_action_edit)
    {
      conflict->tree_conflict_get_incoming_description_func =
        conflict_tree_get_description_incoming_edit;
      conflict->tree_conflict_get_incoming_details_func =
        conflict_tree_get_details_incoming_edit;
    }

  if (local_change == svn_wc_conflict_reason_missing)
    {
      conflict->tree_conflict_get_local_description_func =
        conflict_tree_get_description_local_missing;
      conflict->tree_conflict_get_local_details_func =
        conflict_tree_get_details_local_missing;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_get(svn_client_conflict_t **conflict,
                        const char *local_abspath,
                        svn_client_ctx_t *ctx,
                        apr_pool_t *result_pool,
                        apr_pool_t *scratch_pool)
{
  const apr_array_header_t *descs;
  int i;

  *conflict = apr_pcalloc(result_pool, sizeof(**conflict));

  (*conflict)->local_abspath = apr_pstrdup(result_pool, local_abspath);
  (*conflict)->resolution_text = svn_client_conflict_option_unspecified;
  (*conflict)->resolution_tree = svn_client_conflict_option_unspecified;
  (*conflict)->resolved_props = apr_hash_make(result_pool);
  (*conflict)->ctx = ctx;
  (*conflict)->pool = result_pool;

  /* Add all legacy conflict descriptors we can find. Eventually, this code
   * path should stop relying on svn_wc_conflict_description2_t entirely. */
  SVN_ERR(svn_wc__read_conflict_descriptions2_t(&descs, ctx->wc_ctx,
                                                local_abspath,
                                                result_pool, scratch_pool));
  for (i = 0; i < descs->nelts; i++)
    {
      const svn_wc_conflict_description2_t *desc;

      desc = APR_ARRAY_IDX(descs, i, const svn_wc_conflict_description2_t *);
      add_legacy_desc_to_conflict(desc, *conflict, result_pool);
    }

  SVN_ERR(conflict_type_specific_setup(*conflict, scratch_pool));

  return SVN_NO_ERROR;
}
