# How to run unit test
## Run test one by one
### build
```bash
$ mkdir build && cd build
$ cmake .. -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local -DBUILD_UNIT_TESTS=1
```

### run test
```bash
$ ./test/unit_test/template-test-case/templateTest
```

## Run all test with measuring coverage
### build and run test
```bash
$ cmake .. -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local -DBUILD_UNIT_TESTS=1 -DCOVERAGE=1
$ ./run-ut-coverage.sh
```

### Open html files.
(ex: use firefox)

```bash
$ firefox lcovHtml/index.html
```
