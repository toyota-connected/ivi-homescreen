#!/bin/bash

make

# reset coverage execuion count to zero
lcov -d . -z

# run all test drivers
for driver in `find test/unit_test/ -name '*_test_driver'`; do
  ./${driver}
done

# calculate test coverage
lcov -d . -c --rc lcov_branch_coverage=1 -o coverage.info
lcov -r coverage.info '*/test/*' '*/usr/*' --rc lcov_branch_coverage=1 -o coverage.info

# generate html file
genhtml -o lcovHtml -s --legend --branch-coverage coverage.info

