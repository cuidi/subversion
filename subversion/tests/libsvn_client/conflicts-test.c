/*
 * Regression tests for the conflict resolver in the libsvn_client library.
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



#define SVN_DEPRECATED

#include "svn_client.h"
#include "svn_dirent_uri.h"
#include "svn_hash.h"

#include "../svn_test.h"
#include "../svn_test_fs.h"
#include "../libsvn_wc/utils.h"

struct status_baton
{
  svn_client_status_t *status;
  apr_pool_t *result_pool;
};

/* Implements svn_client_status_func_t */
static svn_error_t *
status_func(void *baton, const char *path,
            const svn_client_status_t *status,
            apr_pool_t *scratch_pool)
{
  struct status_baton *sb = baton;

  sb->status = svn_client_status_dup(status, sb->result_pool);

  return SVN_NO_ERROR;
}

/* 
 * The following tests verify resolution of "incoming file add vs.
 * local file obstruction upon merge" tree conflicts.
 */

/* Some paths we'll care about. */
static const char *trunk_path = "A";
static const char *branch_path = "A_branch";
static const char *new_file_name = "newfile.txt";
static const char *deleted_file_name = "mu";

/* File property content. */
static const char *propval_trunk = "This is a property on the trunk.";
static const char *propval_branch = "This is a property on the branch.";

/* A helper function which prepares a working copy for the tests below. */
static svn_error_t *
create_wc_with_file_add_vs_file_add_merge_conflict(svn_test__sandbox_t *b)
{
  static const char *new_file_path;
  svn_client_ctx_t *ctx;
  static const char *trunk_url;
  svn_opt_revision_t opt_rev;
  svn_client_status_t *status;
  struct status_baton sb;
  svn_client_conflict_t *conflict;
  svn_boolean_t tree_conflicted;

  SVN_ERR(sbox_add_and_commit_greek_tree(b));

  /* Create a branch of node "A". */
  SVN_ERR(sbox_wc_copy(b, trunk_path, branch_path));
  SVN_ERR(sbox_wc_commit(b, ""));

  /* Add new files on trunk and the branch which occupy the same path
   * but have different content and properties. */
  new_file_path = svn_relpath_join(trunk_path, new_file_name, b->pool);
  SVN_ERR(sbox_file_write(b, new_file_path,
                          "This is a new file on the trunk\n"));
  SVN_ERR(sbox_wc_add(b, new_file_path));
  SVN_ERR(sbox_wc_propset(b, "prop", propval_trunk, new_file_path));
  SVN_ERR(sbox_wc_commit(b, ""));
  new_file_path = svn_relpath_join(branch_path, new_file_name, b->pool);
  SVN_ERR(sbox_file_write(b, new_file_path,
                          /* NB: Ensure that the file content's length
                           * differs between the two branches! Tests are
                           * run with sleep for timestamps disabled. */
                          "This is a new file on the branch\n"));
  SVN_ERR(sbox_wc_add(b, new_file_path));
  SVN_ERR(sbox_wc_propset(b, "prop", propval_branch, new_file_path));
  SVN_ERR(sbox_wc_commit(b, ""));

  /* Run a merge from the trunk to the branch. */
  SVN_ERR(svn_test__create_client_ctx(&ctx, b, b->pool));

  SVN_ERR(sbox_wc_update(b, "", SVN_INVALID_REVNUM));
  trunk_url = apr_pstrcat(b->pool, b->repos_url, "/", trunk_path, SVN_VA_NULL);

  opt_rev.kind = svn_opt_revision_head;
  opt_rev.value.number = SVN_INVALID_REVNUM;
  /* This should raise an "incoming add vs local obstruction" tree conflict. */
  SVN_ERR(svn_client_merge_peg5(trunk_url, NULL, &opt_rev,
                                sbox_wc_path(b, branch_path),
                                svn_depth_infinity,
                                FALSE, FALSE, FALSE, FALSE, FALSE, FALSE,
                                NULL, ctx, b->pool));

  /* Ensure that the file has the expected status. */
  opt_rev.kind = svn_opt_revision_working;
  sb.result_pool = b->pool;
  SVN_ERR(svn_client_status6(NULL, ctx, sbox_wc_path(b, new_file_path),
                             &opt_rev, svn_depth_unknown, TRUE, TRUE,
                             TRUE, TRUE, FALSE, TRUE, NULL,
                             status_func, &sb, b->pool));
  status = sb.status;
  SVN_TEST_ASSERT(status->kind == svn_node_file);
  SVN_TEST_ASSERT(status->versioned);
  SVN_TEST_ASSERT(status->conflicted);
  SVN_TEST_ASSERT(status->node_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->text_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->prop_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(!status->copied);
  SVN_TEST_ASSERT(!status->switched);
  SVN_TEST_ASSERT(!status->file_external);
  SVN_TEST_ASSERT(status->moved_from_abspath == NULL);
  SVN_TEST_ASSERT(status->moved_to_abspath == NULL);

  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_file_path),
                                  ctx, b->pool, b->pool));

  /* Ensure that the expected tree conflict is present. */
  SVN_ERR(svn_client_conflict_get_conflicted(NULL, NULL, &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(tree_conflicted);
  SVN_TEST_ASSERT(svn_client_conflict_get_local_change(conflict) ==
                  svn_wc_conflict_reason_obstructed);
  SVN_TEST_ASSERT(svn_client_conflict_get_incoming_change(conflict) ==
                  svn_wc_conflict_action_add);

  return SVN_NO_ERROR;
}

static svn_error_t *
test_option_merge_incoming_added_file_ignore(const svn_test_opts_t *opts,
                                              apr_pool_t *pool)
{
  svn_client_ctx_t *ctx;
  svn_client_conflict_t *conflict;
  const char *new_file_path;
  svn_boolean_t text_conflicted;
  apr_array_header_t *props_conflicted;
  svn_boolean_t tree_conflicted;
  struct status_baton sb;
  struct svn_client_status_t *status;
  svn_opt_revision_t opt_rev;
  const svn_string_t *propval;

  svn_test__sandbox_t *b = apr_palloc(pool, sizeof(*b));

  SVN_ERR(svn_test__sandbox_create(b, "incoming_added_file_ignore",
                                   opts, pool));

  SVN_ERR(create_wc_with_file_add_vs_file_add_merge_conflict(b));

  /* Resolve the tree conflict. */
  SVN_ERR(svn_test__create_client_ctx(&ctx, b, b->pool));
  new_file_path = svn_relpath_join(branch_path, new_file_name, b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_file_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(svn_client_conflict_tree_resolve_by_id(
            conflict,
            svn_client_conflict_option_merge_incoming_add_ignore,
            b->pool));

  /* Ensure that the file has the expected status. */
  opt_rev.kind = svn_opt_revision_working;
  sb.result_pool = b->pool;
  SVN_ERR(svn_client_status6(NULL, ctx, sbox_wc_path(b, new_file_path),
                             &opt_rev, svn_depth_unknown, TRUE, TRUE,
                             TRUE, TRUE, FALSE, TRUE, NULL,
                             status_func, &sb, b->pool));
  status = sb.status;
  SVN_TEST_ASSERT(status->kind == svn_node_file);
  SVN_TEST_ASSERT(status->versioned);
  SVN_TEST_ASSERT(!status->conflicted);
  SVN_TEST_ASSERT(status->node_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->text_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->prop_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(!status->copied);
  SVN_TEST_ASSERT(!status->switched);
  SVN_TEST_ASSERT(!status->file_external);
  SVN_TEST_ASSERT(status->moved_from_abspath == NULL);
  SVN_TEST_ASSERT(status->moved_to_abspath == NULL);

  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_file_path),
                                  ctx, b->pool, b->pool));

  /* The file should not be in conflict. */
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(!text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  /* Verify the merged property value. */
  SVN_ERR(svn_wc_prop_get2(&propval, ctx->wc_ctx,
                           sbox_wc_path(b, new_file_path),
                           "prop", b->pool, b->pool));
  SVN_TEST_STRING_ASSERT(propval->data, propval_branch);

  return SVN_NO_ERROR;
}

static svn_error_t *
test_option_merge_incoming_added_file_text_merge(const svn_test_opts_t *opts,
                                                 apr_pool_t *pool)
{
  svn_client_ctx_t *ctx;
  svn_client_conflict_t *conflict;
  const char *new_file_path;
  svn_boolean_t text_conflicted;
  apr_array_header_t *props_conflicted;
  svn_boolean_t tree_conflicted;
  struct status_baton sb;
  struct svn_client_status_t *status;
  svn_opt_revision_t opt_rev;
  const svn_string_t *propval;

  svn_test__sandbox_t *b = apr_palloc(pool, sizeof(*b));

  SVN_ERR(svn_test__sandbox_create(b, "incoming_added_file_text_merge",
                                   opts, pool));

  SVN_ERR(create_wc_with_file_add_vs_file_add_merge_conflict(b));

  /* Resolve the tree conflict. */
  SVN_ERR(svn_test__create_client_ctx(&ctx, b, b->pool));
  new_file_path = svn_relpath_join(branch_path, new_file_name, b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_file_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(svn_client_conflict_tree_resolve_by_id(
            conflict,
            svn_client_conflict_option_merge_incoming_added_file_text_merge,
            b->pool));

  /* Ensure that the file has the expected status. */
  opt_rev.kind = svn_opt_revision_working;
  sb.result_pool = b->pool;
  SVN_ERR(svn_client_status6(NULL, ctx, sbox_wc_path(b, new_file_path),
                             &opt_rev, svn_depth_unknown, TRUE, TRUE,
                             TRUE, TRUE, FALSE, TRUE, NULL,
                             status_func, &sb, b->pool));
  status = sb.status;
  SVN_TEST_ASSERT(status->kind == svn_node_file);
  SVN_TEST_ASSERT(status->versioned);
  SVN_TEST_ASSERT(status->conflicted);
  SVN_TEST_ASSERT(status->node_status == svn_wc_status_conflicted);
  SVN_TEST_ASSERT(status->text_status == svn_wc_status_conflicted);
  /* ### Shouldn't there be a property conflict? The trunk wins. */
  SVN_TEST_ASSERT(status->prop_status == svn_wc_status_modified);
  SVN_TEST_ASSERT(!status->copied);
  SVN_TEST_ASSERT(!status->switched);
  SVN_TEST_ASSERT(!status->file_external);
  SVN_TEST_ASSERT(status->moved_from_abspath == NULL);
  SVN_TEST_ASSERT(status->moved_to_abspath == NULL);

  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_file_path),
                                  ctx, b->pool, b->pool));

  /* We should have a text conflict instead of a tree conflict. */
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  /* Verify the merged property value. */
  SVN_ERR(svn_wc_prop_get2(&propval, ctx->wc_ctx,
                           sbox_wc_path(b, new_file_path),
                           "prop", b->pool, b->pool));
  SVN_TEST_STRING_ASSERT(propval->data, propval_trunk);

  return SVN_NO_ERROR;
}

