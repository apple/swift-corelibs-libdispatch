## Testing libdispatch

### Running tests

A C-based test suite can be found in the tests subdirectory.
It uses the automake testing harness to execute the tests.

A default set of tests that are always expected to pass can
be executed by doing

   ```
   make check
   ```

An extended test suite that includes some tests that may fail
occasionally can be enabled at configure time:

   ```
   ./configure --enable-extended-test-suite
   make check
   ```

### Additional prerequisites

A few test cases require additional packages to be installed.
In particular, several IO tests assume /usr/bin/vi is available
as an input file and will fail if it is not present.
