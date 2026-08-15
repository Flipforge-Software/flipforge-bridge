.PHONY: test clean-test

test:
	cmake -S tests -B tests/build
	cmake --build tests/build
	ctest --test-dir tests/build --output-on-failure

clean-test:
	cmake -E remove_directory tests/build