static svn_error_t *
test_option_merge_incoming_added_file_replace(const svn_test_opts_t *opts,
                                              apr_pool_t *pool)
{
  svn_client_ctx_t *ctx;
  svn_client_conflict_t *conflict;
  const char *new_file_path;
  svn_boolean_t text_conflicted;
  apr_array_header_t *props_conflicted;
  svn_boolean_t tree_conflicted;
  struct status_baton sb;
  struct svn_client_status_t *status;
  svn_opt_revision_t opt_rev;
  const svn_string_t *propval;

  svn_test__sandbox_t *b = apr_palloc(pool, sizeof(*b));

  SVN_ERR(svn_test__sandbox_create(b, "incoming_added_file_replace",
                                   opts, pool));

  SVN_ERR(create_wc_with_file_add_vs_file_add_merge_conflict(b));

  /* Resolve the tree conflict. */
  SVN_ERR(svn_test__create_client_ctx(&ctx, b, b->pool));
  new_file_path = svn_relpath_join(branch_path, new_file_name, b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_file_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(svn_client_conflict_tree_resolve_by_id(
            conflict,
            svn_client_conflict_option_merge_incoming_added_file_replace,
            b->pool));

  /* Ensure that the file has the expected status. */
  opt_rev.kind = svn_opt_revision_working;
  sb.result_pool = b->pool;
  SVN_ERR(svn_client_status6(NULL, ctx, sbox_wc_path(b, new_file_path),
                             &opt_rev, svn_depth_unknown, TRUE, TRUE,
                             TRUE, TRUE, FALSE, TRUE, NULL,
                             status_func, &sb, b->pool));
  status = sb.status;
  SVN_TEST_ASSERT(status->kind == svn_node_file);
  SVN_TEST_ASSERT(status->versioned);
  SVN_TEST_ASSERT(!status->conflicted);
  SVN_TEST_ASSERT(status->node_status == svn_wc_status_replaced);
  SVN_TEST_ASSERT(status->text_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->prop_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->copied);
  SVN_TEST_ASSERT(!status->switched);
  SVN_TEST_ASSERT(!status->file_external);
  SVN_TEST_ASSERT(status->moved_from_abspath == NULL);
  SVN_TEST_ASSERT(status->moved_to_abspath == NULL);

  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_file_path),
                                  ctx, b->pool, b->pool));

  /* The file should not be in conflict. */
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(!text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  /* Verify the merged property value. */
  SVN_ERR(svn_wc_prop_get2(&propval, ctx->wc_ctx,
                           sbox_wc_path(b, new_file_path),
                           "prop", b->pool, b->pool));
  SVN_TEST_STRING_ASSERT(propval->data, propval_trunk);

  return SVN_NO_ERROR;
}

