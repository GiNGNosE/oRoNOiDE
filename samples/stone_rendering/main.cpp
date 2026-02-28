#include "core/app.h"

int main() {
    App app;
    if (!app.init(1280, 720, "oRoNOiDE - Stone Rendering")) {
        return 1;
    }
    app.run();
    app.shutdown();
    return 0;
}