#ifndef ZMQ_SERVER_H
#define ZMQ_SERVER_H

#include <thread>
#include "zmq_common.hpp"

namespace ps{

typedef std::pair<std::shared_ptr<Comm::Header>, std::shared_ptr<char>> MD_PAIR;

class PSServer{
public:
    PSServer(std::string my_addr, std::string master_addr): 
        my_addr_(my_addr), master_addr_(master_addr){
        LOG(INFO) << "Server Addr: \"" << my_addr << "\"";
        LOG(INFO) << "Master Addr: \"" << master_addr << "\"";
        server_ = std::shared_ptr<ZMQServer>(new ZMQServer(my_addr_));

        LOG(INFO) << "Connecting Master...";
        to_master_.reset(new ZMQClient(master_addr_));
        Comm::Header header4master;
        Comm::CtrlHeader *ctrl4master = header4master.mutable_ch();
        ctrl4master->set_role(Comm::CtrlHeader::SERVER);
        ctrl4master->set_op(Comm::CtrlHeader::ADD);
        ctrl4master->add_addr(my_addr_);
        to_master_->Send(header4master);
        LOG(INFO) << "Master connected...";

        LOG(INFO) << "Launching producer and consumer...";
        producer_ = std::shared_ptr<std::thread>(new std::thread([this]
            {
                while(1){
                    Comm::Header *header = new Comm::Header();
                    std::string id;
                    void *buf = NULL;
                    //LOG(INFO) << "Procuder Receiving";
                    server_->Recv(&id, header, &buf);
                    LOG(INFO) << "Receive a DataHeader? " << header->has_dh();
                    if (header->has_ch()){
                        header->mutable_ch()->set_id(id);
                    }
                    MD_PAIR value;
                    value = std::make_pair(
                        std::shared_ptr<Comm::Header>(header, 
                            [](Comm::Header *dh){delete dh;}),
                        std::shared_ptr<char>((char*)buf, [](char *p){delete p;}));
                    queue_.push(value);

                    if (header->has_ch() &&
                        header->ch().op() == Comm::CtrlHeader::TERMINATE){
                        LOG(INFO) << "Producer Terminating...";
                        break; 
                    }
                }
            }));
        consumer_ = std::shared_ptr<std::thread>(new std::thread([this]
            {
                while(1){
                    //LOG(INFO) << "Consumer Waiting";
                    MD_PAIR one_pair = queue_.wait_and_pop();
                    std::shared_ptr<Comm::Header> header = one_pair.first;
                    if (header->has_ch()){
                        CHECK(header->ch().role() == Comm::CtrlHeader::SERVER);
                        if (header->ch().op() == Comm::CtrlHeader::ADD){
                            CHECK(header->ch().addr_size() == 1);
                            CHECK(header->ch().has_id());
                            server_->Send(header->ch().id(), *header);
                            //const std::string cid = std::to_string(to_workers_.size());
                            if (to_workers_.find(header->ch().addr(0)) == to_workers_.end()){
                                to_workers_[header->ch().addr(0)] = 
                                    std::shared_ptr<ZMQClient>(new ZMQClient(header->ch().addr(0))); 
                            }
                            LOG(INFO) << "Issue a CtrlHeader\t"
                                           << "(adding worker\"" << header->ch().addr(0)
                                           << "\")";
                            continue;
                        }else if (header->ch().op() == Comm::CtrlHeader::TERMINATE){
                            LOG(INFO) << "Consumer Terminating...";
                            break; 
                        }
                    }
                    CHECK(header->has_dh());
                    LOG(INFO) << "Issue a DataHeader with Key[" 
                                   << header->dh().key() << "]";
                    std::shared_ptr<char> buf = one_pair.second;
                    switch (header->dh().type()){
                    case (Comm::DataHeader::FLOAT):
                        accumulate<float>(header->dh().key(), 
                            reinterpret_cast<float*>(buf.get()), header->dh().length());
                        break;
                    case (Comm::DataHeader::DOUBLE):
                        accumulate<double>(header->dh().key(), 
                            reinterpret_cast<double*>(buf.get()), header->dh().length());
                        break;
                    case (Comm::DataHeader::INT32):
                        accumulate<int>(header->dh().key(), 
                            reinterpret_cast<int*>(buf.get()), header->dh().length());
                        break;
                    }
                    // if (kv_count_.find(header->dh().key()) == kv_count_.end()) 
                    //     kv_count_[header->dh().key()] = 0;
		    CHECK(!to_workers_.empty());
                    if (!header->dh().is_init()){
		        ++kv_count_[header->dh().key()];
		    }
                    if (header->dh().is_init() ||
			kv_count_[header->dh().key()] % to_workers_.size() == 0){
                        LOG(INFO) << "Sending KEY[" << header->dh().key() << "]";
                        for (auto &i : to_workers_){
                            i.second->Send(*header, kv_pair_[header->dh().key()].get()); 
                        }
                    }                
		}
            }));
    }
    void Run(){
        producer_->join(); 
        consumer_->join();
    }
private:
    template <typename T>
    void accumulate(int key, T *delta, int length){
        if (kv_pair_.find(key) == kv_pair_.end()){
            kv_pair_[key] = 
                std::shared_ptr<char>(new char[length], [](char *p){delete p;});
            memset(kv_pair_[key].get(), 0, length);
        }
        T *para = reinterpret_cast<T*>(kv_pair_[key].get());
        for (int i = 0; i < length/sizeof(T); i++)
            para[i] += delta[i];
    }
private:
    std::shared_ptr<ZMQServer> server_;
    std::shared_ptr<ZMQClient> to_master_;
    std::unordered_map<std::string, std::shared_ptr<ZMQClient>> to_workers_;
    ThreadSafeQueue<MD_PAIR> queue_;
    std::unordered_map<int, std::shared_ptr<char>> kv_pair_;
    std::unordered_map<int, int> kv_count_;
    std::shared_ptr<std::thread> producer_;
    std::shared_ptr<std::thread> consumer_;
    std::string my_addr_, master_addr_;
};

}

#endif