static svn_error_t *
test_option_merge_incoming_added_file_replace_and_merge(
  const svn_test_opts_t *opts,
   apr_pool_t *pool)
{
  svn_client_ctx_t *ctx;
  svn_client_conflict_t *conflict;
  const char *new_file_path;
  svn_boolean_t text_conflicted;
  apr_array_header_t *props_conflicted;
  svn_boolean_t tree_conflicted;
  struct status_baton sb;
  struct svn_client_status_t *status;
  svn_opt_revision_t opt_rev;
  const svn_string_t *propval;

  svn_test__sandbox_t *b = apr_palloc(pool, sizeof(*b));

  SVN_ERR(svn_test__sandbox_create(b, "incoming_added_file_replace_and_merge",
                                   opts, pool));

  SVN_ERR(create_wc_with_file_add_vs_file_add_merge_conflict(b));

  /* Resolve the tree conflict. */
  SVN_ERR(svn_test__create_client_ctx(&ctx, b, b->pool));
  new_file_path = svn_relpath_join(branch_path, new_file_name, b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_file_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(
    svn_client_conflict_tree_resolve_by_id(
      conflict,
      svn_client_conflict_option_merge_incoming_added_file_replace_and_merge,
      b->pool));

  /* Ensure that the file has the expected status. */
  opt_rev.kind = svn_opt_revision_working;
  sb.result_pool = b->pool;
  SVN_ERR(svn_client_status6(NULL, ctx, sbox_wc_path(b, new_file_path),
                             &opt_rev, svn_depth_unknown, TRUE, TRUE,
                             TRUE, TRUE, FALSE, TRUE, NULL,
                             status_func, &sb, b->pool));
  status = sb.status;
  SVN_TEST_ASSERT(status->kind == svn_node_file);
  SVN_TEST_ASSERT(status->versioned);
  SVN_TEST_ASSERT(status->conflicted);
  SVN_TEST_ASSERT(status->node_status == svn_wc_status_conflicted);
  SVN_TEST_ASSERT(status->text_status == svn_wc_status_conflicted);
  /* ### Shouldn't there be a property conflict? The trunk wins. */
  SVN_TEST_ASSERT(status->prop_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->copied);
  SVN_TEST_ASSERT(!status->switched);
  SVN_TEST_ASSERT(!status->file_external);
  SVN_TEST_ASSERT(status->moved_from_abspath == NULL);
  SVN_TEST_ASSERT(status->moved_to_abspath == NULL);

  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_file_path),
                                  ctx, b->pool, b->pool));

  /* We should have a text conflict instead of a tree conflict. */
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  /* Verify the merged property value. */
  SVN_ERR(svn_wc_prop_get2(&propval, ctx->wc_ctx,
                           sbox_wc_path(b, new_file_path),
                           "prop", b->pool, b->pool));
  SVN_TEST_STRING_ASSERT(propval->data, propval_trunk);

  return SVN_NO_ERROR;
}

/* 
 * The following tests verify resolution of "incoming dir add vs.
 * local dir obstruction upon merge" tree conflicts.
 */
static const char *new_dir_name = "newdir";

