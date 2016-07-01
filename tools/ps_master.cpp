#include "caffe/ps/ps_master.hpp"
#include "caffe/ps/zmq_common.hpp"



using namespace ps;
int main(int argc, char **argv){
    CHECK(argc == 3); 
    PSMaster *master = new PSMaster(argv[1], atoi(argv[2]));
    master->Run();
    return 0;
}
