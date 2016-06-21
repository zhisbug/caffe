#ifndef PS_CLIENT_H
#define PS_CLIENT_H

#include <vector>
#include <thread>
#include "zmq_common.hpp"

namespace ps{

using std::string;
using std::shared_ptr;
using std::thread;
using std::vector;

template <typename T>
class Worker{
public:
    Worker(int key, int bytes, T* data, T* diff, string master_addr) :
        data_(data), diff_(diff)
    {
        LOG(INFO) << "key: " << key << "\tbytes: " << bytes;
        Comm::DataHeader *dh = header_.mutable_dh();
        dh->set_key(key);
        dh->set_type(DataHeaderTypeWrapper<T>());
        dh->set_length(bytes);

        LOG(INFO) << "Register at master " << master_addr;
        to_master_.reset(new ZMQClient(master_addr));
        Comm::Header header4master;
        Comm::CtrlHeader *ctrl4master = header4master.mutable_ch();
        ctrl4master->set_role(Comm::CtrlHeader::WORKER);
        ctrl4master->set_op(Comm::CtrlHeader::QUERY_SERVER);
        ctrl4master->set_key(dh->key());
        to_master_->Send(header4master);
        to_master_->Recv(&header4master);
        CHECK(header4master.ch().addr_size() == 1);

        LOG(INFO) << "Register at server " << header4master.ch().addr(0);
        client_.reset(new ZMQClient(header4master.ch().addr(0))); 
        Comm::Header header4server;
        Comm::CtrlHeader *ctrl4server = header4server.mutable_ch();
        ctrl4server->set_role(Comm::CtrlHeader::WORKER);
        ctrl4server->set_op(Comm::CtrlHeader::ADD);
        ctrl4server->set_key(dh->key());
        client_->Send(header4server);
    }

    void InitWithPS(int client_id){
        inited_ = true;
        client_id_ = client_id;
        Comm::DataHeader *dh = header_.mutable_dh();

        if(client_id == 0){
            dh->set_iter(0);
            dh->set_is_init(true);
            client_->Send(header_, data_);
            dh->set_is_init(false);
        }

        Comm::Header h;
        client_->Recv(&h, (void**)&data_);
        dh->set_iter(h.dh().iter());
    }

    void Push(){
        CHECK(inited_);
        client_->Send(header_, diff_);
    }

    inline void IncIter() {
        CHECK(inited_);
        Comm::DataHeader *dh = header_.mutable_dh();
        dh->set_iter(header_.dh().iter()+1);
    }

    void Pull(){
        CHECK(inited_);
        Comm::Header h;
        client_->Recv(&h, (void**)&data_);
        CHECK(h.dh().iter() == header_.dh().iter());
    }

    inline int iter() { return header_.dh().iter(); }
    inline T* data() { return data_; }
    inline T* diff() { return diff_; }

    void Terminate(){
        Comm::Header header4master;
        Comm::CtrlHeader *ctrl4master = header4master.mutable_ch();
        ctrl4master->set_role(Comm::CtrlHeader::WORKER);
        ctrl4master->set_op(Comm::CtrlHeader::TERMINATE);
        to_master_->Send(header4master);
    }

private:
    Comm::Header header_;
    shared_ptr<ZMQClient> to_master_;
    shared_ptr<ZMQClient> client_;
    T* data_;
    T* diff_;
    int client_id_ = -1;
    bool inited_ = false;
private:
    template <typename Type>
    Comm::DataHeader::DataType DataHeaderTypeWrapper(){
        if (std::is_same<Type, float>:: value){
            return Comm::DataHeader::FLOAT; 
        }else if (std::is_same<Type, double>::value){
            return Comm::DataHeader::DOUBLE;
        }else if (std::is_same<Type, int>::value){
            return Comm::DataHeader::INT32; 
        }else if (std::is_same<Type, char>::value){
            return Comm::DataHeader::BYTE; 
        }
    }
};

}

#endif