/* A helper function which prepares a working copy for the tests below. */
static svn_error_t *
create_wc_with_dir_add_vs_dir_add_merge_conflict(
  svn_test__sandbox_t *b,
  svn_boolean_t file_change_on_trunk,
  svn_boolean_t with_move,
  svn_boolean_t file_change_on_branch)
{
  static const char *new_dir_path;
  static const char *new_file_path;
  svn_client_ctx_t *ctx;
  static const char *trunk_url;
  svn_opt_revision_t opt_rev;
  svn_client_status_t *status;
  struct status_baton sb;
  svn_client_conflict_t *conflict;
  svn_boolean_t tree_conflicted;
  const char *move_src_path;

  SVN_ERR(sbox_add_and_commit_greek_tree(b));

  /* Create a branch of node "A". */
  SVN_ERR(sbox_wc_copy(b, trunk_path, branch_path));
  SVN_ERR(sbox_wc_commit(b, ""));

  /* Add new directories on trunk and the branch which occupy the same path
   * but have different content and properties. */
  if (with_move)
    {
      /* History starts at ^/newdir.orig, outside of ^/A (the "trunk").
       * Then a move to ^/A/newdir causes a collision. */
      move_src_path = apr_pstrcat(b->pool, new_dir_name, ".orig", SVN_VA_NULL);
      new_dir_path = move_src_path;
    }
  else
    {
      new_dir_path = svn_relpath_join(trunk_path, new_dir_name, b->pool);
      move_src_path = NULL;
    }

  SVN_ERR(sbox_wc_mkdir(b, new_dir_path));
  new_file_path = svn_relpath_join(new_dir_path, new_file_name, b->pool);
  SVN_ERR(sbox_file_write(b, new_file_path,
                          "This is a new file on the trunk\n"));
  SVN_ERR(sbox_wc_add(b, new_file_path));
  SVN_ERR(sbox_wc_propset(b, "prop", propval_trunk, new_file_path));
  SVN_ERR(sbox_wc_commit(b, ""));
  if (file_change_on_trunk)
    {
      SVN_ERR(sbox_file_write(b, new_file_path,
                              "This is a change to the new file"
                              "on the trunk\n"));
      SVN_ERR(sbox_wc_commit(b, ""));
    }
  if (with_move)
    {
      /* Now move the new directory to the colliding path. */
      new_dir_path = svn_relpath_join(trunk_path, new_dir_name, b->pool);
      SVN_ERR(sbox_wc_update(b, "", SVN_INVALID_REVNUM));
      sbox_wc_move(b, move_src_path, new_dir_path);
      SVN_ERR(sbox_wc_commit(b, ""));
    }
  new_dir_path = svn_relpath_join(branch_path, new_dir_name, b->pool);
  SVN_ERR(sbox_wc_mkdir(b, new_dir_path));
  new_file_path = svn_relpath_join(branch_path,
                                   svn_relpath_join(new_dir_name,
                                                    new_file_name, b->pool),
                                   b->pool);
  SVN_ERR(sbox_file_write(b, new_file_path,
                          /* NB: Ensure that the file content's length
                           * differs between the two branches! Tests are
                           * run with sleep for timestamps disabled. */
                          "This is a new file on the branch\n"));
  SVN_ERR(sbox_wc_add(b, new_file_path));
  SVN_ERR(sbox_wc_propset(b, "prop", propval_branch, new_file_path));
  SVN_ERR(sbox_wc_commit(b, ""));

  if (file_change_on_branch)
    {
      SVN_ERR(sbox_file_write(b, new_file_path,
                              "This is a change to the new file "
                              "on the branch\n"));
      SVN_ERR(sbox_wc_commit(b, ""));
    }

  /* Run a merge from the trunk to the branch. */
  SVN_ERR(svn_test__create_client_ctx(&ctx, b, b->pool));

  SVN_ERR(sbox_wc_update(b, "", SVN_INVALID_REVNUM));
  trunk_url = apr_pstrcat(b->pool, b->repos_url, "/", trunk_path, SVN_VA_NULL);

  opt_rev.kind = svn_opt_revision_head;
  opt_rev.value.number = SVN_INVALID_REVNUM;
  /* This should raise an "incoming add vs local obstruction" tree conflict. */
  SVN_ERR(svn_client_merge_peg5(trunk_url, NULL, &opt_rev,
                                sbox_wc_path(b, branch_path),
                                svn_depth_infinity,
                                FALSE, FALSE, FALSE, FALSE, FALSE, FALSE,
                                NULL, ctx, b->pool));

  /* Ensure that the directory has the expected status. */
  opt_rev.kind = svn_opt_revision_working;
  sb.result_pool = b->pool;
  SVN_ERR(svn_client_status6(NULL, ctx, sbox_wc_path(b, new_dir_path),
                             &opt_rev, svn_depth_unknown, TRUE, TRUE,
                             TRUE, TRUE, FALSE, TRUE, NULL,
                             status_func, &sb, b->pool));
  status = sb.status;
  SVN_TEST_ASSERT(status->kind == svn_node_dir);
  SVN_TEST_ASSERT(status->versioned);
  SVN_TEST_ASSERT(status->conflicted);
  SVN_TEST_ASSERT(status->node_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->prop_status == svn_wc_status_none);
  SVN_TEST_ASSERT(!status->copied);
  SVN_TEST_ASSERT(!status->switched);
  SVN_TEST_ASSERT(!status->file_external);
  SVN_TEST_ASSERT(status->moved_from_abspath == NULL);
  SVN_TEST_ASSERT(status->moved_to_abspath == NULL);

  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_dir_path),
                                  ctx, b->pool, b->pool));

  /* Ensure that the expected tree conflict is present. */
  SVN_ERR(svn_client_conflict_get_conflicted(NULL, NULL, &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(tree_conflicted);
  SVN_TEST_ASSERT(svn_client_conflict_get_local_change(conflict) ==
                  svn_wc_conflict_reason_obstructed);
  SVN_TEST_ASSERT(svn_client_conflict_get_incoming_change(conflict) ==
                  svn_wc_conflict_action_add);

  return SVN_NO_ERROR;
}

static svn_error_t *
test_option_merge_incoming_added_dir_ignore(const svn_test_opts_t *opts,
                                            apr_pool_t *pool)
{
  svn_client_ctx_t *ctx;
  svn_client_conflict_t *conflict;
  const char *new_dir_path;
  svn_boolean_t text_conflicted;
  apr_array_header_t *props_conflicted;
  svn_boolean_t tree_conflicted;
  struct status_baton sb;
  struct svn_client_status_t *status;
  svn_opt_revision_t opt_rev;
  svn_test__sandbox_t *b = apr_palloc(pool, sizeof(*b));

  SVN_ERR(svn_test__sandbox_create(b, "incoming_added_dir_ignore",
                                   opts, pool));

  SVN_ERR(create_wc_with_dir_add_vs_dir_add_merge_conflict(b, FALSE, FALSE,
                                                           FALSE));

  /* Resolve the tree conflict. */
  SVN_ERR(svn_test__create_client_ctx(&ctx, b, b->pool));
  new_dir_path = svn_relpath_join(branch_path, new_dir_name, b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_dir_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(svn_client_conflict_tree_resolve_by_id(
            conflict,
            svn_client_conflict_option_merge_incoming_add_ignore,
            b->pool));

  /* Ensure that the directory has the expected status. */
  opt_rev.kind = svn_opt_revision_working;
  sb.result_pool = b->pool;
  SVN_ERR(svn_client_status6(NULL, ctx, sbox_wc_path(b, new_dir_path),
                             &opt_rev, svn_depth_unknown, TRUE, TRUE,
                             TRUE, TRUE, FALSE, TRUE, NULL,
                             status_func, &sb, b->pool));
  status = sb.status;
  SVN_TEST_ASSERT(status->kind == svn_node_dir);
  SVN_TEST_ASSERT(status->versioned);
  SVN_TEST_ASSERT(!status->conflicted);
  SVN_TEST_ASSERT(status->node_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->text_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->prop_status == svn_wc_status_none);
  SVN_TEST_ASSERT(!status->copied);
  SVN_TEST_ASSERT(!status->switched);
  SVN_TEST_ASSERT(!status->file_external);
  SVN_TEST_ASSERT(status->moved_from_abspath == NULL);
  SVN_TEST_ASSERT(status->moved_to_abspath == NULL);

  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_dir_path),
                                  ctx, b->pool, b->pool));

  /* The directory should not be in conflict. */
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(!text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  return SVN_NO_ERROR;
}

/* This test currently fails to meet expectations. Our merge code doesn't
 * support a merge of files which were added in the same revision as their
 * parent directory and were not modified since. */
