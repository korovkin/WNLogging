#include "WNLogging.h"
#include <gflags/gflags.h>

int main(int argc, char** argv)
{
    google::ParseCommandLineFlags(&argc, &argv, true);

    LOG(INFO) << "hello!";
    LOG(INFO) << "it works!";

    CHECK(true) << "what?!";
    CHECK_EQ(42, 84/2) << "no way!";

    return EXIT_SUCCESS;
}
