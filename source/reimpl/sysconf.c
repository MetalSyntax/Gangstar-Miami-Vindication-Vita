#include <unistd.h>

long sysconf_soloader(int name) {
    if (name == _SC_PAGESIZE || name == 40) { // _SC_PAGESIZE is 40 on Android
        return 4096;
    }
    return 0;
}