static svn_error_t *
test_option_merge_incoming_added_dir_merge(const svn_test_opts_t *opts,
                                           apr_pool_t *pool)
{
  svn_client_ctx_t *ctx;
  svn_client_conflict_t *conflict;
  const char *new_dir_path;
  const char *new_file_path;
  svn_boolean_t text_conflicted;
  apr_array_header_t *props_conflicted;
  svn_boolean_t tree_conflicted;
  struct status_baton sb;
  struct svn_client_status_t *status;
  svn_opt_revision_t opt_rev;
  const svn_string_t *propval;
  svn_test__sandbox_t *b = apr_palloc(pool, sizeof(*b));

  SVN_ERR(svn_test__sandbox_create(b, "incoming_added_dir_merge",
                                   opts, pool));

  SVN_ERR(create_wc_with_dir_add_vs_dir_add_merge_conflict(b, FALSE, FALSE,
                                                           FALSE));

  /* Resolve the tree conflict. */
  SVN_ERR(svn_test__create_client_ctx(&ctx, b, b->pool));
  new_dir_path = svn_relpath_join(branch_path, new_dir_name, b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_dir_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(svn_client_conflict_tree_get_details(conflict, b->pool));
  SVN_ERR(svn_client_conflict_tree_resolve_by_id(
            conflict,
            svn_client_conflict_option_merge_incoming_added_dir_merge,
            b->pool));

  /* Ensure that the directory has the expected status. */
  opt_rev.kind = svn_opt_revision_working;
  sb.result_pool = b->pool;
  SVN_ERR(svn_client_status6(NULL, ctx, sbox_wc_path(b, new_dir_path),
                             &opt_rev, svn_depth_unknown, TRUE, TRUE,
                             TRUE, TRUE, FALSE, TRUE, NULL,
                             status_func, &sb, b->pool));
  status = sb.status;
  SVN_TEST_ASSERT(status->kind == svn_node_dir);
  SVN_TEST_ASSERT(status->versioned);
  SVN_TEST_ASSERT(!status->conflicted);
  SVN_TEST_ASSERT(status->node_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->text_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->prop_status == svn_wc_status_none);
  SVN_TEST_ASSERT(!status->copied);
  SVN_TEST_ASSERT(!status->switched);
  SVN_TEST_ASSERT(!status->file_external);
  SVN_TEST_ASSERT(status->moved_from_abspath == NULL);
  SVN_TEST_ASSERT(status->moved_to_abspath == NULL);

  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_dir_path),
                                  ctx, b->pool, b->pool));

  /* The directory should not be in conflict. */
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(!text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  /* XFAIL: Currently, no text conflict is raised since the file is not merged.
   * We should have a text conflict in the file. */
  new_file_path = svn_relpath_join(branch_path,
                                   svn_relpath_join(new_dir_name,
                                                    new_file_name, b->pool),
                                   b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_file_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  /* Verify the file's merged property value. */
  SVN_ERR(svn_wc_prop_get2(&propval, ctx->wc_ctx,
                           sbox_wc_path(b, new_file_path),
                           "prop", b->pool, b->pool));
  SVN_TEST_STRING_ASSERT(propval->data, propval_trunk);

  return SVN_NO_ERROR;
}

/* Same test as above, but with an additional file change on the trunk
 * which makes resolution work as expected. */
static svn_error_t *
test_option_merge_incoming_added_dir_merge2(const svn_test_opts_t *opts,
                                            apr_pool_t *pool)
{
  svn_client_ctx_t *ctx;
  svn_client_conflict_t *conflict;
  const char *new_dir_path;
  const char *new_file_path;
  svn_boolean_t text_conflicted;
  apr_array_header_t *props_conflicted;
  svn_boolean_t tree_conflicted;
  struct status_baton sb;
  struct svn_client_status_t *status;
  svn_opt_revision_t opt_rev;
  const svn_string_t *propval;
  svn_test__sandbox_t *b = apr_palloc(pool, sizeof(*b));

  SVN_ERR(svn_test__sandbox_create(b, "incoming_added_dir_merge2",
                                   opts, pool));

  SVN_ERR(create_wc_with_dir_add_vs_dir_add_merge_conflict(b, TRUE, FALSE,
                                                           FALSE));

  /* Resolve the tree conflict. */
  SVN_ERR(svn_test__create_client_ctx(&ctx, b, b->pool));
  new_dir_path = svn_relpath_join(branch_path, new_dir_name, b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_dir_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(svn_client_conflict_tree_get_details(conflict, b->pool));
  SVN_ERR(svn_client_conflict_tree_resolve_by_id(
            conflict,
            svn_client_conflict_option_merge_incoming_added_dir_merge,
            b->pool));

  /* Ensure that the directory has the expected status. */
  opt_rev.kind = svn_opt_revision_working;
  sb.result_pool = b->pool;
  SVN_ERR(svn_client_status6(NULL, ctx, sbox_wc_path(b, new_dir_path),
                             &opt_rev, svn_depth_unknown, TRUE, TRUE,
                             TRUE, TRUE, FALSE, TRUE, NULL,
                             status_func, &sb, b->pool));
  status = sb.status;
  SVN_TEST_ASSERT(status->kind == svn_node_dir);
  SVN_TEST_ASSERT(status->versioned);
  SVN_TEST_ASSERT(!status->conflicted);
  SVN_TEST_ASSERT(status->node_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->text_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->prop_status == svn_wc_status_none);
  SVN_TEST_ASSERT(!status->copied);
  SVN_TEST_ASSERT(!status->switched);
  SVN_TEST_ASSERT(!status->file_external);
  SVN_TEST_ASSERT(status->moved_from_abspath == NULL);
  SVN_TEST_ASSERT(status->moved_to_abspath == NULL);

  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_dir_path),
                                  ctx, b->pool, b->pool));

  /* The directory should not be in conflict. */
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(!text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  /* We should have a text conflict in the file. */
  new_file_path = svn_relpath_join(branch_path,
                                   svn_relpath_join(new_dir_name,
                                                    new_file_name, b->pool),
                                   b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_file_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  /* Verify the file's merged property value. */
  /* ### Shouldn't there be a property conflict? The branch wins. */
  SVN_ERR(svn_wc_prop_get2(&propval, ctx->wc_ctx,
                           sbox_wc_path(b, new_file_path),
                           "prop", b->pool, b->pool));
  SVN_TEST_STRING_ASSERT(propval->data, propval_branch);

  return SVN_NO_ERROR;
}

/* Same test as above, but with an additional move operation on the trunk. */
static svn_error_t *
test_option_merge_incoming_added_dir_merge3(const svn_test_opts_t *opts,
                                            apr_pool_t *pool)
{
  svn_client_ctx_t *ctx;
  svn_client_conflict_t *conflict;
  const char *new_dir_path;
  const char *new_file_path;
  svn_boolean_t text_conflicted;
  apr_array_header_t *props_conflicted;
  svn_boolean_t tree_conflicted;
  struct status_baton sb;
  struct svn_client_status_t *status;
  svn_opt_revision_t opt_rev;
  const svn_string_t *propval;
  svn_test__sandbox_t *b = apr_palloc(pool, sizeof(*b));

  SVN_ERR(svn_test__sandbox_create(b, "incoming_added_dir_merge3",
                                   opts, pool));

  SVN_ERR(create_wc_with_dir_add_vs_dir_add_merge_conflict(b, TRUE, TRUE,
                                                           FALSE));

  /* Resolve the tree conflict. */
  SVN_ERR(svn_test__create_client_ctx(&ctx, b, b->pool));
  new_dir_path = svn_relpath_join(branch_path, new_dir_name, b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_dir_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(svn_client_conflict_tree_get_details(conflict, b->pool));
  SVN_ERR(svn_client_conflict_tree_resolve_by_id(
            conflict,
            svn_client_conflict_option_merge_incoming_added_dir_merge,
            b->pool));

  /* Ensure that the directory has the expected status. */
  opt_rev.kind = svn_opt_revision_working;
  sb.result_pool = b->pool;
  SVN_ERR(svn_client_status6(NULL, ctx, sbox_wc_path(b, new_dir_path),
                             &opt_rev, svn_depth_unknown, TRUE, TRUE,
                             TRUE, TRUE, FALSE, TRUE, NULL,
                             status_func, &sb, b->pool));
  status = sb.status;
  SVN_TEST_ASSERT(status->kind == svn_node_dir);
  SVN_TEST_ASSERT(status->versioned);
  SVN_TEST_ASSERT(!status->conflicted);
  SVN_TEST_ASSERT(status->node_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->text_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->prop_status == svn_wc_status_none);
  SVN_TEST_ASSERT(!status->copied);
  SVN_TEST_ASSERT(!status->switched);
  SVN_TEST_ASSERT(!status->file_external);
  SVN_TEST_ASSERT(status->moved_from_abspath == NULL);
  SVN_TEST_ASSERT(status->moved_to_abspath == NULL);

  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_dir_path),
                                  ctx, b->pool, b->pool));

  /* The directory should not be in conflict. */
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(!text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  /* We should have a text conflict in the file. */
  new_file_path = svn_relpath_join(branch_path,
                                   svn_relpath_join(new_dir_name,
                                                    new_file_name, b->pool),
                                   b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_file_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  /* Verify the file's merged property value. */
  /* ### Shouldn't there be a property conflict? The branch wins. */
  SVN_ERR(svn_wc_prop_get2(&propval, ctx->wc_ctx,
                           sbox_wc_path(b, new_file_path),
                           "prop", b->pool, b->pool));
  SVN_TEST_STRING_ASSERT(propval->data, propval_branch);

  /* XFAIL: Currently, no subtree mergeinfo is created.
   *
   * Verify the directory's subtree mergeinfo. It should mention both
   * location segments of ^/A/newdir's history, shouldn't it? Like this:
   *
   *   /A/newdir:2-6
   *   /newdir.orig:4
   *
   * ### /newdir.orig was created in r3 and moved to /A/newdir in r5.
   * ### Should the second line say "/newdir.orig:3-4" instead? */
  SVN_ERR(svn_wc_prop_get2(&propval, ctx->wc_ctx,
                           sbox_wc_path(b, new_dir_path),
                           "svn:mergeinfo", b->pool, b->pool));
  SVN_TEST_ASSERT(propval != NULL);
  SVN_TEST_STRING_ASSERT(propval->data,
                         apr_psprintf(b->pool, "/%s:2-6\n/%s:4",
                                      svn_relpath_join(trunk_path,
                                                       new_dir_name,
                                                       b->pool),
                                      apr_pstrcat(b->pool,
                                                  new_dir_name, ".orig",
                                                  SVN_VA_NULL)));
  return SVN_NO_ERROR;
}

static svn_error_t *
test_option_merge_incoming_added_dir_replace(const svn_test_opts_t *opts,
                                             apr_pool_t *pool)
{
  svn_client_ctx_t *ctx;
  svn_client_conflict_t *conflict;
  const char *new_dir_path;
  svn_boolean_t text_conflicted;
  apr_array_header_t *props_conflicted;
  svn_boolean_t tree_conflicted;
  struct status_baton sb;
  struct svn_client_status_t *status;
  svn_opt_revision_t opt_rev;
  svn_test__sandbox_t *b = apr_palloc(pool, sizeof(*b));

  SVN_ERR(svn_test__sandbox_create(b, "incoming_added_dir_replace",
                                   opts, pool));

  SVN_ERR(create_wc_with_dir_add_vs_dir_add_merge_conflict(b, FALSE, FALSE,
                                                           FALSE));

  /* Resolve the tree conflict. */
  SVN_ERR(svn_test__create_client_ctx(&ctx, b, b->pool));
  new_dir_path = svn_relpath_join(branch_path, new_dir_name, b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_dir_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(svn_client_conflict_tree_get_details(conflict, b->pool));
  SVN_ERR(svn_client_conflict_tree_resolve_by_id(
            conflict,
            svn_client_conflict_option_merge_incoming_added_dir_replace,
            b->pool));

  /* Ensure that the directory has the expected status. */
  opt_rev.kind = svn_opt_revision_working;
  sb.result_pool = b->pool;
  SVN_ERR(svn_client_status6(NULL, ctx, sbox_wc_path(b, new_dir_path),
                             &opt_rev, svn_depth_unknown, TRUE, TRUE,
                             TRUE, TRUE, FALSE, TRUE, NULL,
                             status_func, &sb, b->pool));
  status = sb.status;
  SVN_TEST_ASSERT(status->kind == svn_node_dir);
  SVN_TEST_ASSERT(status->versioned);
  SVN_TEST_ASSERT(!status->conflicted);
  SVN_TEST_ASSERT(status->node_status == svn_wc_status_replaced);
  SVN_TEST_ASSERT(status->text_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->prop_status == svn_wc_status_none);
  SVN_TEST_ASSERT(status->copied);
  SVN_TEST_ASSERT(!status->switched);
  SVN_TEST_ASSERT(!status->file_external);
  SVN_TEST_ASSERT(status->moved_from_abspath == NULL);
  SVN_TEST_ASSERT(status->moved_to_abspath == NULL);

  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_dir_path),
                                  ctx, b->pool, b->pool));

  /* The directory should not be in conflict. */
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(!text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  return SVN_NO_ERROR;
}

