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
