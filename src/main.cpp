#include "core/logging.hpp"

int main() {
    babo::log::init();

    spdlog::info("babo_exchange up");
    return 0;
}
