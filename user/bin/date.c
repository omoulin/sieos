/* date - print the current date and time (UTC) */
#include "sieos.h"

int main(void)
{
    static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    struct tm tm;
    time_t now_t = time(NULL);
    gmtime_r(&now_t, &tm);
    printf("%s %s %2d %02d:%02d:%02d UTC %d\n", days[tm.tm_wday], months[tm.tm_mon],
           tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_year + 1900);
    return 0;
}
