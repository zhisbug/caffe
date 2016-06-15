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
class Send{
public:
    Send(string m, int k) :
        master_(m), key_(k), iter_(0){
        //key_ = k;
        Comm::DataHeader *dh = header_.mutable_dh();
        dh->set_key(key_);
        dh->set_type(DataHeaderTypeWrapper<T>());

        to_master_.reset(new ZMQClient(master_));
        Comm::Header header4master;
        Comm::CtrlHeader *ctrl4master = header4master.mutable_ch();
        ctrl4master->set_role(Comm::CtrlHeader::WORKER);
        ctrl4master->set_op(Comm::CtrlHeader::QUERY_SERVER);
        ctrl4master->set_key(key_);
        to_master_->Send(header4master);
        to_master_->Recv(&header4master);
        LOG(INFO) << "master: " << master_;
        CHECK(header4master.ch().addr_size() == 1);
        LOG(INFO) << "server: " << header4master.ch().addr(0);

        client_.reset(new ZMQClient(header4master.ch().addr(0))); 
    }

    bool InitRun(const void *buf, size_t bytes){
        Comm::DataHeader *dh = header_.mutable_dh();
        dh->set_length(bytes);
        dh->set_iter(iter_++);
        dh->set_is_init(true);
        client_->Send(header_, buf);
        return true; 
    }

    bool Run(const void *buf, size_t bytes){
        Comm::DataHeader *dh = header_.mutable_dh();
        dh->set_length(bytes);
        dh->set_iter(iter_++);
        client_->Send(header_, buf);
        return true; 
    }

    bool Terminate(){
        Comm::Header header4master;
        Comm::CtrlHeader *ctrl4master = header4master.mutable_ch();
        ctrl4master->set_role(Comm::CtrlHeader::WORKER);
        ctrl4master->set_op(Comm::CtrlHeader::TERMINATE);
        to_master_->Send(header4master);
        return true;
    }
private:
    shared_ptr<ZMQClient> client_;
    shared_ptr<ZMQClient> to_master_;
    string master_;
    int key_, iter_;
    Comm::Header header_;
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

template <typename T>
class Broadcast{
public:
    Broadcast(string m, int k) :
        master_(m), key_(k), iter_(0){
        Comm::DataHeader *dh = header_.mutable_dh();
        dh->set_key(key_);
        dh->set_type(DataHeaderTypeWrapper<T>());
        to_master_.reset(new ZMQClient(master_));
    }

    bool Run(const void *buf, size_t bytes){
        Comm::Header header4master;
        Comm::CtrlHeader *ctrl4master = header4master.mutable_ch();
        ctrl4master->set_role(Comm::CtrlHeader::WORKER);
        ctrl4master->set_op(Comm::CtrlHeader::QUERY_WORKERS);
        to_master_->Send(header4master);
        to_master_->Recv(&header4master);
        LOG(INFO) << "master: " << master_;
        CHECK(header4master.ch().addr_size() >= 1);
        int ibegin = to_workers_.size();
        for (int i = ibegin; i < header4master.ch().addr_size(); i++){
            LOG(INFO) << "workers[" << i << "]:\t" << header4master.ch().addr(i);
            workers_addr_.push_back(header4master.ch().addr(i));
            to_workers_.emplace_back(std::shared_ptr<ZMQClient>(
                        new ZMQClient(header4master.ch().addr(i))));
        }

        Comm::DataHeader *dh = header_.mutable_dh();
        dh->set_length(bytes);
        dh->set_iter(iter_++);
        for (int i = 0; i < to_workers_.size(); i++){
            to_workers_[i]->Send(header_, buf); 
        }
        //client_->Send(header_, buf);
        return true; 
    }

    bool Terminate(){
        Comm::Header header4master;
        Comm::CtrlHeader *ctrl4master = header4master.mutable_ch();
        ctrl4master->set_role(Comm::CtrlHeader::WORKER);
        ctrl4master->set_op(Comm::CtrlHeader::TERMINATE);
        to_master_->Send(header4master);

	return true;
    }

private:
    shared_ptr<ZMQClient> to_master_;
    vector<shared_ptr<ZMQClient>> to_workers_;
    vector<string> workers_addr_;
    string master_;
    int key_, iter_;
    Comm::Header header_;
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

template <typename T>
class IRecvAll{
public:
    IRecvAll(string me, string master) :
        my_addr_(me), master_addr_(master){
        server_.reset(new ZMQServer(my_addr_));
        to_master_.reset(new ZMQClient(master));
        Comm::Header header4master;
        Comm::CtrlHeader *ctrl4master = header4master.mutable_ch();
        ctrl4master->set_role(Comm::CtrlHeader::WORKER);
        ctrl4master->set_op(Comm::CtrlHeader::ADD);
        //ctrl4master->set_addr(my_addr_);
        ctrl4master->add_addr(my_addr_);
        to_master_->Send(header4master);
        to_master_->Recv(&header4master);
        LOG(INFO) << "master: " << master;

        recver_.reset(new thread([this]
            {
                while(1){
                    string id;
                    Comm::Header header;
                    char* buf = NULL;
                    server_->Recv(&id, &header, (void**)&buf);
                    if (header.has_ch()){
                        CHECK(header.ch().role() == Comm::CtrlHeader::WORKER);
                        if (header.ch().op() == Comm::CtrlHeader::TERMINATE){
                            break; 
                        }
                    }
                    CHECK(header.has_dh());
                    // LOG(INFO) << "Received key[" << header.dh().key() << "]";
                    // LOG(INFO) << "Received value[" << *((float*)buf) << "]";
                    shared_ptr<char> ptr(buf, [](char *p){delete p;});
                    ThreadSafeHashTable<char>::get_table()->insert(header.dh().key(), ptr);
                }
            }), [](std::thread *t){t->join();});
    }
    bool Run(){
        return true;
    }

private:
    shared_ptr<ZMQServer> server_;
    shared_ptr<ZMQClient> to_master_;
    shared_ptr<thread> recver_;
    string my_addr_, master_addr_;
    int iter_;
};

template <typename T>
class Wait{
public:
    Wait(vector<int> &&k){keys_ = k;}
    Wait(vector<int> &k){keys_ = k;}

    bool Run(void *buf, size_t bytes, int i){
        shared_ptr<char> value = 
            ThreadSafeHashTable<char>::get_table()->wait_and_get(keys_[i]);
        memcpy(buf, value.get(), bytes);

        // float* ptr = (float*) value.get();
        // LOG(INFO) << "Received " << i << " " << *ptr << *(ptr+1);

        return true;
    }
private:
    vector<int> keys_;
};

}

#endif
