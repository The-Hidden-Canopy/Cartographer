#include <cartographer/carto.h>

int main(void) {
    if (carto_abi_version() != 1U) {
        return 1;
    }

    carto_context* context = 0;
    carto_error error = {0, 0};
    if (carto_context_create(0, &context, &error) != CARTO_OK || context == 0) {
        carto_free(error.message);
        return 2;
    }
    carto_context_destroy(context);
    return 0;
}
