#include "transit_tracker/app.hpp"
#include "transit_tracker/runtime.hpp"

int main() {
    auto state = transit_tracker::createProductionState();
    auto app = transit_tracker::createApp(state, transit_tracker::createProductionDependencies(state));
    app->port(8080).multithreaded().run();
    return 0;
}
