/*
 * @TITLE@ - made with MiR (Make it Real) on SIEOS.
 * A terminal program in C++: it reads standard input and writes standard output.
 */
#include <iostream>
#include <string>

int main()
{
    std::string line;
    int n = 0;
    while (std::getline(std::cin, line))
        std::cout << ++n << ": " << line << '\n';
    std::cout << n << " lines\n";
    return 0;
}
