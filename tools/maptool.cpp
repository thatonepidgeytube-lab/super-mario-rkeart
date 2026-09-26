// smk-map: ROM map helper (stub).
//
// Placeholder for the address-map tool referenced by the Makefile's
// $(BIN)/smk-map rule.  It intentionally does nothing yet; it exists so the
// build rule resolves to a real source file.
#include <cstdio>

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    std::printf("smk-map: not implemented yet (stub)\n");
    return 0;
}