/* This test currently fails to meet expectations. Our merge code doesn't
 * support a merge of files which were added in the same revision as their
 * parent directory and were not modified since. */
static svn_error_t *
test_option_merge_incoming_added_dir_replace_and_merge(
  const svn_test_opts_t *opts, apr_pool_t *pool)
{
  svn_client_ctx_t *ctx;
  svn_client_conflict_t *conflict;
  const char *new_dir_path;
  const char *new_file_path;
  svn_boolean_t text_conflicted;
  apr_array_header_t *props_conflicted;
  svn_boolean_t tree_conflicted;
  struct status_baton sb;
  struct svn_client_status_t *status;
  svn_opt_revision_t opt_rev;
  svn_test__sandbox_t *b = apr_palloc(pool, sizeof(*b));

  SVN_ERR(svn_test__sandbox_create(b, "incoming_added_dir_replace_and_merge",
                                   opts, pool));

  SVN_ERR(create_wc_with_dir_add_vs_dir_add_merge_conflict(b, FALSE, FALSE,
                                                           FALSE));

  /* Resolve the tree conflict. */
  SVN_ERR(svn_test__create_client_ctx(&ctx, b, b->pool));
  new_dir_path = svn_relpath_join(branch_path, new_dir_name, b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_dir_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(svn_client_conflict_tree_get_details(conflict, b->pool));
  SVN_ERR(svn_client_conflict_tree_resolve_by_id(
            conflict,
            svn_client_conflict_option_merge_incoming_added_dir_replace_and_merge,
            b->pool));

  /* Ensure that the directory has the expected status. */
  opt_rev.kind = svn_opt_revision_working;
  sb.result_pool = b->pool;
  SVN_ERR(svn_client_status6(NULL, ctx, sbox_wc_path(b, new_dir_path),
                             &opt_rev, svn_depth_unknown, TRUE, TRUE,
                             TRUE, TRUE, FALSE, TRUE, NULL,
                             status_func, &sb, b->pool));
  status = sb.status;
  SVN_TEST_ASSERT(status->kind == svn_node_dir);
  SVN_TEST_ASSERT(status->versioned);
  SVN_TEST_ASSERT(!status->conflicted);
  SVN_TEST_ASSERT(status->node_status == svn_wc_status_replaced);
  SVN_TEST_ASSERT(status->text_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->prop_status == svn_wc_status_none);
  SVN_TEST_ASSERT(status->copied);
  SVN_TEST_ASSERT(!status->switched);
  SVN_TEST_ASSERT(!status->file_external);
  SVN_TEST_ASSERT(status->moved_from_abspath == NULL);
  SVN_TEST_ASSERT(status->moved_to_abspath == NULL);

  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_dir_path),
                                  ctx, b->pool, b->pool));

  /* The directory should not be in conflict. */
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(!text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  /* We should have a text conflict in the file. */
  new_file_path = svn_relpath_join(branch_path,
                                   svn_relpath_join(new_dir_name,
                                                    new_file_name, b->pool),
                                   b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_file_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  return SVN_NO_ERROR;
}

/* Same test as above, but with an additional file change on the branch
 * which makes resolution work as expected. */
static svn_error_t *
test_option_merge_incoming_added_dir_replace_and_merge2(
  const svn_test_opts_t *opts, apr_pool_t *pool)
{
  svn_client_ctx_t *ctx;
  svn_client_conflict_t *conflict;
  const char *new_dir_path;
  const char *new_file_path;
  svn_boolean_t text_conflicted;
  apr_array_header_t *props_conflicted;
  svn_boolean_t tree_conflicted;
  struct status_baton sb;
  struct svn_client_status_t *status;
  svn_opt_revision_t opt_rev;
  svn_test__sandbox_t *b = apr_palloc(pool, sizeof(*b));

  SVN_ERR(svn_test__sandbox_create(b, "incoming_added_dir_replace_and_merge",
                                   opts, pool));

  SVN_ERR(create_wc_with_dir_add_vs_dir_add_merge_conflict(b, FALSE, FALSE,
                                                           TRUE));

  /* Resolve the tree conflict. */
  SVN_ERR(svn_test__create_client_ctx(&ctx, b, b->pool));
  new_dir_path = svn_relpath_join(branch_path, new_dir_name, b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_dir_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(svn_client_conflict_tree_get_details(conflict, b->pool));
  SVN_ERR(svn_client_conflict_tree_resolve_by_id(
            conflict,
            svn_client_conflict_option_merge_incoming_added_dir_replace_and_merge,
            b->pool));

  /* Ensure that the directory has the expected status. */
  opt_rev.kind = svn_opt_revision_working;
  sb.result_pool = b->pool;
  SVN_ERR(svn_client_status6(NULL, ctx, sbox_wc_path(b, new_dir_path),
                             &opt_rev, svn_depth_unknown, TRUE, TRUE,
                             TRUE, TRUE, FALSE, TRUE, NULL,
                             status_func, &sb, b->pool));
  status = sb.status;
  SVN_TEST_ASSERT(status->kind == svn_node_dir);
  SVN_TEST_ASSERT(status->versioned);
  SVN_TEST_ASSERT(!status->conflicted);
  SVN_TEST_ASSERT(status->node_status == svn_wc_status_replaced);
  SVN_TEST_ASSERT(status->text_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->prop_status == svn_wc_status_none);
  SVN_TEST_ASSERT(status->copied);
  SVN_TEST_ASSERT(!status->switched);
  SVN_TEST_ASSERT(!status->file_external);
  SVN_TEST_ASSERT(status->moved_from_abspath == NULL);
  SVN_TEST_ASSERT(status->moved_to_abspath == NULL);

  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_dir_path),
                                  ctx, b->pool, b->pool));

  /* The directory should not be in conflict. */
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(!text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  /* We should have a text conflict in the file. */
  new_file_path = svn_relpath_join(branch_path,
                                   svn_relpath_join(new_dir_name,
                                                    new_file_name, b->pool),
                                   b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, new_file_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  return SVN_NO_ERROR;
}

/* A helper function which prepares a working copy for the tests below. */
static svn_error_t *
create_wc_with_incoming_delete_merge_conflict(svn_test__sandbox_t *b)
{
  svn_client_ctx_t *ctx;
  static const char *trunk_url;
  svn_opt_revision_t opt_rev;
  const char *deleted_path;

  SVN_ERR(sbox_add_and_commit_greek_tree(b));

  /* Create a branch of node "A". */
  SVN_ERR(sbox_wc_copy(b, trunk_path, branch_path));
  SVN_ERR(sbox_wc_commit(b, ""));

  /* Delete a file on the trunk. */
  deleted_path = svn_relpath_join(trunk_path, deleted_file_name, b->pool);
  SVN_ERR(sbox_wc_delete(b, deleted_path));
  SVN_ERR(sbox_wc_commit(b, ""));

  /* Modify a file on the branch. */
  deleted_path = svn_relpath_join(branch_path, deleted_file_name, b->pool);
  SVN_ERR(sbox_file_write(b, deleted_path,
                          "This is a modified file on the branch\n"));
  SVN_ERR(sbox_wc_commit(b, ""));

  /* Run a merge from the trunk to the branch. */
  SVN_ERR(svn_test__create_client_ctx(&ctx, b, b->pool));

  SVN_ERR(sbox_wc_update(b, "", SVN_INVALID_REVNUM));
  trunk_url = apr_pstrcat(b->pool, b->repos_url, "/", trunk_path, SVN_VA_NULL);

  opt_rev.kind = svn_opt_revision_head;
  opt_rev.value.number = SVN_INVALID_REVNUM;
  /* This should raise an "incoming delete vs local edit" tree conflict. */
  SVN_ERR(svn_client_merge_peg5(trunk_url, NULL, &opt_rev,
                                sbox_wc_path(b, branch_path),
                                svn_depth_infinity,
                                FALSE, FALSE, FALSE, FALSE, FALSE, FALSE,
                                NULL, ctx, b->pool));

  return SVN_NO_ERROR;
}

/* Test 'incoming delete ignore' option. */
static svn_error_t *
test_option_merge_incoming_delete_ignore(
  const svn_test_opts_t *opts, apr_pool_t *pool)
{
  svn_test__sandbox_t *b = apr_palloc(pool, sizeof(*b));
  svn_client_ctx_t *ctx;
  const char *deleted_path;
  svn_client_conflict_t *conflict;
  svn_boolean_t tree_conflicted;
  svn_boolean_t text_conflicted;
  apr_array_header_t *props_conflicted;
  struct status_baton sb;
  struct svn_client_status_t *status;
  svn_opt_revision_t opt_rev;

  SVN_ERR(svn_test__sandbox_create(b, "incoming_delete_ignore",
                                   opts, pool));

  SVN_ERR(create_wc_with_incoming_delete_merge_conflict(b));

  /* Resolve the tree conflict. */
  SVN_ERR(svn_test__create_client_ctx(&ctx, b, b->pool));
  deleted_path = svn_relpath_join(branch_path, deleted_file_name, b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, deleted_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(svn_client_conflict_tree_get_details(conflict, b->pool));
  SVN_ERR(svn_client_conflict_tree_resolve_by_id(
            conflict, svn_client_conflict_option_incoming_delete_ignore,
            b->pool));

  /* Ensure that the deleted file has the expected status. */
  opt_rev.kind = svn_opt_revision_working;
  sb.result_pool = b->pool;
  SVN_ERR(svn_client_status6(NULL, ctx, sbox_wc_path(b, deleted_path),
                             &opt_rev, svn_depth_unknown, TRUE, TRUE,
                             TRUE, TRUE, FALSE, TRUE, NULL,
                             status_func, &sb, b->pool));
  status = sb.status;
  SVN_TEST_ASSERT(status->kind == svn_node_file);
  SVN_TEST_ASSERT(status->versioned);
  SVN_TEST_ASSERT(!status->conflicted);
  SVN_TEST_ASSERT(status->node_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->text_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->prop_status == svn_wc_status_none);
  SVN_TEST_ASSERT(!status->copied);
  SVN_TEST_ASSERT(!status->switched);
  SVN_TEST_ASSERT(!status->file_external);
  SVN_TEST_ASSERT(status->moved_from_abspath == NULL);
  SVN_TEST_ASSERT(status->moved_to_abspath == NULL);
  
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, deleted_path),
                                  ctx, b->pool, b->pool));

  /* The file should not be in conflict. */
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(!text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  return SVN_NO_ERROR;
}

