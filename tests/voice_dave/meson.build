dave_test = executable('voice_dave_test', '../voice_dave_test.cpp', dependencies: dave_dep)
test('voice_dave', dave_test)
if get_option('voice_crypto_only')
  encryption_test = executable('voice_encryption_test', '../voice_encryption_test.cpp', dependencies: dave_dep)
  test('voice_encryption', encryption_test)
endif
reference_dep = dependency('libdave', required: get_option('dave_reference'))
if reference_dep.found()
  reference_test = executable('voice_dave_reference_test', '../voice_dave_reference_test.cpp', dependencies: [dave_dep, reference_dep])
  test('voice_dave_reference', reference_test)
endif
