#include "../../examples/custom_type/type.hpp"
int main() {
    coresql::Registry registry;
    example::codes::install(registry);
    auto value = example::codes::value("AB");
    registry.validate(value, example::codes::type());
    return example::codes::string(value) == "AB" ? 0 : 1;
}
