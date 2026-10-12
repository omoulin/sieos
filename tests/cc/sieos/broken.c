/* broken.c - a compile error on purpose (tools/cctest.py: the message names line 4) */
int main(void)
{
    return undeclared_thing;
}
