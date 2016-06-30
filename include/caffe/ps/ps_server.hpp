#ifndef ZMQ_SERVER_H
#define ZMQ_SERVER_H

#include <thread>
#include "zmq_common.hpp"
#include <bitset>
#include "caffe/util/benchmark.hpp"

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
                    //LOG(INFO) << "Receive a DataHeader? " << header->has_dh();
                    if (header->has_ch()){
                        header->mutable_ch()->set_id(id);
                    }
                    LOG(INFO) << std::bitset<4>(id[0]) << std::bitset<4>(id[1])
                        << std::bitset<4>(id[2]) << std::bitset<4>(id[4]) << std::bitset<4>(id[5]);
      
                    
                    if (header->dh().has_key())
                        if (header->dh().key() < 2)
                            LOG(INFO) << "recv " << header->dh().key() << " at " << header->dh().iter();

                    MD_PAIR value;
                    value = std::make_pair(
                        std::shared_ptr<Comm::Header>(header, 
                            [](Comm::Header *dh){delete dh;}),
                        std::shared_ptr<char>((char*)buf, [](char *p){delete[] p;}));
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
                    MD_PAIR one_pair = queue_.wait_and_pop();
                    std::shared_ptr<Comm::Header> header = one_pair.first;

                    if(header->has_ch()){
                        std::string id = header->ch().id();
                        if(header->ch().op() == Comm::CtrlHeader::ADD){
                            CHECK(header->ch().role() == Comm::CtrlHeader::WORKER);
                            int k = header->ch().key();
                            LOG(INFO) << "Registering " << id << " with key[" << k << "]";
                            to_workers_[k].push_back(id);
                            if(is_init_.find(k) == is_init_.end())
                                is_init_[k] = false;
                            continue;
                        }else if(header->ch().op() == Comm::CtrlHeader::TERMINATE){
                            LOG(INFO) << "Consumer Terminating...";
                            break; 
                        }else{
                            LOG(FATAL) << "Not recognized.";
                        }
                    }

                    CHECK(header->has_dh());
                    int k = header->dh().key();
                    std::shared_ptr<char> buf = one_pair.second;
                    
                    if(header->dh().is_init()){
                        CHECK(is_init_[k] == false);
                        LOG(INFO) << "Initing id " << k;
                        is_init_[k] = true;
                        kv_iter_[k] = header->dh().iter();

                        accumulate_wrapper(header.get(), buf.get());

                        num_workers_[k] = to_workers_[k].size();
                        header->mutable_dh()->set_num_worker(num_workers_[k]);
                        for(auto& id : to_workers_[k]){
                            server_->Send(id, *header, kv_pair_[k].get());
                        }
                    }else{
                        CHECK(header->dh().iter() == kv_iter_[k]);
                        CHECK(num_workers_[k] = to_workers_[k].size());
                        
                        kv_count_[k]++;

                        if(header->dh().svb_length_size()){
                            header->mutable_dh()->set_iter(kv_iter_[k]+1);
                            for(auto& id : to_workers_[k])
                                server_->Send(id, *header, buf.get());
                            if(kv_count_[k] == num_workers_[k]){
                                kv_count_[k] = 0;
                                kv_iter_[k]++;
                            }
                        }else{
                            accumulate_wrapper(header.get(), buf.get());

                            if(kv_count_[k] == num_workers_[k]){
                                kv_count_[k] = 0;
                                kv_iter_[k]++;
                                header->mutable_dh()->set_iter(kv_iter_[k]);
                                for(auto& id : to_workers_[k])
                                    server_->Send(id, *header, kv_pair_[k].get());
                                if(k < 2)
                                    LOG(INFO) << "send " << k << " at " << kv_iter_[k];
                            }
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
    void accumulate_wrapper(Comm::Header* header, void* buf){
        switch (header->dh().type()){
        case (Comm::DataHeader::FLOAT):
            accumulate<float>(header->dh().key(), 
                reinterpret_cast<float*>(buf), header->dh().length());
            break;
        case (Comm::DataHeader::DOUBLE):
            accumulate<double>(header->dh().key(), 
                reinterpret_cast<double*>(buf), header->dh().length());
            break;
        case (Comm::DataHeader::INT32):
            accumulate<int>(header->dh().key(), 
                reinterpret_cast<int*>(buf), header->dh().length());
            break;
        }
    }
    template <typename T>
    void accumulate(int key, T *delta, int length){
        if (kv_pair_.find(key) == kv_pair_.end()){
            kv_pair_[key] = 
                std::shared_ptr<char>(new char[length], [](char *p){delete[] p;});
            memset(kv_pair_[key].get(), 0, length);
        }
        T *para = reinterpret_cast<T*>(kv_pair_[key].get());
        for (int i = 0; i < length/sizeof(T); i++){
            para[i] += delta[i];
        }
    }

private:
    std::string my_addr_, master_addr_;
    std::shared_ptr<ZMQServer> server_;
    std::shared_ptr<ZMQClient> to_master_;

    std::unordered_map<int, std::vector<std::string> > to_workers_;
    std::unordered_map<int, int> num_workers_; // number of ACTIVE workers

    std::unordered_map<int, bool> is_init_;
    std::unordered_map<int, std::shared_ptr<char> > kv_pair_;
    std::unordered_map<int, int> kv_count_;
    std::unordered_map<int, int> kv_iter_;

    ThreadSafeQueue<MD_PAIR> queue_;
    std::shared_ptr<std::thread> producer_;
    std::shared_ptr<std::thread> consumer_;
};

}

#endif