/* Test 'incoming delete accept' option. */
static svn_error_t *
test_option_merge_incoming_delete_accept(
  const svn_test_opts_t *opts, apr_pool_t *pool)
{
  svn_test__sandbox_t *b = apr_palloc(pool, sizeof(*b));
  svn_client_ctx_t *ctx;
  const char *deleted_path;
  svn_client_conflict_t *conflict;
  svn_boolean_t tree_conflicted;
  svn_boolean_t text_conflicted;
  apr_array_header_t *props_conflicted;
  struct status_baton sb;
  struct svn_client_status_t *status;
  svn_opt_revision_t opt_rev;

  SVN_ERR(svn_test__sandbox_create(b, "incoming_delete_accept",
                                   opts, pool));

  SVN_ERR(create_wc_with_incoming_delete_merge_conflict(b));

  /* Resolve the tree conflict. */
  SVN_ERR(svn_test__create_client_ctx(&ctx, b, b->pool));
  deleted_path = svn_relpath_join(branch_path, deleted_file_name, b->pool);
  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, deleted_path),
                                  ctx, b->pool, b->pool));
  SVN_ERR(svn_client_conflict_tree_get_details(conflict, b->pool));
  SVN_ERR(svn_client_conflict_tree_resolve_by_id(
            conflict, svn_client_conflict_option_incoming_delete_accept,
            b->pool));

  /* Ensure that the deleted file has the expected status. */
  opt_rev.kind = svn_opt_revision_working;
  sb.result_pool = b->pool;
  SVN_ERR(svn_client_status6(NULL, ctx, sbox_wc_path(b, deleted_path),
                             &opt_rev, svn_depth_unknown, TRUE, TRUE,
                             TRUE, TRUE, FALSE, TRUE, NULL,
                             status_func, &sb, b->pool));
  status = sb.status;
  SVN_TEST_ASSERT(status->kind == svn_node_file);
  SVN_TEST_ASSERT(status->versioned);
  SVN_TEST_ASSERT(!status->conflicted);
  SVN_TEST_ASSERT(status->node_status == svn_wc_status_deleted);
  SVN_TEST_ASSERT(status->text_status == svn_wc_status_normal);
  SVN_TEST_ASSERT(status->prop_status == svn_wc_status_none);
  SVN_TEST_ASSERT(!status->copied);
  SVN_TEST_ASSERT(!status->switched);
  SVN_TEST_ASSERT(!status->file_external);
  SVN_TEST_ASSERT(status->moved_from_abspath == NULL);
  SVN_TEST_ASSERT(status->moved_to_abspath == NULL);

  SVN_ERR(svn_client_conflict_get(&conflict, sbox_wc_path(b, deleted_path),
                                  ctx, b->pool, b->pool));

  /* The file should not be in conflict. */
  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted,
                                             &props_conflicted,
                                             &tree_conflicted,
                                             conflict, b->pool, b->pool));
  SVN_TEST_ASSERT(!text_conflicted &&
                  props_conflicted->nelts == 0 &&
                  !tree_conflicted);

  return SVN_NO_ERROR;
}

