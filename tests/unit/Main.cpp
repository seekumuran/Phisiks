// PhisiksUnitTests: headless tests, run by ctest. Optional argument: substring filter.
#include "TestFramework.h"

int main(int argc, char** argv) {
    return PhisiksTest::RunAll(argc > 1 ? argv[1] : nullptr);
}
