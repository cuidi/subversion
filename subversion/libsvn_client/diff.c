#define DIFF_REVNUM_NONEXISTENT ((svn_revnum_t) -100)
  if (revnum >= 0)
  else if (revnum == DIFF_REVNUM_NONEXISTENT)
    label = apr_psprintf(pool, _("%s\t(nonexistent)"), path);
  else /* SVN_INVALID_REVNUM */
                                 DIFF_REVNUM_NONEXISTENT,
                                 DIFF_REVNUM_NONEXISTENT,
                               copyfrom_source ? copyfrom_source->revision
                                               : DIFF_REVNUM_NONEXISTENT,
                                     DIFF_REVNUM_NONEXISTENT,
                                     DIFF_REVNUM_NONEXISTENT,
  apr_hash_t *left_props;
  if (dwi->no_diff_added)
    return SVN_NO_ERROR;

  if (copyfrom_source && !dwi->show_copies_as_adds)
    {
      left_props = copyfrom_props ? copyfrom_props
                                  : apr_hash_make(scratch_pool);
    }
  else
    {
      left_props = apr_hash_make(scratch_pool);
      copyfrom_source = NULL;
    }

  SVN_ERR(svn_prop_diffs(&prop_changes, right_props, left_props,
                                            DIFF_REVNUM_NONEXISTENT,
                                            left_props,
  diff_writer_info_t *dwi = processor->baton;
  apr_array_header_t *prop_changes;

  if (dwi->no_diff_deleted)
    return SVN_NO_ERROR;


  SVN_ERR(svn_prop_diffs(&prop_changes, apr_hash_make(scratch_pool),
                         left_props, scratch_pool));

  SVN_ERR(diff_props_changed(relpath,
                             left_source->revision,
                             DIFF_REVNUM_NONEXISTENT,
                             prop_changes,
                             left_props,
                             TRUE /* show_diff_header */,
                             dwi,
                             scratch_pool));