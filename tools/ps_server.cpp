#include <iostream>
#include "caffe/ps/ps_server.hpp"
#include "caffe/ps/zmq_common.hpp"

using namespace ps;

int main(int argc, char **argv){
    CHECK(argc == 3);
    PSServer *server = new PSServer(argv[1], argv[2]); 
    server->Run();
    delete server;
    return 0;
}
