#include <mapreduce/yt/tests/lib/lib.h>

#include <mapreduce/yt/interface/client.h>

////////////////////////////////////////////////////////////////////////////////

int main(int argc, const char* argv[])
{
    NYT::Initialize(argc, argv);
    return NYT::NTest::Main(argc, argv);
}

////////////////////////////////////////////////////////////////////////////////

