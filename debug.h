#ifndef ____DEBUG
#define ____DEBUG

#include <iostream>

#define COUT (std::cout << __FILE__ << ':' << __LINE__ << "  [" << __PRETTY_FUNCTION__ << ']' << '\t')

#endif
