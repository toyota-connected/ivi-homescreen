# How to run unit test
## Run test one by one
### build
```bash
$ mkdir build && cd build
build$ cmake .. -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local -DBUILD_UNIT_TESTS=1 -DCMAKE_BUILD_TYPE=Debug
```

### run all test
```bash
build$ ctest
```

### run test per test case
```bash
build$ ./test/unit_test/template-test-case/templateTest
```

## Run all test with measuring coverage
### build and run test
```bash
build$ cmake .. -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local -DBUILD_UNIT_TESTS=1 -DCOVERAGE=1 -DCMAKE_BUILD_TYPE=Debug
build$ ./run-ut-coverage.sh
```

### Open html files.
(ex: use firefox)

```bash
build$ firefox lcovHtml/index.html
```

# Writing tests
## Headless Backend
Testing requires BUILD_BACKEND_HEADLESS compile option, which uses OSMesa to render to an offscren buffer.  This buffer can then be written to a file for comparison to a known-good result.

## Flutter App 
For unit tests that generate/compare images, a flutter app bundle must be provided via the UNIT_TEST_APP_BUNDLE option.  

## Image Comparisons
To test the output of rendering functions, it may be necessary to compare the test output to a previously taken image that is known to be correct.  To obtain these images, run cmake with the options BUILD_UNIT_TEST and UNIT_TEST_SAVE_GOLDENS set to true.  In the implemention of the unit test, when this flag is set, the output image should just be saved to the build/test/unit_test/test-images-golden directory.  Manually inspect the output images to verify they are correct.  It is also recommended to ensure that the test fails afterwards, in case this setting is ever set inadvertently.  When UNIT_TEST_SAVE_GOLDENS is set to false, then the test shall create the image and then compare to the one in this directory.  See test_case_app_headless.cc for an example of this procedure.