/* ========================================================================== */


static int max_threads = 1;

static struct svn_test_descriptor_t test_funcs[] =
  {
    SVN_TEST_NULL,
    SVN_TEST_OPTS_PASS(test_option_merge_incoming_added_file_ignore,
                       "test incoming add file ignore"),
    SVN_TEST_OPTS_PASS(test_option_merge_incoming_added_file_text_merge,
                       "test incoming add file text merge"),
    SVN_TEST_OPTS_PASS(test_option_merge_incoming_added_file_replace,
                       "test incoming add file replace"),
    SVN_TEST_OPTS_PASS(test_option_merge_incoming_added_file_replace_and_merge,
                       "test incoming add file replace and merge"),
    SVN_TEST_OPTS_PASS(test_option_merge_incoming_added_dir_ignore,
                       "test incoming add dir ignore"),
    SVN_TEST_OPTS_XFAIL(test_option_merge_incoming_added_dir_merge,
                       "test incoming add dir merge"),
    SVN_TEST_OPTS_PASS(test_option_merge_incoming_added_dir_merge2,
                       "test incoming add dir merge with file change"),
    SVN_TEST_OPTS_XFAIL(test_option_merge_incoming_added_dir_merge3,
                       "test incoming add dir merge with move history"),
    SVN_TEST_OPTS_PASS(test_option_merge_incoming_added_dir_replace,
                       "test incoming add dir replace"),
    SVN_TEST_OPTS_XFAIL(test_option_merge_incoming_added_dir_replace_and_merge,
                       "test incoming add dir replace and merge"),
    SVN_TEST_OPTS_PASS(test_option_merge_incoming_added_dir_replace_and_merge2,
                       "test incoming add dir replace and merge"),
    SVN_TEST_OPTS_PASS(test_option_merge_incoming_delete_ignore,
                       "test merge incoming delete ignore"),
    SVN_TEST_OPTS_PASS(test_option_merge_incoming_delete_accept,
                       "test merge incoming delete accept"),
    SVN_TEST_NULL
  };

SVN_TEST_MAIN
