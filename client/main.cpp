#include "Client.h"

int main() {
    Client c("127.0.0.1", 4444);
    c.run();
    return 0;
}
