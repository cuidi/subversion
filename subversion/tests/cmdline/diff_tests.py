from svntest.verify import make_diff_header, make_no_diff_deleted_header, \
                           make_diff_header, make_no_diff_deleted_header, \
                           make_git_diff_header, make_diff_prop_header, \
                           make_diff_prop_val, make_diff_prop_deleted, \
                           make_diff_prop_added, make_diff_prop_modified
def diff_switched_file(sbox):
  "diff a switched file against repository"

  sbox.build()
  svntest.actions.run_and_verify_svn(None, None, [], 'switch',
                                     sbox.repo_url + '/A/mu',
                                     sbox.ospath('iota'), '--ignore-ancestry')
  sbox.simple_append('iota', 'Mu????')

  # This diffs the file against its origin
  expected_output = [
    'Index: %s\n' % sbox.path('iota'),
    '===================================================================\n',
    '--- %s\t(.../A/mu)\t(revision 1)\n' % sbox.path('iota'),
    '+++ %s\t(.../iota)\t(working copy)\n' % sbox.path('iota'),
    '@@ -1 +1,2 @@\n',
    ' This is the file \'mu\'.\n',
    '+Mu????\n',
    '\ No newline at end of file\n',
  ]
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'diff', '-r', '1', sbox.ospath('iota'))

  # And this undoes the switch for the diff
  expected_output = [
    'Index: %s\n' % sbox.path('iota'),
    '===================================================================\n',
    '--- %s\t(revision 1)\n' % sbox.path('iota'),
    '+++ %s\t(working copy)\n' % sbox.path('iota'),
    '@@ -1 +1,2 @@\n',
    '-This is the file \'iota\'.\n',
    '+This is the file \'mu\'.\n',
    '+Mu????\n',
    '\ No newline at end of file\n',
  ]
  svntest.actions.run_and_verify_svn(None, expected_output, [],
                                     'diff', '-r', '1', sbox.ospath(''))


              diff_switched_file,