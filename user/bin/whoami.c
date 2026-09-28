/* whoami - print the effective user name */
#include "sieos.h"

int main(void)
{
    struct passwd *pw = getpwuid(geteuid());
    if (pw)
        printf("%s\n", pw->pw_name);
    else
        printf("%d\n", geteuid());
    return 0;
}
