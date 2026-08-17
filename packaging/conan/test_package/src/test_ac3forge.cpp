#include <print>

#include "ac3/version.hpp"

int main() {
    std::println("{}", ac3::version_details());
    return 0;
}
