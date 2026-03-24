/* Helper to access cff_driver_class without name collision with ESP-IDF */
#include <ft2build.h>
#include FT_MODULE_H

extern const FT_Module_Class cff_driver_class;

void* get_cff_driver_class(void) {
    return (void*)&cff_driver_class;
}
