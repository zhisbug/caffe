#ifndef ZMQ_MASTER_H
#define ZMQ_MASTER_H

#include <thread>
#include "zmq_common.hpp"

namespace ps{

class PSMaster{
public:
    PSMaster(std::string my_addr, int expected_servers) : 
        my_addr_(my_addr), servers_(0){
        LOG(INFO) << "Master Addr: \"" << my_addr << "\"";
        LOG(INFO) << "Expected Servers: " << expected_servers;
        int expected_clients = expected_servers;
        LOG(INFO) << "Expected Clients: " << expected_servers;
        master_ = std::shared_ptr<ZMQServer>(new ZMQServer(my_addr_));
        dealer_ = std::shared_ptr<std::thread>(new std::thread([this, expected_servers, expected_clients]
            {
                while(1){
                    Comm::Header request;
                    std::string id;
                    master_->Recv(&id, &request);
                    CHECK(request.has_ch());
                    const Comm::CtrlHeader *ch = &request.ch();
                    if (ch->role() == Comm::CtrlHeader::SERVER){//server
                        LOG(INFO) << "Received Server Register";
                        if (ch->op() == Comm::CtrlHeader::ADD){//add
                            CHECK(ch->addr_size() == 1);
                            if (to_servers_.find(ch->addr(0)) == to_servers_.end()){
                                to_servers_[ch->addr(0)] = std::shared_ptr<ZMQClient>(
                                        new ZMQClient(ch->addr(0)));
                                servers_addr_.push_back(ch->addr(0));
                                LOG(INFO) << "Server[" << servers_ << "]("
                                               << ch->addr(0) 
                                               << ") registered successfully";
			        Comm::Header reply;
				reply.mutable_ch()->set_role(Comm::CtrlHeader::SERVER);
				reply.mutable_ch()->set_op(Comm::CtrlHeader::ADD);
				reply.mutable_ch()->set_num_client(expected_clients);
			        master_->Send(id, reply);
				servers_++;
                                if (servers_ == expected_servers){
                                    LOG(INFO) << "All expected servers registered";
                                }else if (servers_ > expected_servers){
				    LOG(FATAL) << "Too many servers";
				}
                            }else{
                                LOG(FATAL) << "Existing server(" << ch->addr(0) 
                                          << ") already registered";
                            }
                        }
                    }else{//worker
                        if(ch->op() == Comm::CtrlHeader::ASK_INIT){
                            Comm::Header reply;
			    reply.mutable_ch()->set_role(Comm::CtrlHeader::WORKER);
			    if(servers_ == expected_servers)
			        reply.mutable_ch()->set_op(Comm::CtrlHeader::GO);
			    else if(servers_ < expected_servers)
			        reply.mutable_ch()->set_op(Comm::CtrlHeader::WAIT);
			    else
			        LOG(FATAL) << "Too many servers.";
                            master_->Send(id, reply);
                        }else if (ch->op() == Comm::CtrlHeader::QUERY_SERVER){
                            //for worker to server communication
                            CHECK(ch->has_key());
                            LOG(INFO) << id << " Sender Query the Addr of Server for Key " << ch->key();
                            Comm::Header reply;
                            Comm::CtrlHeader *reply_ch = reply.mutable_ch();
                            reply_ch->set_role(Comm::CtrlHeader::WORKER);
                            reply_ch->set_op(Comm::CtrlHeader::QUERY_SERVER);
                            reply_ch->add_addr(servers_addr_[ch->key()%servers_addr_.size()]);
                            LOG(INFO) << "Replied Addr = " << reply_ch->addr(0);
                            master_->Send(id, reply);//respond to sender
                            to_workers_.push_back(id);
                        }else if (ch->op() == Comm::CtrlHeader::TERMINATE){
                            Comm::Header reply;
                            Comm::CtrlHeader *reply_ch = reply.mutable_ch();
                            reply_ch->set_role(Comm::CtrlHeader::SERVER);
                            reply_ch->set_op(Comm::CtrlHeader::TERMINATE);
                            for (const auto &i : to_servers_){
                                LOG(INFO) << "Sending terminate message to " << i.first;
                                i.second->Send(reply); 
                            }
                            servers_addr_.clear();
                            to_servers_.clear();
                            reply_ch->set_role(Comm::CtrlHeader::WORKER);
                            for (const auto &id : to_workers_){
                                LOG(INFO) << "Sending terminate message to " << id;
                                master_->Send(id, reply);
                            }
                            to_workers_.clear();
                            servers_ = 0;
                        }else{
                            LOG(FATAL) << "Not Recognize.";
                        }
                    }
                }
            }), [](std::thread *t){t->join();});
    }
    void Run(){
        dealer_->join(); 
    }
    
private:
    std::shared_ptr<ZMQServer> master_;
    std::shared_ptr<std::thread> dealer_;
    std::vector<std::string> servers_addr_;
    std::unordered_map<std::string, std::shared_ptr<ZMQClient>> to_servers_;
    //std::vector<std::string> workers_addr_;
    //std::vector<std::shared_ptr<ZMQClient>> to_workers_;
    std::vector<std::string> to_workers_;
    // std::unordered_map<std::string, std::shared_ptr<ZMQClient>> to_workers_;
    int servers_;
    std::string my_addr_;
};

}

#endif
